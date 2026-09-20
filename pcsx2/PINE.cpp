// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "BuildVersion.h"
#include "Common.h"
#include "Host.h"
#include "Elfheader.h"
#include "SaveState.h"
#include "PINE.h"
#include "VMManager.h"
#ifdef ENABLE_VR
#include "Counters.h"
#include "MTGS.h"
#include "VR/DepthHistogram.h"
#include "fmt/format.h"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#endif
#include "vtlb.h"
#include "common/Error.h"
#include "common/Threading.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <sys/types.h>
#include <thread>

#include "fmt/format.h"

#if defined(_WIN32)
#define read_portable(a, b, c) (recv(a, (char*)b, c, 0))
#define write_portable(a, b, c) (send(a, (const char*)b, c, 0))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			closesocket((a)); \
			(a) = INVALID_SOCKET; \
		} \
	} while (0)
#include "common/RedtapeWindows.h"
#include <WinSock2.h>
#elif defined(__linux__) || defined(__FreeBSD__)
#define read_portable(a, b, c) (read(a, b, c))
#define write_portable(a, b, c) (send(a, b, c, MSG_NOSIGNAL))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			close((a)); \
			(a) = -1; \
		} \
	} while (0)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#else
#define read_portable(a, b, c) (read(a, b, c))
#define write_portable(a, b, c) (write(a, b, c))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			close((a)); \
			(a) = -1; \
		} \
	} while (0)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define PINE_EMULATOR_NAME "pcsx2"

#ifdef _WIN32

static bool InitializeWinsock()
{
	static bool initialized = false;
	if (initialized)
		return true;

	WSADATA wsa = {};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;

	initialized = true;
	std::atexit([]() { WSACleanup(); });
	return true;
}

#endif

namespace PINEServer
{
	static std::thread s_thread;
	static int s_slot;

#ifdef _WIN32
	static SOCKET s_sock = INVALID_SOCKET;
	static SOCKET s_msgsock = INVALID_SOCKET;
#else
	static std::string s_socket_name;
	static int s_sock = -1;
	static int s_msgsock = -1;
#endif

	static std::atomic_bool s_end{true};

#define MAX_IPC_SIZE 650000

#define MAX_IPC_RETURN_SIZE 450000

	static std::vector<u8> s_ret_buffer;

	static std::vector<u8> s_ipc_buffer;

	enum IPCCommand : unsigned char
	{
		MsgRead8 = 0,
		MsgRead16 = 1,
		MsgRead32 = 2,
		MsgRead64 = 3,
		MsgWrite8 = 4,
		MsgWrite16 = 5,
		MsgWrite32 = 6,
		MsgWrite64 = 7,
		MsgVersion = 8,
		MsgSaveState = 9,
		MsgLoadState = 0xA,
		MsgTitle = 0xB,
		MsgID = 0xC,
		MsgUUID = 0xD,
		MsgGameVersion = 0xE,
		MsgStatus = 0xF,
		MsgVRQhistFlush = 0xE0,
		MsgUnimplemented = 0xFF
	};

	enum EmuStatus : uint32_t
	{
		Running = 0,
		Paused = 1,
		Shutdown = 2
	};

	struct IPCBuffer
	{
		int size;
		std::vector<u8> buffer;
	};

	enum IPCResult : unsigned char
	{
		IPC_OK = 0,
		IPC_FAIL = 0xFF
	};

	void MainLoop();
	void ClientLoop();

	static IPCBuffer ParseCommand(std::span<u8> buf, std::vector<u8>& ret_buffer, u32 buf_size);

	static std::vector<u8>& MakeOkIPC(std::vector<u8>& ret_buffer, uint32_t size);
	static std::vector<u8>& MakeFailIPC(std::vector<u8>& ret_buffer, uint32_t size);

	bool AcceptClient();

	template <typename T>
	static void ToResultVector(std::vector<u8>& res_vector, T res, int i)
	{
		memcpy(&res_vector[i], (char*)&res, sizeof(T));
	}

	template <typename T>
	static T FromSpan(std::span<u8> span, int i)
	{
		return *(T*)(&span[i]);
	}

	static inline bool SafetyChecks(u32 command_len, int command_size, u32 reply_len, int reply_size = 0, u32 buf_size = MAX_IPC_SIZE - 1)
	{
		return !((command_len + command_size) > buf_size ||
				 (reply_len + reply_size) >= MAX_IPC_RETURN_SIZE);
	}
}

bool PINEServer::Initialize(int slot)
{
	s_end.store(false, std::memory_order_release);
	s_slot = slot;

#ifdef _WIN32
	if (!InitializeWinsock())
	{
		Console.WriteLn(Color_Red, "PINE: Cannot initialize winsock! Shutting down...");
		Deinitialize();
		return false;
	}

	s_sock = socket(AF_INET, SOCK_STREAM, 0);
	if ((s_sock == INVALID_SOCKET) || slot > 65536)
	{
		Console.WriteLn(Color_Red, "PINE: Cannot open socket! Shutting down...");
		Deinitialize();
		return false;
	}

	sockaddr_in server = {};
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	server.sin_port = htons(slot);

	if (bind(s_sock, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
	{
		Console.WriteLn(Color_Red, "PINE: Error while binding to socket! Shutting down...");
		Deinitialize();
		return false;
	}

#else
	char* runtime_dir = nullptr;
#ifdef __APPLE__
	runtime_dir = std::getenv("TMPDIR");
#else
	runtime_dir = std::getenv("XDG_RUNTIME_DIR");
#endif
	if (runtime_dir == nullptr)
		s_socket_name = "/tmp/" PINE_EMULATOR_NAME ".sock";
	else
	{
		s_socket_name = runtime_dir;
		s_socket_name += "/" PINE_EMULATOR_NAME ".sock";
	}

	if (slot != PINE_DEFAULT_SLOT)
		s_socket_name += "." + std::to_string(slot);

	struct sockaddr_un server;

	s_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s_sock < 0)
	{
		Console.WriteLn(Color_Red, "PINE: Cannot open socket! Shutting down...");
		Deinitialize();
		return false;
	}
	server.sun_family = AF_UNIX;
	StringUtil::Strlcpy(server.sun_path, s_socket_name, sizeof(server.sun_path));

	unlink(s_socket_name.c_str());
	if (bind(s_sock, (struct sockaddr*)&server, sizeof(struct sockaddr_un)))
	{
		Console.WriteLn(Color_Red, "PINE: Error while binding to socket! Shutting down...");
		Deinitialize();
		return false;
	}
#endif

	if (listen(s_sock, 4096))
	{
		Console.WriteLn(Color_Red, "PINE: Cannot listen for connections! Shutting down...");
		Deinitialize();
		return false;
	}

	s_ret_buffer.resize(MAX_IPC_RETURN_SIZE);
	s_ipc_buffer.resize(MAX_IPC_SIZE);

	s_thread = std::thread(&PINEServer::MainLoop);

	return true;
}

bool PINEServer::IsInitialized()
{
	return !s_end.load(std::memory_order_acquire);
}

int PINEServer::GetSlot()
{
	return s_slot;
}

std::vector<u8>& PINEServer::MakeOkIPC(std::vector<u8>& ret_buffer, uint32_t size = 5)
{
	ToResultVector<uint32_t>(ret_buffer, size, 0);
	ret_buffer[4] = IPC_OK;
	return ret_buffer;
}

std::vector<u8>& PINEServer::MakeFailIPC(std::vector<u8>& ret_buffer, uint32_t size = 5)
{
	ToResultVector<uint32_t>(ret_buffer, size, 0);
	ret_buffer[4] = IPC_FAIL;
	return ret_buffer;
}

bool PINEServer::AcceptClient()
{
	s_msgsock = accept(s_sock, 0, 0);
	if (s_msgsock < 0)
	{
#ifdef _WIN32
		const int errno_w = WSAGetLastError();
		if (!(errno_w == WSAECONNRESET || errno_w == WSAEINTR || errno_w == WSAEINPROGRESS || errno_w == WSAEMFILE || errno_w == WSAEWOULDBLOCK) && s_sock != INVALID_SOCKET)
			Console.Error("PINE: accept() returned error %d", errno_w);
#else
		if (!(errno == ECONNABORTED || errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) && s_sock >= 0)
			Console.Error("PINE: accept() returned error %d", errno);
#endif

		return false;
	}

#ifdef __APPLE__
	int nosigpipe = 1;
	setsockopt(s_msgsock, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

	Console.WriteLn("PINE: New client with FD %d connected.", (int)s_msgsock);
	return true;
}

void PINEServer::MainLoop()
{
	Threading::SetNameOfCurrentThread("PINE Server");

	while (!s_end.load(std::memory_order_acquire))
	{
		if (!AcceptClient())
			continue;

		ClientLoop();

		Console.WriteLn("PINE: Client disconnected.");
		safe_close_portable(s_msgsock);
	}
}

void PINEServer::ClientLoop()
{
	while (!s_end.load(std::memory_order_acquire))
	{
		auto receive_length = 0;
		auto end_length = 4;
		const std::span<u8> ipc_buffer_span(s_ipc_buffer);

		while (receive_length < end_length)
		{
			const auto tmp_length = read_portable(s_msgsock, &ipc_buffer_span[receive_length], MAX_IPC_SIZE - receive_length);

			if (tmp_length <= 0)
				return;

			receive_length += tmp_length;

			if (end_length == 4 && receive_length >= 4)
			{
				end_length = FromSpan<u32>(ipc_buffer_span, 0);
				if (end_length > MAX_IPC_SIZE || end_length < 4)
				{
					receive_length = 0;
					break;
				}
			}
		}
		PINEServer::IPCBuffer res;

		if (receive_length != 0)
		{
			res = ParseCommand(ipc_buffer_span.subspan(4), s_ret_buffer, (u32)end_length - 4);

			if (write_portable(s_msgsock, res.buffer.data(), res.size) < 0)
				return;
		}
	}
}

void PINEServer::Deinitialize()
{
	s_end.store(true, std::memory_order_release);

#ifndef _WIN32
	if (!s_socket_name.empty())
	{
		unlink(s_socket_name.c_str());
		s_socket_name = {};
	}
#endif

#ifdef _WIN32
	if (s_sock != INVALID_SOCKET)
		shutdown(s_sock, SD_BOTH);
#else
	if (s_sock >= 0)
		shutdown(s_sock, SHUT_RDWR);
#endif

	safe_close_portable(s_sock);
	safe_close_portable(s_msgsock);

	if (s_thread.joinable())
		s_thread.join();
}

PINEServer::IPCBuffer PINEServer::ParseCommand(std::span<u8> buf, std::vector<u8>& ret_buffer, u32 buf_size)
{
	u32 ret_cnt = 5;
	u32 buf_cnt = 0;

	while (buf_cnt < buf_size)
	{
		if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
			return IPCBuffer{5, MakeFailIPC(ret_buffer)};
		buf_cnt++;
		switch ((IPCCommand)buf[buf_cnt - 1])
		{
			case MsgRead8:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 1, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u8 res = vtlb_ramRead<mem8_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 1;
				buf_cnt += 4;
				break;
			}
			case MsgRead16:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 2, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u16 res = vtlb_ramRead<mem16_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 2;
				buf_cnt += 4;
				break;
			}
			case MsgRead32:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 4, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u32 res = vtlb_ramRead<mem32_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 4;
				buf_cnt += 4;
				break;
			}
			case MsgRead64:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 8, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u64 res = vtlb_ramRead<mem64_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 8;
				buf_cnt += 4;
				break;
			}
			case MsgWrite8:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem8_t>(a, FromSpan<u8>(buf, buf_cnt + 4));
				buf_cnt += 5;
				break;
			}
			case MsgWrite16:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 2 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem16_t>(a, FromSpan<u16>(buf, buf_cnt + 4));
				buf_cnt += 6;
				break;
			}
			case MsgWrite32:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem32_t>(a, FromSpan<u32>(buf, buf_cnt + 4));
				buf_cnt += 8;
				break;
			}
			case MsgWrite64:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 8 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem64_t>(a, FromSpan<u64>(buf, buf_cnt + 4));
				buf_cnt += 12;
				break;
			}
#ifdef ENABLE_VR
			case MsgVRQhistFlush:
			{
				if (!VMManager::HasValidVM() || !VR::DepthHistogramArmed() || VR::QhistLiveDir().empty())
				{
					Console.Warning("(PINE) MsgVRQhistFlush refused: %s",
						!VMManager::HasValidVM() ? "no VM" :
							(!VR::DepthHistogramArmed() ? "histogram not armed (launch with --qhist-live <dir>)" :
															"no live dir"));
					goto error;
				}
				struct FlushState
				{
					std::mutex m;
					std::condition_variable cv;
					bool done = false;
					bool ok = false;
					std::string path;
					std::string err;
				};
				auto st = std::make_shared<FlushState>();
				Host::RunOnCPUThread([st]() {
					MTGS::RunOnGSThread([st]() {
					static std::atomic<u32> s_seq{0};
					VR::DepthHistogram& h = VR::GlobalDepthHistogram();
					h.key.serial = VMManager::GetDiscSerial();
					h.key.crc = fmt::format("0x{:08x}", VMManager::GetDiscCRC());
					h.key.dump = std::string();
					h.key.frame = g_FrameCount;
					h.key.widescreen_hack = EmuConfig.EnableWideScreenPatches;
					const u32 seq = s_seq.fetch_add(1) + 1;
					const std::string path = fmt::format("{}/qhist-live-{}-{:03d}-f{}.json",
						VR::QhistLiveDir(), h.key.serial.empty() ? "unknown" : h.key.serial, seq, h.key.frame);
					std::string err;
					const bool ok = h.WriteJson(path, &err);
					if (ok)
						h.Reset();
					std::lock_guard<std::mutex> lock(st->m);
					st->ok = ok;
					st->path = path;
					st->err = err;
					st->done = true;
					st->cv.notify_one();
					});
				});
				{
					std::unique_lock<std::mutex> lock(st->m);
					if (!st->cv.wait_for(lock, std::chrono::seconds(10), [&st] { return st->done; }))
					{
						Console.Warning("(PINE) MsgVRQhistFlush: GS thread did not service the flush in 10s (VM paused?)");
						goto error;
					}
					if (!st->ok)
					{
						Console.ErrorFmt("(PINE) MsgVRQhistFlush: {}", st->err);
						goto error;
					}
				}
				const u32 size = static_cast<u32>(st->path.size()) + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], st->path.c_str(), size);
				ret_cnt += size;
				break;
			}
#endif
			case MsgVersion:
			{
				u32 size = strlen(BuildVersion::GitRev) + 7;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				snprintf(reinterpret_cast<char*>(&ret_buffer[ret_cnt]), size, "PCSX2 %s", BuildVersion::GitRev);
				ret_cnt += size;
				break;
			}
			case MsgSaveState:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				Host::RunOnCPUThread([slot = FromSpan<u8>(buf, buf_cnt)] {
					VMManager::SaveStateToSlot(slot, true, [slot](const std::string& error) {
						SaveState_ReportSaveErrorOSD(error, slot);
					});
				});
				buf_cnt += 1;
				break;
			}
			case MsgLoadState:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				Host::RunOnCPUThread([slot = FromSpan<u8>(buf, buf_cnt)] {
					Error state_error;
					if (!VMManager::LoadStateFromSlot(slot, false, &state_error))
						SaveState_ReportLoadErrorOSD(state_error.GetDescription(), slot, false);
				});
				buf_cnt += 1;
				break;
			}
			case MsgTitle:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string gameName = VMManager::GetTitle(false);
				const u32 size = gameName.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], gameName.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgID:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string gameSerial = VMManager::GetDiscSerial();
				const u32 size = gameSerial.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], gameSerial.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgUUID:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string crc = fmt::format("{:08x}", VMManager::GetDiscCRC());
				const u32 size = crc.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], crc.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgGameVersion:
			{
				if (!VMManager::HasValidVM())
					goto error;

				const std::string ElfVersion = VMManager::GetDiscVersion();
				const u32 size = ElfVersion.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], ElfVersion.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgStatus:
			{
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, 4, buf_size)) [[unlikely]]
					goto error;
				EmuStatus status;

				switch (VMManager::GetState())
				{
					case VMState::Running:
						status = EmuStatus::Running;
						break;
					case VMState::Paused:
						status = EmuStatus::Paused;
						break;
					default:
						status = EmuStatus::Shutdown;
						break;
				}

				ToResultVector(ret_buffer, status, ret_cnt);
				ret_cnt += 4;
				break;
			}
			default:
			{
			error:
				return IPCBuffer{5, MakeFailIPC(ret_buffer)};
			}
		}
	}
	return IPCBuffer{(int)ret_cnt, MakeOkIPC(ret_buffer, ret_cnt)};
}

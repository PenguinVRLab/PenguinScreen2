// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/CameraDriver.h"
#include "VR/HeadPose.h"
#include "VR/DisplayFit.h"
#include "VR/ExecHook.h"
#include "VR/PadLook.h"
#include "VR/VRManager.h"
#include "VR/SplitState.h"
#include "VR/GuestRam.h"
#include "VR/GuestPatch.h"
#include "VR/VRProfileDB.h"
#include "VR/WorldProjection.h"

#include "Config.h"
#include "Host.h"
#include "Memory.h"
#include "VMManager.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace VR::CameraDriver
{
	namespace
	{
		constexpr float PI_F = 3.14159265358979323846f;

		struct EulerAngles
		{
			float yaw = 0.0f;
			float pitch = 0.0f;
			float roll = 0.0f;
		};

		EulerAngles QuaternionToEulerYXZ(float x, float y, float z, float w)
		{
			const float n2 = x * x + y * y + z * z + w * w;
			if (!std::isfinite(n2) || n2 < 1e-12f)
				return EulerAngles{};
			const float inv = 1.0f / std::sqrt(n2);
			x *= inv;
			y *= inv;
			z *= inv;
			w *= inv;

			EulerAngles e;

			const float sinp = 2.0f * (w * x - y * z);
			if (std::abs(sinp) >= 1.0f)
				e.pitch = std::copysign(PI_F * 0.5f, sinp);
			else
				e.pitch = std::asin(sinp);

			e.yaw = std::atan2(2.0f * (w * y + x * z), 1.0f - 2.0f * (x * x + y * y));

			e.roll = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (x * x + z * z));

			return e;
		}

		float SelectSource(ProfileDB::CameraSource src, const EulerAngles& e, const HeadPose::Snapshot& pose)
		{
			switch (src)
			{
				case ProfileDB::CameraSource::HeadYaw:   return e.yaw;
				case ProfileDB::CameraSource::HeadPitch: return e.pitch;
				case ProfileDB::CameraSource::HeadRoll:  return e.roll;
				case ProfileDB::CameraSource::HeadX:     return pose.position_x;
				case ProfileDB::CameraSource::HeadY:     return pose.position_y;
				case ProfileDB::CameraSource::HeadZ:     return pose.position_z;
				case ProfileDB::CameraSource::Constant:  return 0.0f;
				default:                                 return 0.0f;
			}
		}

		void EncodeAndWrite(u32 address, float value, ProfileDB::CameraEncoding enc)
		{
			EEMemoryInterface ee;
			GuestRam::EncodeAndWrite(ee, address, value, enc);
		}

		std::optional<float> DecodeGuestValue(u32 address, ProfileDB::CameraEncoding enc)
		{
			EEMemoryInterface ee;
			return GuestRam::DecodeGuestValue(ee, address, enc);
		}

		float Wrap360(float v)
		{
			v = std::fmod(v, 360.0f);
			return (v < 0.0f) ? v + 360.0f : v;
		}

		constexpr u32 kMipsNop = 0x00000000u;
		constexpr u32 MipsLui(u32 rt, u32 imm16) { return 0x3C000000u | ((rt & 31u) << 16) | (imm16 & 0xFFFFu); }
		constexpr u32 MipsLwc1(u32 ft, u32 off16, u32 base) { return 0xC4000000u | ((base & 31u) << 21) | ((ft & 31u) << 16) | (off16 & 0xFFFFu); }
		constexpr u32 MipsAddS(u32 fd, u32 fs, u32 ft) { return 0x46000000u | ((ft & 31u) << 16) | ((fs & 31u) << 11) | ((fd & 31u) << 6); }
		constexpr u32 MipsJ(u32 target) { return 0x08000000u | ((target >> 2) & 0x03FFFFFFu); }
		constexpr u32 MipsJal(u32 target) { return 0x0C000000u | ((target >> 2) & 0x03FFFFFFu); }

		struct AssembledHook
		{
			u32 cave[5];
			u32 trampoline;
		};

		AssembledHook AssembleHook(const ProfileDB::CameraCodeHook& h)
		{
			const u32 hi = (h.scratch_address + 0x8000u) >> 16;
			const u32 lo = h.scratch_address & 0xFFFFu;
			AssembledHook a;
			a.cave[0] = MipsLui(h.addr_gpr, hi);
			a.cave[1] = MipsLwc1(h.scratch_fpr, lo, h.addr_gpr);
			a.cave[2] = MipsAddS(h.target_fpr, h.target_fpr, h.scratch_fpr);
			a.cave[3] = MipsJ(h.tail_jump_address);
			a.cave[4] = kMipsNop;
			a.trampoline = MipsJal(h.cave_address);
			return a;
		}

		struct Mat3
		{
			float m[9];
		};

		Mat3 Mat3Mul(const Mat3& a, const Mat3& b)
		{
			Mat3 o{};
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
				{
					float s = 0.0f;
					for (int k = 0; k < 3; k++)
						s += a.m[i * 3 + k] * b.m[k * 3 + j];
					o.m[i * 3 + j] = s;
				}
			return o;
		}

		Mat3 Mat3Transpose(const Mat3& a)
		{
			Mat3 o{};
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					o.m[i * 3 + j] = a.m[j * 3 + i];
			return o;
		}

		Mat3 Mat3FromEulerYXZ(float yaw, float pitch, float roll)
		{
			const float cy = std::cos(yaw), sy = std::sin(yaw);
			const float cx = std::cos(pitch), sx = std::sin(pitch);
			const float cz = std::cos(roll), sz = std::sin(roll);
			const Mat3 Ry{{cy, 0, sy, 0, 1, 0, -sy, 0, cy}};
			const Mat3 Rx{{1, 0, 0, 0, cx, -sx, 0, sx, cx}};
			const Mat3 Rz{{cz, -sz, 0, sz, cz, 0, 0, 0, 1}};
			return Mat3Mul(Mat3Mul(Ry, Rx), Rz);
		}

		std::optional<Mat3> ReadGuestMat3(u32 addr4x4)
		{
			Mat3 g{};
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
				{
					const float v = std::bit_cast<float>(static_cast<u32>(memRead32(addr4x4 + (r * 4 + c) * 4)));
					if (!std::isfinite(v))
						return std::nullopt;
					g.m[r * 3 + c] = v;
				}
			return g;
		}

		void WriteGuestMat3(u32 addr4x4, const Mat3& g)
		{
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					memWrite32(addr4x4 + (r * 4 + c) * 4, std::bit_cast<u32>(g.m[r * 3 + c]));
		}


		u32 s_delta_crc = 0;
		std::vector<float> s_delta_prev;
		std::vector<bool> s_delta_has;
		std::vector<float> s_delta_total;
		std::vector<float> s_delta_written;
		std::vector<bool> s_delta_written_has;
		u32 s_base_now = 0;
		u64 s_base_now_vsync = 0;
		void ResetDeltaTotals()
		{
			std::fill(s_delta_total.begin(), s_delta_total.end(), 0.0f);
		}

		struct AnchorState
		{
			float base = 0.0f;
			float last_written = 0.0f;
			float head_ref = 0.0f;
			float clamp_min = -std::numeric_limits<float>::infinity();
			float clamp_max = std::numeric_limits<float>::infinity();
			bool wrap360 = false;
			u32 addr = 0;
			ProfileDB::CameraEncoding encoding = ProfileDB::CameraEncoding::F32;
			bool anchored = false;
			bool has = false;
		};
		std::vector<AnchorState> s_anchor;

		void RestoreAnchoredScalar(AnchorState& a)
		{
			if (!a.has || !a.anchored)
			{
				a.has = false;
				return;
			}
			a.has = false;
			const std::optional<float> guest = DecodeGuestValue(a.addr, a.encoding);
			if (guest.has_value() && guest.value() == a.last_written)
			{
				float orig = a.base + a.head_ref;
				if (a.wrap360)
					orig = Wrap360(orig);
				orig = std::clamp(orig, a.clamp_min, a.clamp_max);
				EncodeAndWrite(a.addr, orig, a.encoding);
			}
			else
				DevCon.WriteLn("(VR) CameraDriver: anchored op at 0x%08X not restored — guest value is no longer ours (game wrote it).", a.addr);
		}

		using MatBaseline = MatrixHeadBaseline;
		u32 s_mat_crc = 0;
		std::vector<MatBaseline> s_mat_prev;

		struct MatAnchor
		{
			float base[9] = {};
			float written[9] = {};
			u32 addr = 0;
			bool has = false;
		};
		std::vector<MatAnchor> s_mat_anchor;

		void ResetDeltaState(bool keep_anchors = false)
		{
			std::fill(s_delta_has.begin(), s_delta_has.end(), false);
			std::fill(s_delta_written_has.begin(), s_delta_written_has.end(), false);
			for (MatBaseline& b : s_mat_prev)
				b.Reset();
			if (keep_anchors)
				return;
			for (MatAnchor& m : s_mat_anchor)
				m.has = false;
			for (AnchorState& a : s_anchor)
				a.has = false;
		}

		void EvaluateAndWrite(const ProfileDB::CameraWriteOp& op, size_t op_index, u32 address,
			const EulerAngles& e, const HeadPose::Snapshot& pose)
		{
			const float src = SelectSource(op.source, e, pose);
			float v;
			if (op.compose == ProfileDB::CameraCompose::Anchored)
			{
				if (op_index >= s_anchor.size())
					return;
				AnchorState& a = s_anchor[op_index];
				if (a.has && a.addr != address)
					RestoreAnchoredScalar(a);
				const std::optional<float> game = DecodeGuestValue(address, op.encoding);
				if (!game.has_value())
					return;
				const float head_term = src * op.axis_sign * op.scale;
				if (!a.has)
				{
					a.head_ref = op.anchor_head ? head_term : 0.0f;
					a.base = game.value() - a.head_ref;
					a.has = true;
				}
				else
				{
					float own = game.value() - a.last_written;
					if (op.wrap == ProfileDB::CameraWrap::Deg360)
						own = std::remainder(own, 360.0f);
					a.base += own;
				}
				v = a.base + head_term;
				if (op.wrap == ProfileDB::CameraWrap::Deg360)
				{
					v = Wrap360(v);
					a.base = std::remainder(a.base, 360.0f);
				}
				v = std::clamp(v, op.clamp_min, op.clamp_max);
				EncodeAndWrite(address, v, op.encoding);
				a.last_written = DecodeGuestValue(address, op.encoding).value_or(v);
				a.addr = address;
				a.encoding = op.encoding;
				a.clamp_min = op.clamp_min;
				a.clamp_max = op.clamp_max;
				a.wrap360 = (op.wrap == ProfileDB::CameraWrap::Deg360);
				a.anchored = true;
				return;
			}
			if (op.compose == ProfileDB::CameraCompose::Delta)
			{
				if (!s_delta_has[op_index])
				{
					s_delta_prev[op_index] = src;
					s_delta_has[op_index] = true;
					return;
				}
				float d = src - s_delta_prev[op_index];
				s_delta_prev[op_index] = src;
				if (op.source == ProfileDB::CameraSource::HeadYaw ||
					op.source == ProfileDB::CameraSource::HeadPitch ||
					op.source == ProfileDB::CameraSource::HeadRoll)
				{
					d = std::remainder(d, 2.0f * PI_F);
				}
				const std::optional<float> game = DecodeGuestValue(address, op.encoding);
				if (!game.has_value())
					return;
				const bool hold = op.head_only_projected && s_delta_written_has[op_index] && VR::WorldProjection::EeEnabled();
				const DeltaStep step = ComposeDelta(game.value(), hold ? &s_delta_written[op_index] : nullptr,
					d * op.axis_sign * op.scale, op.wrap == ProfileDB::CameraWrap::Deg360, op.clamp_min, op.clamp_max);
				s_delta_total[op_index] += step.applied;
				s_delta_written[op_index] = step.value;
				s_delta_written_has[op_index] = true;
				EncodeAndWrite(address, step.value, op.encoding);
				return;
			}
			else
			{
				v = src * op.axis_sign * op.scale + op.bias;
			}
			if (op.wrap == ProfileDB::CameraWrap::Deg360)
				v = Wrap360(v);
			v = std::clamp(v, op.clamp_min, op.clamp_max);
			EncodeAndWrite(address, v, op.encoding);
		}

		void ApplyMatrixOp(const ProfileDB::CameraMatrixOp& op, size_t op_index, u32 address,
			u32 transpose_address, const EulerAngles& e)
		{
			if (op.anchored)
			{
				const std::optional<Mat3> guest = ReadGuestMat3(address);
				if (!guest.has_value())
					return;

				MatAnchor& an = s_mat_anchor[op_index];
				Mat3 base = guest.value();
				if (an.has)
				{
					bool is_ours = true;
					for (int i = 0; i < 9 && is_ours; i++)
						is_ours = std::fabs(guest.value().m[i] - an.written[i]) < 1e-5f;
					if (is_ours)
						std::memcpy(base.m, an.base, sizeof(base.m));
				}

				float off[3];
				MatrixOpAngles(op, s_mat_prev[op_index], e.yaw, e.pitch, e.roll, off);
				const Mat3 r_off = Mat3FromEulerYXZ(off[0], off[1], off[2]);
				const Mat3 r_new = (op.order == ProfileDB::MatrixComposeOrder::Pre)
									   ? Mat3Mul(r_off, base)
									   : Mat3Mul(base, r_off);
				WriteGuestMat3(address, r_new);
				if (op.also_transpose)
					WriteGuestMat3(transpose_address, Mat3Transpose(r_new));
				std::memcpy(s_mat_anchor[op_index].base, base.m, sizeof(base.m));
				std::memcpy(s_mat_anchor[op_index].written, r_new.m, sizeof(r_new.m));
				s_mat_anchor[op_index].addr = address;
				s_mat_anchor[op_index].has = true;
				return;
			}

			float d[3];
			if (!MatrixOpAngles(op, s_mat_prev[op_index], e.yaw, e.pitch, e.roll, d))
				return;
			const float dyaw = d[0], dpitch = d[1], droll = d[2];

			const std::optional<Mat3> game = ReadGuestMat3(address);
			if (!game.has_value())
				return;

			const Mat3 r_delta = Mat3FromEulerYXZ(dyaw, dpitch, droll);
			const Mat3 r_new = (op.order == ProfileDB::MatrixComposeOrder::Pre)
								   ? Mat3Mul(r_delta, game.value())
								   : Mat3Mul(game.value(), r_delta);
			WriteGuestMat3(address, r_new);
			if (op.also_transpose)
				WriteGuestMat3(transpose_address, Mat3Transpose(r_new));
		}

		struct ResolvedBase
		{
			u32 crc = 0;
			u32 base = 0;
			bool valid = false;
			u64 next_retry_vsync = 0;
		};
		ResolvedBase s_base;
		u64 s_vsync_counter = 0;
		u32 s_base_unresolved_vsyncs = 0;

		std::optional<u32> ScanForBase(const ProfileDB::CameraBase& spec)
		{
			if (!eeMem)
				return std::nullopt;

			const u32 ram_size = Ps2MemSize::MainRam;
			const u32 lo = std::min(spec.scan_start, ram_size);
			const u32 hi = std::min(spec.scan_end, ram_size);
			const size_t n = spec.pattern.size();
			if (n == 0 || lo + n >= hi)
				return std::nullopt;

			const u8* const ram = reinterpret_cast<const u8*>(eeMem->Main);
			const u8* const pat = spec.pattern.data();
			const u8* const msk = spec.mask.data();

			std::optional<u32> last_good;
			for (u32 at = lo; at + n <= hi; at++)
			{
				if (msk[0] && ram[at] != pat[0])
					continue;

				bool match = true;
				for (size_t i = 1; i < n; i++)
				{
					if (msk[i] && ram[at + i] != pat[i])
					{
						match = false;
						break;
					}
				}
				if (!match)
					continue;

				if (spec.has_validate)
				{
					const s64 vaddr = static_cast<s64>(at) + spec.validate_offset;
					if (vaddr < 0 || vaddr + 4 > static_cast<s64>(ram_size))
						continue;
					u32 dword;
					std::memcpy(&dword, ram + vaddr, 4);
					if (dword != spec.validate_equals)
						continue;
				}

				last_good = at;
			}

			if (!last_good.has_value())
				return std::nullopt;

			const s64 base = static_cast<s64>(last_good.value()) + spec.base_offset;
			if (base < 0 || base >= static_cast<s64>(ram_size))
				return std::nullopt;
			return static_cast<u32>(base);
		}

		std::optional<u32> WalkChain(u32 root, const std::vector<s32>& hops)
		{
			EEMemoryInterface ee;
			return GuestRam::WalkChain(ee, root, hops);
		}

		std::optional<u32> GetBase(const ProfileDB::CameraProfile& cam, u32 crc)
		{
			if (!cam.base.has_value())
				return std::nullopt;

			if (cam.base->is_indexed)
			{
				const ProfileDB::CameraBase& spec = cam.base.value();
				u32 idx = 0;
				switch (spec.index_width)
				{
					case 1: idx = memRead8(spec.index_addr); break;
					case 2: idx = memRead16(spec.index_addr); break;
					default: idx = memRead32(spec.index_addr); break;
				}
				const std::optional<u32> element = ProfileDB::ResolveIndexedBase(spec, idx, [](u32 a, u32* v) {
					*v = memRead32(a);
					return true;
				});
				if (!element.has_value())
				{
					s_base.valid = false;
					return std::nullopt;
				}
				const u32 base = element.value();
				if (!s_base.valid || s_base.crc != crc || s_base.base != base)
				{
					s_base = ResolvedBase{crc, base, true, 0};
					DevCon.WriteLn("(VR) CameraDriver: camera base resolved at 0x%08X (index %u).", base, idx);
				}
				return base;
			}

			if (cam.base->is_pointer)
			{
				const ProfileDB::CameraBase& spec = cam.base.value();
				const std::optional<u32> obj = WalkChain(spec.pointer_addr, spec.deref_offsets);
				if (!obj.has_value())
				{
					s_base.valid = false;
					return std::nullopt;
				}
				u32 deref = obj.value();
				if (spec.has_validate)
				{
					const s64 vaddr = static_cast<s64>(deref) + spec.validate_offset;
					if (vaddr < 0 || vaddr + 4 > static_cast<s64>(Ps2MemSize::MainRam) ||
						memRead32(static_cast<u32>(vaddr)) != spec.validate_equals)
					{
						s_base.valid = false;
						return std::nullopt;
					}
				}
				const s64 resolved = static_cast<s64>(deref) + spec.base_offset;
				if (resolved < 0 || resolved >= static_cast<s64>(Ps2MemSize::MainRam))
				{
					s_base.valid = false;
					return std::nullopt;
				}
				const u32 base = static_cast<u32>(resolved);
				if (!s_base.valid || s_base.crc != crc || s_base.base != base)
				{
					s_base = ResolvedBase{crc, base, true, 0};
					Console.WriteLn("(VR) CameraDriver: camera struct base resolved at 0x%08X (pointer deref, %zu extra hop(s)).",
						base, spec.deref_offsets.size());
				}
				return base;
			}

			if (s_base.valid && s_base.crc == crc)
			{
				if (cam.base->has_validate)
				{
					const s64 vaddr = static_cast<s64>(s_base.base) - cam.base->base_offset + cam.base->validate_offset;
					if (vaddr < 0 || memRead32(static_cast<u32>(vaddr)) != cam.base->validate_equals)
					{
						s_base.valid = false;
						s_base.next_retry_vsync = 0;
					}
				}
				if (s_base.valid)
					return s_base.base;
			}

			if (s_vsync_counter < s_base.next_retry_vsync)
				return std::nullopt;

			const std::optional<u32> found = ScanForBase(cam.base.value());
			if (found.has_value())
			{
				const bool was_valid = (s_base.valid && s_base.crc == crc && s_base.base == found.value());
				s_base = ResolvedBase{crc, found.value(), true, 0};
				if (!was_valid)
					Console.WriteLn("(VR) CameraDriver: camera struct base resolved at 0x%08X.", found.value());
				return s_base.base;
			}

			s_base = ResolvedBase{crc, 0, false, s_vsync_counter + 60};
			return std::nullopt;
		}

		bool GuardListPass(const std::vector<ProfileDB::CameraGuard>& guards)
		{
			EEMemoryInterface ee;
			return GuestRam::GuardListPass(ee, guards);
		}

		bool GuardsPass(const ProfileDB::CameraProfile& cam)
		{
			return GuardListPass(cam.guards);
		}

		GuestPatch::Ledger s_patches;
		bool s_patches_log_set = false;

		void EnsurePatchLog()
		{
			if (s_patches_log_set)
				return;
			s_patches_log_set = true;
			s_patches.SetLog([](GuestPatch::LogLevel level, const std::string& line) {
				switch (level)
				{
					case GuestPatch::LogLevel::Routine:
						DevCon.WriteLn(line);
						break;
					case GuestPatch::LogLevel::Notice:
						Console.WriteLn(line);
						break;
					case GuestPatch::LogLevel::Warning:
						Console.Warning(line);
						break;
				}
			});
		}

		std::vector<GuestPatch::Hook> s_hook_list;
		std::vector<const ProfileDB::CameraCodeHook*> s_hook_src;

		void BuildHookList(const ProfileDB::CameraProfile& cam)
		{
			s_hook_list.clear();
			s_hook_src.clear();
			for (const ProfileDB::CameraCodeHook& h : cam.code_hooks)
			{
				if (!h.enabled)
					continue;
				const AssembledHook a = AssembleHook(h);
				GuestPatch::Hook g;
				g.site = h.hook_address;
				g.original = MipsJal(h.tail_jump_address);
				g.trampoline = a.trampoline;
				g.cave = h.cave_address;
				for (size_t w = 0; w < g.body.size(); w++)
					g.body[w] = a.cave[w];
				g.scratch = h.scratch_address;
				s_hook_list.push_back(g);
				s_hook_src.push_back(&h);
			}
		}

		void WriteCodeHookScratch(const EulerAngles& e, const HeadPose::Snapshot& pose)
		{
			EEMemoryInterface ee;
			for (size_t i = 0; i < s_hook_list.size(); i++)
			{
				const ProfileDB::CameraCodeHook& h = *s_hook_src[i];
				const float v = SelectSource(h.source, e, pose) * h.axis_sign * h.scale;
				if (!std::isfinite(v))
					continue;
				s_patches.WriteScratch(ee, i, std::bit_cast<u32>(v));
			}
		}

		PadLookState s_padlook{};

		bool s_armed_logged = false;

		std::atomic_bool s_recenter_requested{false};
		bool s_has_reference = false;
		float s_head_rel[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		u64 s_head_rel_sample = 0;
		u32 s_reference_crc = 0;
		float s_ref_x = 0.0f, s_ref_y = 0.0f, s_ref_z = 0.0f, s_ref_w = 1.0f;
		float s_ref_px = 0.0f, s_ref_py = 0.0f, s_ref_pz = 0.0f;

		void QuatMultiply(float ax, float ay, float az, float aw, float bx, float by, float bz, float bw,
			float& ox, float& oy, float& oz, float& ow)
		{
			ox = aw * bx + ax * bw + ay * bz - az * by;
			oy = aw * by - ax * bz + ay * bw + az * bx;
			oz = aw * bz + ax * by - ay * bx + az * bw;
			ow = aw * bw - ax * bx - ay * by - az * bz;
		}

		void ApplyReference(float& x, float& y, float& z, float& w)
		{
			if (!s_has_reference)
				return;
			float ox, oy, oz, ow;
			QuatMultiply(-s_ref_x, -s_ref_y, -s_ref_z, s_ref_w, x, y, z, w, ox, oy, oz, ow);
			x = ox;
			y = oy;
			z = oz;
			w = ow;
		}

		enum class DirectState : u8
		{
			Idle,
			Queued,
			Armed,
			Refused,
		};
		std::atomic<DirectState> s_direct_state{DirectState::Idle};
		u32 s_direct_crc = 0;
		u64 s_direct_retry_vsync = 0;
		bool s_direct_retry_said = false;

		bool DirectLeversInMemory(const ProfileDB::CameraDirectLook& dl)
		{
			const std::optional<ProfileDB::CameraDirectLever>* const levers[3] = {&dl.yaw, &dl.pitch, &dl.aim};
			for (const auto* lv : levers)
			{
				if (lv->has_value() && memRead32((*lv)->hook_address) != (*lv)->expect)
					return false;
			}
			return true;
		}

		void DirectLookRelease()
		{
			ExecHookSetHead(0.0f, 0.0f, false);
			if (s_direct_state.load() == DirectState::Idle)
				return;
			s_direct_state.store(DirectState::Idle);
			s_direct_crc = 0;
			Host::RunOnCPUThread([]() { ExecHookClearAll(); });
		}

		void DirectLookHold()
		{
				ExecHookSetHead(0.0f, 0.0f, false);
		}

		bool DirectLookApply(const ProfileDB::CameraDirectLook& dl, u32 crc, float yaw_rad, float pitch_rad)
		{
			if (s_direct_crc != crc && s_direct_state.load() != DirectState::Idle)
				DirectLookRelease();

			DirectLookPlan yaw, pitch, aim;
			if (dl.yaw.has_value())
				yaw = PlanDirectLever(dl.yaw.value(), yaw_rad);
			if (dl.pitch.has_value())
				pitch = PlanDirectLever(dl.pitch.value(), pitch_rad);
			if (dl.aim.has_value())
				aim = PlanDirectLever(dl.aim.value(), yaw_rad);

			if (s_direct_state.load() == DirectState::Refused && s_vsync_counter >= s_direct_retry_vsync)
			{
				s_direct_retry_vsync = s_vsync_counter + 60;
				if (DirectLeversInMemory(dl))
				{
					Console.WriteLn("(VR) CameraDriver: directLook: the game now holds the levers' instructions; arming again.");
					s_direct_state.store(DirectState::Idle);
				}
				else if (!s_direct_retry_said)
				{
					s_direct_retry_said = true;
					Console.WriteLn("(VR) CameraDriver: directLook: the levers' instructions are not in memory yet (the code loads later, or "
									"this is another revision); looking again once a second. padLook carries the look meanwhile.");
				}
			}

			if (s_direct_state.load() == DirectState::Idle)
			{
				s_direct_state.store(DirectState::Queued);
				s_direct_crc = crc;
				s_direct_retry_said = false;
				const bool has_yaw = dl.yaw.has_value(), has_pitch = dl.pitch.has_value(), has_aim = dl.aim.has_value();
				ExecGate gates[EXEC_GATES_MAX];
				int gate_count = 0;
				for (const ProfileDB::CameraGuard& g : dl.when)
				{
					if (g.is_chain || gate_count >= EXEC_GATES_MAX)
						continue;
					gates[gate_count].address = g.ee_address;
					gates[gate_count].equals = g.equals;
					gates[gate_count].width = g.width;
					gates[gate_count].not_equals = g.not_equals;
					gate_count++;
				}
				const ExecGate gate0 = gates[0], gate1 = gates[1];
				Host::RunOnCPUThread([yaw, pitch, aim, has_yaw, has_pitch, has_aim, gate0, gate1, crc]() {
					const auto spec_of = [gate0, gate1](const DirectLookPlan& p, bool is_pitch) {
						ExecHookSpec s;
						s.pc = p.pc;
						s.expect_op = p.expect;
						s.action = p.is_aim ? ExecAction::AimLookAt : (p.is_view ? ExecAction::ComposeView : ExecAction::AddGpr);
						s.reg = p.is_aim ? p.eye_reg : p.reg;
						s.reg2 = p.is_aim ? p.reg : 0;
						s.address = p.view_address;
						s.use_pitch = is_pitch;
						if (p.is_aim)
						{
							s.yaw_units = p.units;
							s.pitch_units = p.pitch_units;
						}
						else
						{
							s.yaw_units = (p.is_view && is_pitch) ? 0.0f : p.units;
							s.pitch_units = (p.is_view && !is_pitch) ? 0.0f : p.units;
						}
						s.caller_sp_offset = p.caller_sp_offset;
						s.caller_expect = p.caller;
						s.gates[0] = gate0;
						s.gates[1] = gate1;
						return s;
					};
					bool ok = true;
					if (has_yaw)
						ok = (ExecHookArm(spec_of(yaw, false)) >= 0) && ok;
					if (has_pitch)
						ok = (ExecHookArm(spec_of(pitch, true)) >= 0) && ok;
					if (has_aim)
						ok = (ExecHookArm(spec_of(aim, false)) >= 0) && ok;
					if (ok)
					{
						Console.WriteLn(Color_StrongGreen, "(VR) CameraDriver: directLook ARMED (CRC %08X) — yaw %s, pitch %s, aim %s.",
							crc, has_yaw ? "yes" : "no", has_pitch ? "yes" : "no", has_aim ? "yes" : "no");
						s_direct_state.store(DirectState::Armed);
					}
					else
					{
						ExecHookClearAll();
						Console.Warning("(VR) CameraDriver: directLook REFUSED (CRC %08X) — a lever did not arm; padLook, if "
										"the profile has one, carries the head-look.",
							crc);
						s_direct_state.store(DirectState::Refused);
						s_direct_retry_vsync = s_vsync_counter + 60;
					}
				});
			}

			if (s_direct_state.load() != DirectState::Armed)
			{
				ExecHookSetHead(0.0f, 0.0f, false);
				return false;
			}
			if (!dl.when.empty() && !GuardListPass(dl.when))
			{
				ExecHookSetHead(0.0f, 0.0f, false);
				return false;
			}
			const DirectLookAngles head = PlanDirectAngles(dl, yaw_rad, pitch_rad);
			ExecHookSetHead(head.yaw_rad, head.pitch_rad, true);
			return true;
		}

	}

	float PadLookDeflection(const ProfileDB::CameraPadLook& pl, float yaw_deg, PadLookState& st)
	{
		const float mag = std::abs(yaw_deg);
		const int dir = (yaw_deg > 0.0f) ? 1 : (yaw_deg < 0.0f) ? -1 : 0;

		const bool holding = (st.gate != 0) && (dir == st.gate);
		const bool engaged = (dir != 0) && (holding ? (mag >= pl.release_deg) : (mag > pl.engage_deg));

		float deflection = 0.0f;
		if (engaged)
		{
			const float t =
				std::clamp((mag - pl.engage_deg) / (pl.max_look_deg - pl.engage_deg), 0.0f, 1.0f);
			const float stick = pl.stick_floor + std::pow(t, pl.curve) * (1.0f - pl.stick_floor);
			deflection = std::copysign(stick, yaw_deg);
			st.gate = dir;
		}
		else
		{
			st.gate = 0;
		}

		if (pl.latch)
		{
			if (deflection >= 1.0f)
				st.latch = 1;
			else if (deflection <= -1.0f)
				st.latch = -1;
			else if (st.latch != 0)
			{
				const bool settled = (mag < pl.release_deg);
				const bool opposite = (mag > pl.engage_deg) && ((yaw_deg > 0.0f) != (st.latch > 0));
				if (settled || opposite)
					st.latch = 0;
				else
					deflection = static_cast<float>(st.latch);
			}
		}
		else
		{
			st.latch = 0;
		}
		return deflection;
	}

	HeadPose::Snapshot VsyncHead()
	{
		HeadPose::Snapshot pose = HeadPose::Get();
		return pose;
	}

	void Apply()
	{
		s_vsync_counter++;

		const bool switched_on = EffectiveVREnabled(EmuConfig.VR.Enable) && EmuConfig.VR.HeadCamera &&
		                         !SplitState::Active();

		const VMState vm_state = VMManager::GetState();
		const bool vm_live = (vm_state == VMState::Running || vm_state == VMState::Paused);
		if (!vm_live)
		{
			s_armed_logged = false;
			s_patches.Forget();
			s_base.valid = false;
			s_padlook = PadLookState{};
			ResetDeltaState();
			ResetDeltaTotals();
			PadLook::Publish(0.0f);
			DirectLookRelease();
			return;
		}

		ProfileDB::EnsureLoaded();
		const u32 crc = VMManager::GetDiscCRC();
		EnsurePatchLog();
		EEMemoryInterface patch_mem;
		s_patches.Service(patch_mem, crc, vm_state == VMState::Running, s_vsync_counter);
		const ProfileDB::Profile* profile = ProfileDB::Lookup(VMManager::GetDiscSerial(), crc);
		if (!profile || !profile->camera.has_value())
		{
			s_armed_logged = false;
			if (vm_state == VMState::Running)
				s_patches.Disarm(patch_mem, s_vsync_counter, nullptr);
			s_base.valid = false;
			s_padlook = PadLookState{};
			if (vm_state == VMState::Running)
			{
				for (AnchorState& a : s_anchor)
					RestoreAnchoredScalar(a);
			}
			ResetDeltaState( vm_state != VMState::Running);
			PadLook::Publish(0.0f);
			DirectLookRelease();
			return;
		}
		const ProfileDB::CameraProfile& cam = profile->camera.value();

		const HeadPose::Snapshot pose = VsyncHead();

		const bool projection_ok = cam.ArmsWithWorldProjection(VR::WorldProjection::EeEnabled());
		const bool armed =
			switched_on && (vm_state == VMState::Running) && pose.valid && projection_ok && GuardsPass(cam);

		if (armed != s_armed_logged)
		{
			if (armed)
				Console.WriteLn(Color_StrongGreen,
					"(VR) CameraDriver: ARMED (CRC %08X) — %zu write op(s), %zu matrix op(s).",
					crc, cam.writes.size(), cam.matrix_writes.size());
			else
			{
				const char* reason =
					!switched_on                   ? "VR/HeadCamera switch off" :
					(vm_state != VMState::Running) ? "VM not Running (paused)" :
					!pose.valid                    ? "no valid head pose published" :
					!projection_ok                 ? "world projection off (camera.requiresWorldProjection)" :
													 "profile guard failed";
				Console.WriteLn(Color_StrongOrange, "(VR) CameraDriver: DISARMED (CRC %08X) — %s.", crc, reason);
			}
			s_armed_logged = armed;
		}

		if (!armed)
		{
			if (s_patches.RepairPending() && GuardsPass(cam))
			{
				BuildHookList(cam);
				const GuestPatch::Ledger::Lists lists{cam.silence, s_hook_list};
				s_patches.Disarm(patch_mem, s_vsync_counter, &lists);
			}
			else
			{
				s_patches.Disarm(patch_mem, s_vsync_counter, nullptr);
			}
			if (vm_state == VMState::Running)
			{
				for (MatAnchor& an : s_mat_anchor)
				{
					if (!an.has)
						continue;
					const std::optional<Mat3> guest = ReadGuestMat3(an.addr);
					if (guest.has_value())
					{
						bool is_ours = true;
						for (int c = 0; c < 9 && is_ours; c++)
							is_ours = std::fabs(guest.value().m[c] - an.written[c]) < 1e-5f;
						if (is_ours)
						{
							Mat3 base{};
							std::memcpy(base.m, an.base, sizeof(base.m));
							WriteGuestMat3(an.addr, base);
						}
					}
					an.has = false;
				}
				for (AnchorState& a : s_anchor)
					RestoreAnchoredScalar(a);
			}
			s_padlook = PadLookState{};
			ResetDeltaState( vm_state != VMState::Running);
			PadLook::Publish(0.0f);
			DirectLookHold();
			return;
		}

		BuildHookList(cam);
		s_patches.Arm(patch_mem, cam.silence, s_hook_list, s_vsync_counter);

		const std::optional<u32> base = GetBase(cam, crc);
		s_base_now = base.value_or(0);
		s_base_now_vsync = base.has_value() ? s_vsync_counter : 0;
		if (cam.base.has_value() && !base.has_value())
		{
			s_base_unresolved_vsyncs++;
			if (s_base_unresolved_vsyncs == 300 || (s_base_unresolved_vsyncs > 300 && (s_base_unresolved_vsyncs - 300) % 1800 == 0))
			{
				if (cam.base->is_pointer)
					Console.Warning("(VR) CameraDriver: camera base UNRESOLVED for %u s while armed (CRC %08X) — relative op(s) are sitting out; pointer root 0x%08X reads 0x%08X (a hop or the validate word failed if that is non-zero).",
						s_base_unresolved_vsyncs / 60, crc, cam.base->pointer_addr, memRead32(cam.base->pointer_addr));
				else
					Console.Warning("(VR) CameraDriver: camera base UNRESOLVED for %u s while armed (CRC %08X) — relative op(s) are sitting out (scan/indexed base: no match).",
						s_base_unresolved_vsyncs / 60, crc);
			}
		}
		else
			s_base_unresolved_vsyncs = 0;

		if (s_recenter_requested.exchange(false, std::memory_order_acq_rel) || !s_has_reference ||
			s_reference_crc != crc)
		{
			s_ref_x = pose.orientation_x;
			s_ref_y = pose.orientation_y;
			s_ref_z = pose.orientation_z;
			s_ref_w = pose.orientation_w;
			s_ref_px = pose.position_x;
			s_ref_py = pose.position_y;
			s_ref_pz = pose.position_z;
			s_has_reference = true;
			s_reference_crc = crc;
			ResetDeltaTotals();
			ResetDeltaState( true);
			s_padlook.gate = 0;
			DevCon.WriteLn("(VR) CameraDriver: view recentered.");
		}

		if (s_delta_crc != crc || s_delta_prev.size() != cam.writes.size())
		{
			s_delta_crc = crc;
			s_delta_prev.assign(cam.writes.size(), 0.0f);
			s_delta_has.assign(cam.writes.size(), false);
			s_delta_total.assign(cam.writes.size(), 0.0f);
			s_delta_written.assign(cam.writes.size(), 0.0f);
			s_delta_written_has.assign(cam.writes.size(), false);
			for (AnchorState& a : s_anchor)
				RestoreAnchoredScalar(a);
			s_anchor.assign(cam.writes.size(), AnchorState{});
		}
		if (s_mat_crc != crc || s_mat_prev.size() != cam.matrix_writes.size())
		{
			s_mat_crc = crc;
			s_mat_prev.assign(cam.matrix_writes.size(), MatBaseline{});
			s_mat_anchor.assign(cam.matrix_writes.size(), MatAnchor{});
		}

		float qx = pose.orientation_x, qy = pose.orientation_y, qz = pose.orientation_z, qw = pose.orientation_w;
		ApplyReference(qx, qy, qz, qw);
		s_head_rel[0] = qx;
		s_head_rel[1] = qy;
		s_head_rel[2] = qz;
		s_head_rel[3] = qw;
		s_head_rel_sample++;

		EulerAngles euler = QuaternionToEulerYXZ(qx, qy, qz, qw);
		const float head_yaw_rad = euler.yaw, head_pitch_rad = euler.pitch;
		const EulerAngles raw_euler = euler;
		const DisplayFit::Fit display_fit = DisplayFit::Get();
		DisplayFit::HeadToReference(euler.yaw, euler.pitch, display_fit);

		for (size_t i = 0; i < cam.writes.size(); i++)
		{
			const ProfileDB::CameraWriteOp& op = cam.writes[i];
			u32 address = op.ee_address;
			if (op.relative)
			{
				if (!base.has_value())
				{
					s_delta_has[i] = false;
					if (i < s_anchor.size()) RestoreAnchoredScalar(s_anchor[i]);
					continue;
				}
				const s64 absolute = static_cast<s64>(address) + static_cast<s64>(base.value());
				if (absolute < 0 || absolute + 8 > static_cast<s64>(Ps2MemSize::MainRam))
				{
					s_delta_has[i] = false;
					if (i < s_anchor.size()) RestoreAnchoredScalar(s_anchor[i]);
					continue;
				}
				address = static_cast<u32>(absolute);
			}
			if (!op.when.empty() && !GuardListPass(op.when))
			{
				s_delta_has[i] = false;
				if (i < s_anchor.size())
					RestoreAnchoredScalar(s_anchor[i]);
				continue;
			}
			EulerAngles op_euler = raw_euler;
			DisplayFit::HeadForOp(op_euler.yaw, op_euler.pitch, display_fit, op.fitted_angles);
			EvaluateAndWrite(op, i, address, op_euler, pose);
		}

		for (size_t i = 0; i < cam.matrix_writes.size(); i++)
		{
			const ProfileDB::CameraMatrixOp& op = cam.matrix_writes[i];
			u32 address = op.ee_address;
			u32 taddress = op.transpose_address;
			if (op.relative)
			{
				if (!base.has_value())
				{
					s_mat_prev[i].OnGap();
					continue;
				}
				address += base.value();
				taddress += base.value();
			}
			if (!op.when.empty() && !GuardListPass(op.when))
			{
				s_mat_prev[i].OnGap();
				if (s_mat_anchor[i].has)
				{
					const std::optional<Mat3> guest = ReadGuestMat3(address);
					if (guest.has_value())
					{
						bool is_ours = true;
						for (int c = 0; c < 9 && is_ours; c++)
							is_ours = std::fabs(guest.value().m[c] - s_mat_anchor[i].written[c]) < 1e-5f;
						if (is_ours)
						{
							Mat3 base{};
							std::memcpy(base.m, s_mat_anchor[i].base, sizeof(base.m));
							WriteGuestMat3(address, base);
						}
					}
					s_mat_anchor[i].has = false;
				}
				continue;
			}
			ApplyMatrixOp(op, i, address, taddress, euler);
		}

		WriteCodeHookScratch(euler, pose);

		bool direct = false;
		if (cam.direct_look.has_value())
		{
			const ProfileDB::CameraDirectLook& dl = cam.direct_look.value();
			direct = DirectLookApply(dl, crc, dl.fitted_angles ? euler.yaw : head_yaw_rad,
				dl.fitted_angles ? euler.pitch : head_pitch_rad);
		}
		else
		{
			DirectLookRelease();
		}

		if (direct)
		{
			s_padlook = PadLookState{};
			PadLook::Publish(0.0f);
		}
		else if (cam.pad_look.has_value())
		{
			const float yaw_deg = euler.yaw * (180.0f / PI_F);
			PadLook::Publish(PadLookDeflection(cam.pad_look.value(), yaw_deg, s_padlook));
		}
		else
		{
			s_padlook = PadLookState{};
			PadLook::Publish(0.0f);
		}
	}

	void OnStateLoaded()
	{
		ResetDeltaState();
		ResetDeltaTotals();
		s_patches.OnStateLoaded();
	}

	void RequestRecenter()
	{
		s_recenter_requested.store(true, std::memory_order_release);
	}


	void HeadEulerYXZ(const float q[4], float out[3])
	{
		const EulerAngles e = QuaternionToEulerYXZ(q[0], q[1], q[2], q[3]);
		out[0] = e.yaw;
		out[1] = e.pitch;
		out[2] = e.roll;
	}

	bool MatrixOpAngles(const ProfileDB::CameraMatrixOp& op, MatrixHeadBaseline& prev,
		float head_yaw_rad, float head_pitch_rad, float head_roll_rad, float out[3])
	{
		constexpr float to_rad = PI_F / 180.0f;
		constexpr float turn = 2.0f * PI_F;
		const float raw[3] = {head_yaw_rad, head_pitch_rad, head_roll_rad};
		const float lo_deg[3] = {op.yaw_min_deg, op.pitch_min_deg, op.roll_min_deg};
		const float hi_deg[3] = {op.yaw_max_deg, op.pitch_max_deg, op.roll_max_deg};
		const bool near_zenith = std::abs(head_pitch_rad) > 80.0f * to_rad;
		float c[3];
		for (int a = 0; a < 3; a++)
		{
			if (std::isinf(lo_deg[a]) && std::isinf(hi_deg[a]))
			{
				c[a] = raw[a];
				continue;
			}
			if (near_zenith && a != 1 && prev.view_has[a])
			{
				c[a] = prev.last_view[a];
				continue;
			}
			const float lo = lo_deg[a] * to_rad, hi = hi_deg[a] * to_rad;
			if (raw[a] >= lo && raw[a] <= hi)
				prev.held[a] = 0;
			else if (prev.held[a] == 0)
			{
				if (std::isinf(lo_deg[a]))
					prev.held[a] = 1;
				else if (std::isinf(hi_deg[a]))
					prev.held[a] = -1;
				else
				{
					const float ref = prev.view_has[a] ? prev.last_view[a] : raw[a];
					const float d_hi = std::abs(std::remainder(ref - hi, turn));
					const float d_lo = std::abs(std::remainder(ref - lo, turn));
					prev.held[a] = (d_hi <= d_lo) ? 1 : -1;
				}
			}
			c[a] = (prev.held[a] > 0) ? hi : (prev.held[a] < 0) ? lo : raw[a];
			prev.last_view[a] = c[a];
			prev.view_has[a] = true;
		}
		const float y = c[0], p = c[1], r = c[2];
		if (op.anchored)
		{
			out[0] = y * op.axis_sign_yaw;
			out[1] = p * op.axis_sign_pitch;
			out[2] = r * op.axis_sign_roll;
			return true;
		}
		if (!prev.has)
		{
			prev.yaw = y;
			prev.pitch = p;
			prev.roll = r;
			prev.has = true;
			return false;
		}
		out[0] = std::remainder(y - prev.yaw, turn) * op.axis_sign_yaw;
		out[1] = std::remainder(p - prev.pitch, turn) * op.axis_sign_pitch;
		out[2] = std::remainder(r - prev.roll, turn) * op.axis_sign_roll;
		prev.yaw = y;
		prev.pitch = p;
		prev.roll = r;
		return true;
	}

	DeltaStep ComposeDelta(float game, const float* hold, float d, bool wrap360, float clamp_min, float clamp_max)
	{
		const float start = hold ? *hold : game;
		DeltaStep s;
		s.value = start + d;
		if (wrap360)
			s.value = Wrap360(s.value);
		s.value = std::clamp(s.value, clamp_min, clamp_max);
#if defined(VR_CAMERA_PLANTED_TOTAL_UNCLAMPED)
		s.applied = d;
#else
		s.applied = wrap360 ? std::remainder(s.value - start, 360.0f) : s.value - start;
#endif
		return s;
	}

	bool DeltaTotal(size_t op_index, float* total)
	{
		if (op_index >= s_delta_total.size())
			return false;
		*total = s_delta_total[op_index];
		return true;
	}

	bool ResolvedBaseNow(u32* base)
	{
		if (s_base_now_vsync == 0 || s_base_now_vsync != s_vsync_counter || s_base_now == 0)
			return false;
		*base = s_base_now;
		return true;
	}

	bool AnchoredBase(size_t op_index, float* base)
	{
		if (op_index >= s_anchor.size() || !s_anchor[op_index].has || !s_anchor[op_index].anchored)
			return false;
		*base = s_anchor[op_index].base;
		return true;
	}

	bool HeadRelative(float orientation[4], u64* sample)
	{
		if (s_head_rel_sample == 0)
			return false;
		for (int i = 0; i < 4; i++)
			orientation[i] = s_head_rel[i];
		*sample = s_head_rel_sample;
		return true;
	}

	bool Reference(float orientation[4], float position[3])
	{
		if (!s_has_reference)
			return false;
		orientation[0] = s_ref_x;
		orientation[1] = s_ref_y;
		orientation[2] = s_ref_z;
		orientation[3] = s_ref_w;
		position[0] = s_ref_px;
		position[1] = s_ref_py;
		position[2] = s_ref_pz;
		return true;
	}
}

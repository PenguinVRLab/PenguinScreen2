// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#if defined(__APPLE__)
#include <mach/semaphore.h>
#elif !defined(_WIN32)
#include <semaphore.h>
#endif

#include <atomic>
#include <functional>

namespace Threading
{

	extern u64 GetThreadCpuTime();
	extern u64 GetThreadTicksPerSecond();

	extern void SetNameOfCurrentThread(const char* name);

	extern void Timeslice();

	extern void SpinWait();

	extern void EnableHiresScheduler();
	extern void DisableHiresScheduler();

	extern void Sleep(int ms);

	extern void SleepUntil(u64 ticks);

	class ThreadHandle
	{
	public:
		ThreadHandle();
		ThreadHandle(ThreadHandle&& handle);
		ThreadHandle(const ThreadHandle& handle);
		~ThreadHandle();

		static ThreadHandle GetForCallingThread();

		ThreadHandle& operator=(ThreadHandle&& handle);
		ThreadHandle& operator=(const ThreadHandle& handle);

		operator void*() const { return m_native_handle; }
		operator bool() const { return (m_native_handle != nullptr); }

		u64 GetCPUTime() const;

		bool SetAffinity(u64 processor_mask) const;

	protected:
		void* m_native_handle = nullptr;

#if defined(__linux__)
		unsigned int m_native_id = 0;
#endif
	};

	class Thread : public ThreadHandle
	{
	public:
		using EntryPoint = std::function<void()>;

		Thread();
		Thread(Thread&& thread);
		Thread(const Thread&) = delete;
		Thread(EntryPoint func);
		~Thread();

		ThreadHandle& operator=(Thread&& thread);
		ThreadHandle& operator=(const Thread& handle) = delete;

		__fi bool Joinable() const { return (m_native_handle != nullptr); }
		__fi u32 GetStackSize() const { return m_stack_size; }

		void SetStackSize(u32 size);

		bool Start(EntryPoint func);
		void Detach();
		void Join();

	protected:
#ifdef _WIN32
		static unsigned __stdcall ThreadProc(void* param);
#else
		static void* ThreadProc(void* param);
#endif

		u32 m_stack_size = 0;
	};

	class KernelSemaphore
	{
#if defined(_WIN32)
		void* m_sema;
#elif defined(__APPLE__)
		semaphore_t m_sema;
#else
		sem_t m_sema;
#endif
	public:
		KernelSemaphore();
		~KernelSemaphore();
		void Post();
		void Wait();
		bool TryWait();
	};

	class WorkSema
	{
		KernelSemaphore m_sema;
		KernelSemaphore m_empty_sema;
		std::atomic<s32> m_state{0};

		enum
		{
			STATE_SPINNING = -2,
			STATE_SLEEPING = -1,
			STATE_RUNNING_0 = 0,
			STATE_FLAG_WAITING_EMPTY = 1 << 30,
		};

		bool IsDead(s32 state)
		{
			return state < STATE_SPINNING;
		}

		bool IsReadyForSleep(s32 state)
		{
			s32 waiting_empty_cleared = state & (STATE_FLAG_WAITING_EMPTY - 1);
			return waiting_empty_cleared == STATE_RUNNING_0;
		}

		s32 NextStateWaitForWork(s32 current)
		{
			s32 new_state = IsReadyForSleep(current) ? STATE_SLEEPING : STATE_RUNNING_0;
			return new_state | (current & STATE_FLAG_WAITING_EMPTY);
		}

	public:
		void NotifyOfWork()
		{
			s32 old = m_state.fetch_add(2, std::memory_order_release);
			if (old == STATE_SLEEPING)
				m_sema.Post();
		}

		bool CheckForWork();
		void WaitForWork();
		void WaitForWorkWithSpin();
		bool WaitForEmpty();
		bool WaitForEmptyWithSpin();
		void Kill();
		void Reset();
	};

	class UserspaceSemaphore
	{
		KernelSemaphore m_sema;
		std::atomic<int32_t> m_counter{0};

	public:
		UserspaceSemaphore() = default;
		~UserspaceSemaphore() = default;

		void Post()
		{
			if (m_counter.fetch_add(1, std::memory_order_release) < 0)
				m_sema.Post();
		}

		void Wait()
		{
			if (m_counter.fetch_sub(1, std::memory_order_acquire) <= 0)
				m_sema.Wait();
		}

		bool TryWait()
		{
			int32_t counter = m_counter.load(std::memory_order_relaxed);
			while (counter > 0 && !m_counter.compare_exchange_weak(counter, counter - 1, std::memory_order_acquire, std::memory_order_relaxed))
				;
			return counter > 0;
		}
	};
}

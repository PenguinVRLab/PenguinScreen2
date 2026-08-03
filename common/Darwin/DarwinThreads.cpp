// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Threading.h"
#include "common/Assertions.h"

#include <cstdio>
#include <cassert>
#include <sched.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#include <mach/thread_act.h>

__forceinline void Threading::Timeslice()
{
	sched_yield();
}

__forceinline void Threading::SpinWait()
{
#if defined(ARCH_X86)
	__asm__("pause");
#elif defined(ARCH_ARM64)
	__asm__ __volatile__("isb");
#endif
}

__forceinline void Threading::EnableHiresScheduler()
{
}

__forceinline void Threading::DisableHiresScheduler()
{
}

u64 Threading::GetThreadTicksPerSecond()
{
	return 1000000;
}

static u64 getthreadtime(thread_port_t thread)
{
	mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
	thread_basic_info_data_t info;

	kern_return_t kr = thread_info(thread, THREAD_BASIC_INFO,
		(thread_info_t)&info, &count);
	if (kr != KERN_SUCCESS)
	{
		return 0;
	}

	return (u64)info.user_time.seconds * (u64)1e6 +
		   (u64)info.user_time.microseconds +
		   (u64)info.system_time.seconds * (u64)1e6 +
		   (u64)info.system_time.microseconds;
}

u64 Threading::GetThreadCpuTime()
{
	u64 us = getthreadtime(pthread_mach_thread_np(pthread_self()));
	return us;
}

static void MACH_CHECK(kern_return_t mach_retval)
{
	if (mach_retval != KERN_SUCCESS)
	{
		fprintf(stderr, "mach error: %s", mach_error_string(mach_retval));
		assert(mach_retval == KERN_SUCCESS);
	}
}

Threading::KernelSemaphore::KernelSemaphore()
{
	MACH_CHECK(semaphore_create(mach_task_self(), &m_sema, SYNC_POLICY_FIFO, 0));
}

Threading::KernelSemaphore::~KernelSemaphore()
{
	MACH_CHECK(semaphore_destroy(mach_task_self(), m_sema));
}

void Threading::KernelSemaphore::Post()
{
	MACH_CHECK(semaphore_signal(m_sema));
}

void Threading::KernelSemaphore::Wait()
{
	MACH_CHECK(semaphore_wait(m_sema));
}

bool Threading::KernelSemaphore::TryWait()
{
	mach_timespec_t time = {};
	kern_return_t res = semaphore_timedwait(m_sema, time);
	if (res == KERN_OPERATION_TIMED_OUT)
		return false;
	MACH_CHECK(res);
	return true;
}

Threading::ThreadHandle::ThreadHandle() = default;

Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
	: m_native_handle(handle.m_native_handle)
{
}

Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
	: m_native_handle(handle.m_native_handle)
{
	handle.m_native_handle = nullptr;
}

Threading::ThreadHandle::~ThreadHandle() = default;

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
	ThreadHandle ret;
	ret.m_native_handle = pthread_self();
	return ret;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
	m_native_handle = handle.m_native_handle;
	handle.m_native_handle = nullptr;
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
	m_native_handle = handle.m_native_handle;
	return *this;
}

u64 Threading::ThreadHandle::GetCPUTime() const
{
	return getthreadtime(pthread_mach_thread_np((pthread_t)m_native_handle));
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
	return false;
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread)
	: ThreadHandle(thread)
	, m_stack_size(thread.m_stack_size)
{
	thread.m_stack_size = 0;
}

Threading::Thread::Thread(EntryPoint func)
	: ThreadHandle()
{
	if (!Start(std::move(func)))
		pxFailRel("Failed to start implicitly started thread.");
}

Threading::Thread::~Thread()
{
	pxAssertRel(!m_native_handle, "Thread should be detached or joined at destruction");
}

void Threading::Thread::SetStackSize(u32 size)
{
	pxAssertRel(!m_native_handle, "Can't change the stack size on a started thread");
	m_stack_size = size;
}

void* Threading::Thread::ThreadProc(void* param)
{
	std::unique_ptr<EntryPoint> entry(static_cast<EntryPoint*>(param));
	(*entry.get())();
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pxAssertRel(!m_native_handle, "Can't start an already-started thread");

	std::unique_ptr<EntryPoint> func_clone(std::make_unique<EntryPoint>(std::move(func)));

	pthread_attr_t attrs;
	bool has_attributes = false;

	if (m_stack_size != 0)
	{
		has_attributes = true;
		pthread_attr_init(&attrs);
	}
	if (m_stack_size != 0)
		pthread_attr_setstacksize(&attrs, m_stack_size);

	pthread_t handle;
	const int res = pthread_create(&handle, has_attributes ? &attrs : nullptr, ThreadProc, func_clone.get());
	if (res != 0)
		return false;

	m_native_handle = (void*)handle;
	func_clone.release();
	return true;
}

void Threading::Thread::Detach()
{
	pxAssertRel(m_native_handle, "Can't detach without a thread");
	pthread_detach((pthread_t)m_native_handle);
	m_native_handle = nullptr;
}

void Threading::Thread::Join()
{
	pxAssertRel(m_native_handle, "Can't join without a thread");
	void* retval;
	const int res = pthread_join((pthread_t)m_native_handle, &retval);
	if (res != 0)
		pxFailRel("pthread_join() for thread join failed");

	m_native_handle = nullptr;
}

void Threading::SetNameOfCurrentThread(const char* name)
{
	pthread_setname_np(name);
}

// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Threading.h"
#include "common/Assertions.h"
#include "common/HostSys.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include <limits>

bool Threading::WorkSema::CheckForWork()
{
	s32 value = m_state.load(std::memory_order_relaxed);
	pxAssert(!IsDead(value));

	while (!m_state.compare_exchange_weak(value,
		IsReadyForSleep(value) ? STATE_RUNNING_0 : (value & STATE_FLAG_WAITING_EMPTY),
		std::memory_order_acq_rel, std::memory_order_relaxed))
	{
	}

	if (!IsReadyForSleep(value))
		return true;

	if (value & STATE_FLAG_WAITING_EMPTY)
		m_empty_sema.Post();

	return false;
}

void Threading::WorkSema::WaitForWork()
{
	s32 value = m_state.load(std::memory_order_relaxed);
	pxAssert(!IsDead(value));
	while (!m_state.compare_exchange_weak(value, NextStateWaitForWork(value), std::memory_order_acq_rel, std::memory_order_relaxed))
		;
	if (IsReadyForSleep(value))
	{
		if (value & STATE_FLAG_WAITING_EMPTY)
			m_empty_sema.Post();
		m_sema.Wait();
		m_state.fetch_and(STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire);
	}
}

void Threading::WorkSema::WaitForWorkWithSpin()
{
	s32 value = m_state.load(std::memory_order_relaxed);
	pxAssert(!IsDead(value));
	while (IsReadyForSleep(value))
	{
		if (m_state.compare_exchange_weak(value, STATE_SPINNING, std::memory_order_release, std::memory_order_relaxed))
		{
			if (value & STATE_FLAG_WAITING_EMPTY)
				m_empty_sema.Post();
			value = STATE_SPINNING;
			break;
		}
	}
	u32 waited = 0;
	while (value < 0)
	{
		if (waited > SPIN_TIME_NS)
		{
			if (!m_state.compare_exchange_weak(value, STATE_SLEEPING, std::memory_order_relaxed))
				continue;
			m_sema.Wait();
			break;
		}
		waited += ShortSpin();
		value = m_state.load(std::memory_order_relaxed);
	}
	m_state.fetch_and(STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire);
}

bool Threading::WorkSema::WaitForEmpty()
{
	s32 value = m_state.load(std::memory_order_acquire);
	while (true)
	{
		if (value < 0)
			return !IsDead(value);
		if (m_state.compare_exchange_weak(value, value | STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire))
			break;
	}
	pxAssertMsg(!(value & STATE_FLAG_WAITING_EMPTY), "Multiple threads attempted to wait for empty (not currently supported)");
	m_empty_sema.Wait();
	return !IsDead(m_state.load(std::memory_order_relaxed));
}

bool Threading::WorkSema::WaitForEmptyWithSpin()
{
	s32 value = m_state.load(std::memory_order_acquire);
	u32 waited = 0;
	while (true)
	{
		if (value < 0)
			return !IsDead(value);
		if (waited > SPIN_TIME_NS && m_state.compare_exchange_weak(value, value | STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire))
			break;
		waited += ShortSpin();
		value = m_state.load(std::memory_order_acquire);
	}
	pxAssertMsg(!(value & STATE_FLAG_WAITING_EMPTY), "Multiple threads attempted to wait for empty (not currently supported)");
	m_empty_sema.Wait();
	return !IsDead(m_state.load(std::memory_order_relaxed));
}

void Threading::WorkSema::Kill()
{
	s32 value = m_state.exchange(std::numeric_limits<s32>::min(), std::memory_order_release);
	if (value & STATE_FLAG_WAITING_EMPTY)
		m_empty_sema.Post();
}

void Threading::WorkSema::Reset()
{
	m_state = STATE_RUNNING_0;
}

#if !defined(__APPLE__)

Threading::KernelSemaphore::KernelSemaphore()
{
#ifdef _WIN32
	m_sema = CreateSemaphore(nullptr, 0, LONG_MAX, nullptr);
#else
	sem_init(&m_sema, false, 0);
#endif
}

Threading::KernelSemaphore::~KernelSemaphore()
{
#ifdef _WIN32
	CloseHandle(m_sema);
#else
	sem_destroy(&m_sema);
#endif
}

void Threading::KernelSemaphore::Post()
{
#ifdef _WIN32
	ReleaseSemaphore(m_sema, 1, nullptr);
#else
	sem_post(&m_sema);
#endif
}

void Threading::KernelSemaphore::Wait()
{
#ifdef _WIN32
	WaitForSingleObject(m_sema, INFINITE);
#else
	sem_wait(&m_sema);
#endif
}

bool Threading::KernelSemaphore::TryWait()
{
#ifdef _WIN32
	return WaitForSingleObject(m_sema, 0) == WAIT_OBJECT_0;
#else
	return sem_trywait(&m_sema) == 0;
#endif
}

#endif

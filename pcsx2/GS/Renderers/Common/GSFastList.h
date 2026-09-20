// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/AlignedMalloc.h"
#include <climits>

template <class T>
struct Element
{
	T data;
	u16 next_index;
	u16 prev_index;
};

template <class T>
class FastListIterator;

template <class T>
class FastListReverseIterator;

template <class T>
class FastList
{
	friend class FastListIterator<T>;
	friend class FastListReverseIterator<T>;
private:
	Element<T>* m_buffer;
	u16 m_capacity;
	u16 m_free_indexes_stack_top;
	u16* m_free_indexes_stack;

public:
	__forceinline FastList()
	{
		m_buffer = nullptr;
		clear();
	}

	__forceinline ~FastList()
	{
		_aligned_free(m_buffer);
	}

	void clear()
	{
		m_capacity = 4;

		_aligned_free(m_buffer);
		m_buffer = (Element<T>*)_aligned_malloc(m_capacity * sizeof(Element<T>) + (m_capacity - 1) * sizeof(u16), 64);
		m_free_indexes_stack = (u16*)&m_buffer[m_capacity];

		m_buffer[0] = {T(), 0, 0};

		m_free_indexes_stack_top = 0;

		for (u16 i = 0; i < m_capacity - 1; i++)
		{
			m_free_indexes_stack[i] = i + 1;
		}
	}

	__forceinline u16 InsertFront(const T& data)
	{
		if (Full())
		{
			Grow();
		}

		const u16 free_index = m_free_indexes_stack[m_free_indexes_stack_top++];
		m_buffer[free_index].data = data;
		ListInsertFront(free_index);
		return free_index;
	}

	__forceinline void push_front(const T& data)
	{
		InsertFront(data);
	}

	__forceinline const T& back() const
	{
		return m_buffer[LastIndex()].data;
	}

	__forceinline void pop_back()
	{
		EraseIndex(LastIndex());
	}

	__forceinline u16 size() const
	{
		return m_free_indexes_stack_top;
	}

	__forceinline bool empty() const
	{
		return size() == 0;
	}

	__forceinline void EraseIndex(const u16 index)
	{
		ListRemove(index);
		m_free_indexes_stack[--m_free_indexes_stack_top] = index;
	}

	__forceinline void MoveFront(const u16 index)
	{
		if (FirstIndex() != index)
		{
			ListRemove(index);
			ListInsertFront(index);
		}
	}

	__forceinline const FastListIterator<T> begin() const
	{
		return FastListIterator<T>(this, FirstIndex());
	}

	__forceinline const FastListIterator<T> end() const
	{
		return FastListIterator<T>(this, 0);
	}

	__forceinline FastListIterator<T> erase(FastListIterator<T> i)
	{
		EraseIndex(i.Index());
		return ++i;
	}

	__forceinline const FastListReverseIterator<T> rbegin() const {
		return FastListReverseIterator<T>(this, LastIndex());
	}

	__forceinline const FastListReverseIterator<T> rend() const {
		return FastListReverseIterator<T>(this, 0);
	}

private:
	__forceinline const T& Data(const u16 index) const
	{
		return m_buffer[index].data;
	}

	__forceinline u16 NextIndex(const u16 index) const
	{
		return m_buffer[index].next_index;
	}

	__forceinline u16 PrevIndex(const u16 index) const
	{
		return m_buffer[index].prev_index;
	}

	__forceinline u16 FirstIndex() const
	{
		return m_buffer[0].next_index;
	}

	__forceinline u16 LastIndex() const
	{
		return m_buffer[0].prev_index;
	}

	__forceinline bool Full() const
	{
		return size() == m_capacity - 1;
	}

	__forceinline void ListInsertFront(const u16 index)
	{
		Element<T>& head = m_buffer[0];
		m_buffer[index].prev_index = 0;
		m_buffer[index].next_index = head.next_index;
		m_buffer[head.next_index].prev_index = index;
		head.next_index = index;
	}

	__forceinline void ListRemove(const u16 index)
	{
		const Element<T>& to_remove = m_buffer[index];
		m_buffer[to_remove.prev_index].next_index = to_remove.next_index;
		m_buffer[to_remove.next_index].prev_index = to_remove.prev_index;
	}

	void Grow()
	{
		if (m_capacity == USHRT_MAX)
			pxFailRel("FastList size maxed out at USHRT_MAX (65535) elements, cannot grow futhermore.");

		const u16 new_capacity = m_capacity <= (USHRT_MAX / 2) ? (m_capacity * 2) : USHRT_MAX;

		Element<T>* new_buffer = (Element<T>*)_aligned_malloc(new_capacity * sizeof(Element<T>) + (new_capacity - 1) * sizeof(u16), 64);
		u16* new_free_indexes_stack = (u16*)&new_buffer[new_capacity];

		memcpy(new_buffer, m_buffer, m_capacity * sizeof(Element<T>));
		memcpy(new_free_indexes_stack, m_free_indexes_stack, (m_capacity - 1) * sizeof(u16));

		_aligned_free(m_buffer);

		m_buffer = new_buffer;
		m_free_indexes_stack = new_free_indexes_stack;

		for (u16 i = m_capacity - 1; i < new_capacity - 1; i++)
			m_free_indexes_stack[i] = i + 1;

		m_capacity = new_capacity;
	}
};


template <class T>
class FastListIterator
{
private:
	const FastList<T>* m_fastlist;
	u16 m_index;

public:
	__forceinline FastListIterator(const FastList<T>* fastlist, const u16 index)
	{
		m_fastlist = fastlist;
		m_index = index;
	}

	__forceinline bool operator!=(const FastListIterator<T>& other) const
	{
		return (m_index != other.m_index);
	}

	__forceinline bool operator==(const FastListIterator<T>& other) const
	{
		return (m_index == other.m_index);
	}

	__forceinline const FastListIterator<T>& operator++()
	{
		m_index = m_fastlist->NextIndex(m_index);
		return *this;
	}

	__forceinline const FastListIterator<T> operator++(int)
	{
		FastListIterator<T> copy(*this);
		++(*this);
		return copy;
	}

	__forceinline const FastListIterator<T>& operator--()
	{
		m_index = m_fastlist->PrevIndex(m_index);
		return *this;
	}

	__forceinline const FastListIterator<T> operator--(int)
	{
		FastListIterator<T> copy(*this);
		--(*this);
		return copy;
	}

	__forceinline const T& operator*() const
	{
		return m_fastlist->Data(m_index);
	}

	__forceinline u16 Index() const
	{
		return m_index;
	}
};

template <class T>
class FastListReverseIterator
{
private:
	const FastList<T>* m_fastlist;
	u16 m_index;

public:
	__forceinline FastListReverseIterator(const FastList<T>* fastlist, const u16 index) {
		m_fastlist = fastlist;
		m_index = index;
	}

	__forceinline bool operator!=(const FastListReverseIterator<T>& other) const {
		return (m_index != other.m_index);
	}

	__forceinline bool operator==(const FastListReverseIterator<T>& other) const {
		return (m_index == other.m_index);
	}

	__forceinline const FastListReverseIterator<T>& operator++() {
		m_index = m_fastlist->PrevIndex(m_index);
		return *this;
	}

	__forceinline const FastListReverseIterator<T> operator++(int) {
		FastListReverseIterator<T> copy(*this);
		++(*this);
		return copy;
	}

	__forceinline const FastListReverseIterator<T>& operator--() {
		m_index = m_fastlist->NextIndex(m_index);
		return *this;
	}

	__forceinline const FastListReverseIterator<T> operator--(int) {
		FastListReverseIterator<T> copy(*this);
		--(*this);
		return copy;
	}

	__forceinline const T& operator*() const {
		return m_fastlist->Data(m_index);
	}

	__forceinline u16 Index() const {
		return m_index;
	}
};

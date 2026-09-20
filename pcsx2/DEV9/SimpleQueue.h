// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <atomic>

#include "common/Assertions.h"
#include "common/Console.h"

template <class T>
class SimpleQueue
{
private:
	struct SimpleQueueEntry
	{
		std::atomic_bool ready{false};
		SimpleQueueEntry* next;
		T value;
	};

	std::atomic<SimpleQueueEntry*> head{nullptr};
	SimpleQueueEntry* tail = nullptr;

public:
	SimpleQueue();

	void Enqueue(T entry);
	bool Dequeue(T* entry);
	bool IsQueueEmpty();

	~SimpleQueue();
};

template <class T>
SimpleQueue<T>::SimpleQueue()
{
	tail = new SimpleQueueEntry();
	head.store(tail);
}

template <class T>
void SimpleQueue<T>::Enqueue(T entry)
{
	SimpleQueueEntry* newHead = new SimpleQueueEntry();
	SimpleQueueEntry* newEntry = head.exchange(newHead);

	newEntry->value = std::move(entry);
	newEntry->next = newHead;

	newEntry->ready.store(true);
}

template <class T>
bool SimpleQueue<T>::Dequeue(T* entry)
{
	if (!tail->ready.load())
		return false;

	SimpleQueueEntry* retEntry = tail;
	tail = retEntry->next;

	*entry = std::move(retEntry->value);
	delete retEntry;
	return true;
}

template <class T>
bool SimpleQueue<T>::IsQueueEmpty()
{
	return head.load() == tail;
}

template <class T>
SimpleQueue<T>::~SimpleQueue()
{
	if (head != nullptr)
	{
		if (!IsQueueEmpty())
		{
			Console.Error("DEV9: Queue not empty");
			pxAssert(false);

			T entry;
			while (!IsQueueEmpty())
				Dequeue(&entry);
		}

		delete head;
		head = nullptr;
		tail = nullptr;
	}
}

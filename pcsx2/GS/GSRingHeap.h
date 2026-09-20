// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>

class GSRingHeap
{
	struct Buffer;
	Buffer* m_current_buffer;

	void orphanBuffer() noexcept;
	void* alloc_internal(size_t size, size_t align_mask, size_t prefix_size);
	static void free_internal(void* ptr, size_t size) noexcept;

	static constexpr size_t MIN_ALIGN = std::max(alignof(size_t), alignof(void*));

	static size_t getAlignMask(size_t align)
	{
		return std::max(MIN_ALIGN, align) - 1;
	}

public:
	GSRingHeap(GSRingHeap&&) = delete;
	GSRingHeap();
	~GSRingHeap() noexcept;

	void* alloc(size_t size, size_t align)
	{
		size_t alloc_size = size + sizeof(size_t);
		void* ptr = alloc_internal(size, getAlignMask(align), sizeof(size_t));
		size_t* header = static_cast<size_t*>(ptr);
		*header = alloc_size;
		return static_cast<void*>(header + 1);
	};

	template <typename T, typename... Args>
	T* make(Args&&... args)
	{
		std::unique_ptr<void, void(*)(void*)> ptr(alloc(sizeof(T), alignof(T)), GSRingHeap::free);
		new (ptr.get()) T(std::forward<Args>(args)...);
		return static_cast<T*>(ptr.release());
	}

	template <typename T>
	T* make_array(size_t count)
	{
		std::unique_ptr<void, void(*)(void*)> ptr(alloc(sizeof(T) * count, alignof(T)), GSRingHeap::free);
		new (ptr.get()) T[count]();
		return static_cast<T*>(ptr.release());
	}

	static void free(void* ptr)
	{
		size_t* header = static_cast<size_t*>(ptr) - 1;
		free_internal(static_cast<void*>(header), *header);
	}

	template <typename T>
	static void destroy(T* ptr)
	{
		ptr->~T();
		free(ptr);
	}

	template <typename T>
	static void destroy_array(T* ptr)
	{
		size_t* header = const_cast<size_t*>(reinterpret_cast<const size_t*>(ptr)) - 1;
		size_t size = (*header - sizeof(size_t)) / sizeof(T);
		for (size_t i = 0; i < size; i++)
			ptr[i].~T();
		free(ptr);
	}

	template <typename T>
	class SharedPtr
	{
		friend class GSRingHeap;

		struct alignas(MIN_ALIGN) AllocationHeader
		{
			uint32_t size;
			std::atomic<uint32_t> refcnt;
		};

		T* m_ptr;

		SharedPtr(T* ptr)
			: m_ptr(ptr)
		{
		}

		AllocationHeader* getHeader()
		{
			return const_cast<AllocationHeader*>(reinterpret_cast<const AllocationHeader*>(m_ptr)) - 1;
		}

	public:
		SharedPtr()
			: m_ptr(nullptr)
		{
		}

		SharedPtr(std::nullptr_t)
			: m_ptr(nullptr)
		{
		}

		SharedPtr(const SharedPtr& other)
			: m_ptr(other.m_ptr)
		{
			if (m_ptr)
				getHeader()->refcnt.fetch_add(1, std::memory_order_relaxed);
		}

		SharedPtr(SharedPtr&& other)
			: m_ptr(other.m_ptr)
		{
			other.m_ptr = nullptr;
		}

		SharedPtr& operator=(const SharedPtr& other)
		{
			this->~SharedPtr();
			new (this) SharedPtr(other);
			return *this;
		}

		SharedPtr& operator=(SharedPtr&& other)
		{
			this->~SharedPtr();
			new (this) SharedPtr(other);
			return *this;
		}

		~SharedPtr()
		{
			if (!m_ptr)
				return;
			AllocationHeader* header = getHeader();
			if (header->refcnt.fetch_sub(1, std::memory_order_relaxed) == 1)
			{
				m_ptr->~T();
				free_internal(static_cast<void*>(header), header->size);
			}
		}

		T& operator*() const { return *m_ptr; }
		T* operator->() const { return m_ptr; }
		T* get() const { return m_ptr; }

		template <typename Other>
		SharedPtr<Other> cast() const&
		{
			getHeader().refcount.fetch_add(1, std::memory_order_relaxed);
			return SharedPtr<Other>(static_cast<Other*>(m_ptr));
		}

		template <typename Other>
		SharedPtr<Other> cast() &&
		{
			SharedPtr<Other> other(static_cast<Other*>(m_ptr));
			m_ptr = nullptr;
			return other;
		}
	};

	template <typename T, typename... Args>
	SharedPtr<T> make_shared(Args&&... args)
	{
		using Header = typename SharedPtr<T>::AllocationHeader;
		static constexpr size_t alloc_size = sizeof(T) + sizeof(Header);
		static_assert(alignof(Header) <= MIN_ALIGN, "Header alignment too high");
		static_assert(alloc_size <= UINT32_MAX, "Allocation overflow");

		void* ptr = alloc_internal(sizeof(T), getAlignMask(alignof(T)), sizeof(Header));
		std::unique_ptr<void, void(*)(void*)> guard(ptr, [](void* p){ free_internal(p, alloc_size); });
		Header* header = static_cast<Header*>(ptr);
		header->size = static_cast<uint32_t>(alloc_size);
		header->refcnt.store(1, std::memory_order_relaxed);

		T* tptr = reinterpret_cast<T*>(header + 1);
		new (tptr) T(std::forward<Args>(args)...);
		guard.release();
		return SharedPtr<T>(tptr);
	}

	template <typename T>
	struct Deleter
	{
		void operator()(T* t)
		{
			if (t)
				destroy(t);
		}
	};

	template <typename T>
	struct Deleter<T[]>
	{
		void operator()(T* t)
		{
			if (t)
				destroy_array(t);
		}
	};

	template <typename T>
	using UniquePtr = std::unique_ptr<T, Deleter<T>>;

	template <typename T>
	struct _unique_if
	{
		typedef UniquePtr<T> _unique_single;
	};

	template <typename T>
	struct _unique_if<T[]>
	{
		typedef UniquePtr<T[]> _unique_array_unknown_bound;
	};

	template <typename T, size_t N>
	struct _unique_if<T[N]>
	{
		typedef void _unique_array_known_bound;
	};

	template <typename T, typename... Args>
	typename _unique_if<T>::_unique_single make_unique(Args&&... args)
	{
		return UniquePtr<T>(make<T>(std::forward<Args>(args)...));
	}

	template <typename T>
	typename _unique_if<T>::_unique_array_unknown_bound make_unique(size_t count)
	{
		typedef std::remove_extent_t<T> Base;
		return UniquePtr<T>(make_array<Base>(count));
	}

	template <class T, class... _Args>
	typename _unique_if<T>::_unique_array_known_bound make_unique(_Args&&...) = delete;
};

// The process-wide `operator new`/`delete` replacement.
//
// mimalloc is linked with MI_OVERRIDE OFF, so it does NOT patch the CRT. `malloc` and `free`
// keep their ordinary implementation and every other module in the process - notably
// coreclr.dll, which brings its own CRT and its own `operator new` - is completely
// unaffected. The dynamic override would have patched the CRT process-wide, which is a poor
// thing to do underneath a hosted .NET runtime that allocates executable memory for its JIT
// and runs threads this engine does not control.
//
// Consequence worth stating: this sees allocations from modules that link Engine, and
// nothing else. The runtime's own native heap is invisible here by design; managed memory is
// reported separately from GC statistics.
//
// Every overload the standard defines has to be replaced together. Replacing only some of
// them is the classic way to get a block allocated by mimalloc and released by the CRT.

#include "memory/MemoryBackend.hpp"

#include <cstddef>
#include <new>

#include <mimalloc.h>

#include "utils/Profiler.hpp"

extern "C" void aether_memory_force_link() noexcept
{
}

namespace aether::memory
{
	bool IsMimallocActive() noexcept
	{
		// Deliberately routed through ::operator new rather than mi_malloc: the question is
		// whether the REPLACEMENT is in effect, and calling mimalloc directly would answer
		// "is mimalloc linked", which is a different and much weaker claim.
		void* probe = ::operator new(64);
		const bool owned = mi_is_in_heap_region(probe);
		::operator delete(probe, 64);
		return owned;
	}
} // namespace aether::memory

// ── Allocation ──────────────────────────────────────────────────────────────────

void* operator new(const std::size_t size)
{
	void* ptr = mi_malloc(size);
	if (ptr == nullptr)
	{
		throw std::bad_alloc{};
	}
	AE_PROFILE_ALLOC(ptr, size);
	return ptr;
}

void* operator new[](const std::size_t size)
{
	void* ptr = mi_malloc(size);
	if (ptr == nullptr)
	{
		throw std::bad_alloc{};
	}
	AE_PROFILE_ALLOC(ptr, size);
	return ptr;
}

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept
{
	void* ptr = mi_malloc(size);
	if (ptr != nullptr)
	{
		AE_PROFILE_ALLOC(ptr, size);
	}
	return ptr;
}

void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept
{
	void* ptr = mi_malloc(size);
	if (ptr != nullptr)
	{
		AE_PROFILE_ALLOC(ptr, size);
	}
	return ptr;
}

// The C++17 over-aligned forms. The previous malloc-backed set omitted these, so every
// over-aligned type in the engine was still being served by the CRT.
void* operator new(const std::size_t size, const std::align_val_t alignment)
{
	void* ptr = mi_malloc_aligned(size, static_cast<std::size_t>(alignment));
	if (ptr == nullptr)
	{
		throw std::bad_alloc{};
	}
	AE_PROFILE_ALLOC(ptr, size);
	return ptr;
}

void* operator new[](const std::size_t size, const std::align_val_t alignment)
{
	void* ptr = mi_malloc_aligned(size, static_cast<std::size_t>(alignment));
	if (ptr == nullptr)
	{
		throw std::bad_alloc{};
	}
	AE_PROFILE_ALLOC(ptr, size);
	return ptr;
}

void* operator new(const std::size_t size, const std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	void* ptr = mi_malloc_aligned(size, static_cast<std::size_t>(alignment));
	if (ptr != nullptr)
	{
		AE_PROFILE_ALLOC(ptr, size);
	}
	return ptr;
}

void* operator new[](const std::size_t size, const std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	void* ptr = mi_malloc_aligned(size, static_cast<std::size_t>(alignment));
	if (ptr != nullptr)
	{
		AE_PROFILE_ALLOC(ptr, size);
	}
	return ptr;
}

// ── Deallocation ────────────────────────────────────────────────────────────────

void operator delete(void* ptr) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free(ptr);
}

void operator delete[](void* ptr) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free(ptr);
}

void operator delete(void* ptr, const std::size_t size) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free_size(ptr, size);
}

void operator delete[](void* ptr, const std::size_t size) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free_size(ptr, size);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free(ptr);
}

void operator delete(void* ptr, const std::align_val_t alignment) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete[](void* ptr, const std::align_val_t alignment) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr, const std::size_t size, const std::align_val_t alignment) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free_size_aligned(ptr, size, static_cast<std::size_t>(alignment));
}

void operator delete[](void* ptr, const std::size_t size, const std::align_val_t alignment) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free_size_aligned(ptr, size, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr, const std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete[](void* ptr, const std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	AE_PROFILE_FREE(ptr);
	mi_free_aligned(ptr, static_cast<std::size_t>(alignment));
}

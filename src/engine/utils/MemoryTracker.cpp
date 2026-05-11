// MemoryTracker.cpp
// Overrides global operator new/delete to report every CPU heap allocation to
// Tracy.  Only compiled in when TRACY_ENABLE is defined - zero cost in shipping
// builds.
//
// Tracy's own internals use malloc() directly so they do not recurse through
// these overrides.

#include "utils/Profiler.hpp"

#ifdef TRACY_ENABLE

#	include <cstdlib>
#	include <new>

void* operator new(std::size_t size)
{
	void* ptr = std::malloc(size);
	if (!ptr)
	{
		throw std::bad_alloc{};
	}
	AE_PROFILE_ALLOC(ptr, size);
	return ptr;
}

void* operator new[](std::size_t size)
{
	void* ptr = std::malloc(size);
	if (!ptr)
	{
		throw std::bad_alloc{};
	}
	AE_PROFILE_ALLOC(ptr, size);
	return ptr;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
	void* ptr = std::malloc(size);
	if (ptr)
	{
		AE_PROFILE_ALLOC(ptr, size);
	}
	return ptr;
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
	void* ptr = std::malloc(size);
	if (ptr)
	{
		AE_PROFILE_ALLOC(ptr, size);
	}
	return ptr;
}

void operator delete(void* ptr) noexcept
{
	AE_PROFILE_FREE(ptr);
	std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
	AE_PROFILE_FREE(ptr);
	std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
	AE_PROFILE_FREE(ptr);
	std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
	AE_PROFILE_FREE(ptr);
	std::free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept
{
	AE_PROFILE_FREE(ptr);
	std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept
{
	AE_PROFILE_FREE(ptr);
	std::free(ptr);
}

#endif // TRACY_ENABLE

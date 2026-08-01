#pragma once

#include "memory/MemoryTag.hpp"

namespace aether::memory
{
	namespace detail
	{
		// Constant-initialized, so it is valid from the first allocation in the process -
		// including allocations during static initialization, which run before any engine
		// code has had a chance to set anything up.
		//
		// It also has to be correct on threads this engine never created. The CLR makes its
		// own (GC, finalizer, tiered JIT), and managed-to-native callbacks run engine code on
		// them; there the value default-initializes to Unknown, which is the honest answer
		// rather than whatever the last engine thread happened to be doing.
		inline thread_local constinit MemTag t_currentTag = MemTag::Unknown;
	} // namespace detail

	[[nodiscard]] inline MemTag CurrentTag() noexcept
	{
		return detail::t_currentTag;
	}

	// Sets the tag for the enclosing scope and restores the PREVIOUS value, not Unknown, so
	// scopes nest correctly: a Texture scope inside a Rendering scope leaves Rendering behind
	// when it ends.
	class ScopedMemTag
	{
	public:
		explicit ScopedMemTag(const MemTag tag) noexcept
		      : m_previous(detail::t_currentTag)
		{
			detail::t_currentTag = tag;
		}

		~ScopedMemTag()
		{
			detail::t_currentTag = m_previous;
		}

		ScopedMemTag(const ScopedMemTag&) = delete;
		ScopedMemTag& operator=(const ScopedMemTag&) = delete;
		ScopedMemTag(ScopedMemTag&&) = delete;
		ScopedMemTag& operator=(ScopedMemTag&&) = delete;

	private:
		MemTag m_previous;
	};
} // namespace aether::memory

// Line-suffixed so two scopes in the same block do not collide, and so the guard cannot be
// named and accidentally destroyed early.
#define AE_MEM_SCOPE_CAT_(a, b) a##b
#define AE_MEM_SCOPE_CAT(a, b) AE_MEM_SCOPE_CAT_(a, b)
#define AE_MEM_SCOPE(tag) const ::aether::memory::ScopedMemTag AE_MEM_SCOPE_CAT(aeMemScope_, __LINE__)(tag)

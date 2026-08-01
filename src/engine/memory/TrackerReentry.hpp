#pragma once

namespace aether::memory
{
	namespace detail
	{
		// Constant-initialized: this is consulted on the very first allocation in the process,
		// which can happen during another translation unit's static initialization.
		inline thread_local constinit int t_trackerDepth = 0;
	} // namespace detail

	[[nodiscard]] inline bool InTracker() noexcept
	{
		return detail::t_trackerDepth > 0;
	}

	// Stops the tracker recursing into itself.
	//
	// Every structure the tracker keeps - the tag table, later the ledger and the callstack
	// database - allocates. Those allocations go through the replaced `operator new`, which
	// would try to record them, which allocates again. Without this the first tracked
	// allocation never returns.
	//
	// It is a depth counter rather than a flag so that nesting is safe: only the OUTERMOST
	// guard reports that its caller may proceed, and an inner one silently does nothing
	// instead of clearing the flag early and re-opening the recursion on scope exit.
	class TrackerReentryGuard
	{
	public:
		TrackerReentryGuard() noexcept
		      : m_outermost(detail::t_trackerDepth == 0)
		{
			++detail::t_trackerDepth;
		}

		~TrackerReentryGuard() noexcept
		{
			--detail::t_trackerDepth;
		}

		// True only for the outermost guard. A nested guard reports false and its caller must
		// do nothing at all.
		[[nodiscard]] bool IsOutermost() const noexcept
		{
			return m_outermost;
		}

		TrackerReentryGuard(const TrackerReentryGuard&) = delete;
		TrackerReentryGuard& operator=(const TrackerReentryGuard&) = delete;
		TrackerReentryGuard(TrackerReentryGuard&&) = delete;
		TrackerReentryGuard& operator=(TrackerReentryGuard&&) = delete;

	private:
		bool m_outermost;
	};
} // namespace aether::memory

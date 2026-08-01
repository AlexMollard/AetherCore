#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

#include "Defines.hpp" // AE_DEV_TOOLING (force-included, named here for clarity)

namespace aether::memory
{
	// How much the tracker records. Ordered from cheapest to most expensive, and compared
	// with `<`, so inserting a level in the middle changes the meaning of every comparison -
	// append instead.
	enum class TrackingLevel : std::uint8_t
	{
		Disabled,   // nothing; the allocation path is a straight call into mimalloc
		Counters,   // per-tag totals only. Cheap enough to leave on in a shipped build
		Ledger,     // plus a pointer -> record map, so individual live blocks can be listed
		Callstacks, // plus a captured stack per allocation. Expensive; a debugging tool
	};

	// The highest level this BUILD can reach, regardless of what anyone asks for at runtime.
	//
	// A shipped build has no business paying for a ledger or capturing stacks, and gating it
	// at compile time means the code to do so is not merely unreachable but absent. Retail
	// and ship builds stop at Counters.
#if AE_DEV_TOOLING
	inline constexpr TrackingLevel kMaxTrackingLevel = TrackingLevel::Callstacks;
#else
	inline constexpr TrackingLevel kMaxTrackingLevel = TrackingLevel::Counters;
#endif

	namespace detail
	{
		// Constant-initialized for the same reason the counters are: this is read on the
		// allocation path, which runs before any dynamic initializer could have set it.
		//
		// Relaxed ordering - it gates statistics, and no reader depends on it being ordered
		// against anything. A stronger ordering would put a barrier on every allocation.
		inline constinit std::atomic<TrackingLevel> t_level{TrackingLevel::Counters};
	} // namespace detail

	[[nodiscard]] inline TrackingLevel CurrentLevel() noexcept
	{
		return detail::t_level.load(std::memory_order_relaxed);
	}

	// Silently clamps to the build's ceiling rather than failing. A project's settings file is
	// shared across configurations, so a developer asking for Callstacks must not break a
	// retail build that cannot provide them - it gets as much as it can give.
	inline void SetTrackingLevel(const TrackingLevel level) noexcept
	{
		detail::t_level.store(level > kMaxTrackingLevel ? kMaxTrackingLevel : level, std::memory_order_relaxed);
	}

	[[nodiscard]] inline bool LevelAtLeast(const TrackingLevel level) noexcept
	{
		return CurrentLevel() >= level;
	}

	[[nodiscard]] constexpr const char* ToString(const TrackingLevel level) noexcept
	{
		switch (level)
		{
			case TrackingLevel::Counters:
				return "Counters";
			case TrackingLevel::Ledger:
				return "Ledger";
			case TrackingLevel::Callstacks:
				return "Callstacks";
			case TrackingLevel::Disabled:
				break;
		}
		return "Disabled";
	}

	// Returns nothing for unrecognised text so the caller can distinguish "the user asked for
	// Disabled" from "the settings file says something we do not understand", and warn.
	[[nodiscard]] constexpr std::optional<TrackingLevel> ParseTrackingLevel(const std::string_view text) noexcept
	{
		if (text == "Disabled")
		{
			return TrackingLevel::Disabled;
		}
		if (text == "Counters")
		{
			return TrackingLevel::Counters;
		}
		if (text == "Ledger")
		{
			return TrackingLevel::Ledger;
		}
		if (text == "Callstacks")
		{
			return TrackingLevel::Callstacks;
		}
		return std::nullopt;
	}
} // namespace aether::memory

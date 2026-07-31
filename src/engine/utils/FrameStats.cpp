#include "utils/FrameStats.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "utils/FrameTimeline.hpp"

namespace aether
{
	namespace
	{
		// A non-finite sample breaks the strict weak ordering std::sort relies on, so filter
		// before sorting rather than trusting the timeline's floats. This is a DIAGNOSTIC: it
		// has to survive whatever it is handed rather than take the process down while
		// someone is trying to measure a problem.
		//
		// This was NOT the cause of the "stack around the variable 'sorted' was corrupted"
		// failure seen in a Debug GameRuntime - that reproduced with this filter in place and
		// after a clean rebuild, and does not reproduce in EngineTests against the same
		// Engine.lib. It remains open; the frame report no longer routes through here.
		void CollectFinite(const std::span<const FrameTiming> frames, std::vector<float>& out)
		{
			out.clear();
			out.reserve(frames.size());
			for (const FrameTiming& frame: frames)
			{
				if (std::isfinite(frame.wallMs))
				{
					out.push_back(frame.wallMs);
				}
			}
		}

		float PercentileOfSorted(const std::vector<float>& sorted, const float fraction)
		{
			if (sorted.empty())
			{
				return 0.0f;
			}
			const auto maxIndex = static_cast<float>(sorted.size() - 1);
			const auto index = static_cast<std::size_t>(std::lround(maxIndex * fraction));
			return sorted[std::min(index, sorted.size() - 1)];
		}
	} // namespace

	std::string_view SmoothnessLabel(const Smoothness smoothness)
	{
		switch (smoothness)
		{
			case Smoothness::Alternating:
				return "alternating";
			case Smoothness::Stuttering:
				return "stuttering";
			case Smoothness::Even:
				break;
		}
		return "even";
	}

	Smoothness ClassifySmoothness(const std::span<const FrameTiming> frames)
	{
		const std::size_t count = std::min(frames.size(), kClassifyWindow);
		if (count < 4)
		{
			return Smoothness::Even;
		}
		const std::span<const FrameTiming> window = frames.last(count);

		std::vector<float> sorted;
		CollectFinite(window, sorted);
		if (sorted.size() < 4)
		{
			return Smoothness::Even;
		}
		std::ranges::sort(sorted);
		const float median = sorted[sorted.size() / 2];
		if (median <= 0.0f)
		{
			return Smoothness::Even;
		}

		// Both tests require the excursion to be noticeable in absolute milliseconds too,
		// or a very fast build trips them on deviations nobody can see.
		std::size_t over15 = 0;
		bool anyOver2 = false;
		for (const float ms: sorted)
		{
			if (ms - median < kNoticeableMs)
			{
				continue;
			}
			if (ms > median * 2.0f)
			{
				anyOver2 = true;
			}
			if (ms > median * 1.5f)
			{
				++over15;
			}
		}
		if (anyOver2 || static_cast<float>(over15) > static_cast<float>(sorted.size()) * 0.01f)
		{
			return Smoothness::Stuttering;
		}

		float totalDelta = 0.0f;
		for (std::size_t i = 1; i < count; ++i)
		{
			totalDelta += std::abs(window[i].wallMs - window[i - 1].wallMs);
		}
		const float meanDelta = totalDelta / static_cast<float>(count - 1);
		const bool alternating = meanDelta > median * 0.5f && meanDelta > kNoticeableMs;
		return alternating ? Smoothness::Alternating : Smoothness::Even;
	}

	FrameStats ComputeFrameStats(const std::span<const FrameTiming> frames)
	{
		FrameStats stats;
		if (frames.empty())
		{
			return stats;
		}

		std::vector<float> sorted;
		CollectFinite(frames, sorted);
		if (sorted.empty())
		{
			return stats;
		}
		float total = 0.0f;
		for (const float ms: sorted)
		{
			total += ms;
		}
		std::ranges::sort(sorted);

		stats.sampleCount = sorted.size();
		stats.avgMs = total / static_cast<float>(sorted.size());
		stats.minMs = sorted.front();
		stats.maxMs = sorted.back();
		stats.medianMs = sorted[sorted.size() / 2];
		stats.p95Ms = PercentileOfSorted(sorted, 0.95f);
		stats.p99Ms = PercentileOfSorted(sorted, 0.99f);
		stats.smoothness = ClassifySmoothness(frames);
		return stats;
	}
} // namespace aether

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace aether
{
	struct FrameTiming;

	// How frames are being DELIVERED, which is what the eye responds to. An average frame
	// rate cannot express this: the editor measured a flawless 60 fps average while
	// alternating between 0.4 ms and 33 ms frames.
	enum class Smoothness
	{
		Even,
		Alternating,
		Stuttering,
	};

	[[nodiscard]] std::string_view SmoothnessLabel(Smoothness smoothness);

	struct FrameStats
	{
		std::size_t sampleCount = 0;
		float avgMs = 0.0f;
		float minMs = 0.0f;
		float maxMs = 0.0f;
		float medianMs = 0.0f;
		float p95Ms = 0.0f;
		float p99Ms = 0.0f;
		Smoothness smoothness = Smoothness::Even;
	};

	inline constexpr std::size_t kClassifyWindow = 120;

	// A verdict is about PERCEPTIBLE smoothness, so a deviation has to be large in absolute
	// terms as well as relative terms. Without this floor a build running at 1494 fps was
	// reported as "stuttering" because one 3.3 ms frame was 5x its 0.6 ms median - true, and
	// utterly invisible. A tool that cries wolf at 1494 fps does not get believed at 60.
	inline constexpr float kNoticeableMs = 4.0f;

	// All statistics are over FrameTiming::wallMs, never the clamped simulation delta.
	[[nodiscard]] FrameStats ComputeFrameStats(std::span<const FrameTiming> frames);

	// Uses at most the last kClassifyWindow frames, against their median m. Every threshold
	// is relative AND must clear kNoticeableMs in absolute terms:
	//   stuttering  - any frame > 2m, or more than 1% of frames > 1.5m
	//   alternating - mean absolute difference between consecutive frames > 0.5m
	//   even        - neither
	// Checked in that order, so a sequence that both alternates and spikes reports the
	// worse of the two.
	[[nodiscard]] Smoothness ClassifySmoothness(std::span<const FrameTiming> frames);
} // namespace aether

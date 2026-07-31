# Frame Timeline & Performance Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace a Performance panel that can only report clamped simulation delta with a real per-frame timeline that measures unclamped wall time and where each frame's time went.

**Architecture:** A lock-free ring of per-frame records lives in the engine (`FrameTimeline`), written by the game thread and completed by the render thread, and is always compiled in — including Release, where the existing Tracy-based timings vanish. A pure statistics header turns a span of those records into percentiles and a smoothness classification. The panel becomes a read-only view over both, holding no sample buffers of its own.

**Tech Stack:** C++23, CMake, doctest, Dear ImGui.

**Spec:** `docs/superpowers/specs/2026-07-31-frame-timeline-performance-panel-design.md`

## Global Constraints

- Build and test with the MSVC preset: `cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug`, then run `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`.
- The full suite must stay green at every commit. Baseline at time of writing: **649 test cases**. Note `tests/net/NetClientAuthorityTests.cpp` has a known load-sensitive case; if it fails, re-run before investigating.
- `FrameTimeline` must **not** be gated on `AE_DEV_TOOLING`, `TRACY_ENABLE`, or any config macro. It compiles into Release. This is the whole point.
- New files under `src/engine/**` are globbed into the `Engine` target automatically (`CONFIGURE_DEPENDS`), and `EngineTests` links `Engine` — so **no `tests/CMakeLists.txt` change is needed** for engine-side files. A brand-new `src/app/**.cpp` would need a CMake reconfigure; this plan adds none.
- The panel must never read `context.deltaTimeSeconds`. That is the clamped value this work exists to stop reporting.
- Commit style: plain imperative subject under ~72 chars, no `feat:`/`fix:` prefixes, no attribution lines. Body only when grouping several changes, as a flat bullet list.

## Instrumentation points (verified against current source)

| Phase | Where | Note |
|---|---|---|
| loop top | `AetherCore.cpp:310` `while (!ShouldClose())` | wall delta measured here |
| pacer wait | `AetherCore.cpp:314` `m_framePacer.Wait()` | no-op when `targetFps == 0` |
| sim delta | `AetherCore.cpp:318-320` | already clamped to `1/30` |
| in-flight wait | `AetherCore.cpp:337-340` `WaitUntilFrameCompleted` | the suspected stall |
| frame index | `m_producerFrameIndex`, incremented at `AetherCore.cpp:433` | render thread sees the same value as `packet.frameIndex` |
| render exec | `AetherCore.cpp:799` `execStart` in `ExecuteRenderFrame` | timer already exists, currently only fed to Tracy |
| acquire | `AetherCore.cpp:460` `BeginFrame()` -> `BeginSwapchainFrame()` | where FIFO blocks |
| present | `AetherCore.cpp:821` `EndFrame(packet)` -> `SubmitAndPresent()` | multiple return paths inside; time the whole call |

---

## File Structure

**Create:**
- `src/engine/utils/FrameTimeline.hpp` / `.cpp` — the ring and its two writers.
- `src/engine/utils/FrameStats.hpp` / `.cpp` — pure statistics over a span of records.
- `tests/utils/FrameTimelineTests.cpp`
- `tests/utils/FrameStatsTests.cpp`

**Modify:**
- `src/engine/AetherCore.hpp` — own a `FrameTimeline`.
- `src/engine/AetherCore.cpp` — instrument the loop and the render thread; register the service.
- `src/app/debug/PerformancePanel.hpp` / `.cpp` — rewrite as a view.

---

## Task 1: FrameTimeline — the ring and its two writers

**Files:**
- Create: `src/engine/utils/FrameTimeline.hpp`, `src/engine/utils/FrameTimeline.cpp`
- Test: `tests/utils/FrameTimelineTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `aether::FrameTiming`, `aether::FrameTimeline` with `RecordGameFrame(const FrameTiming&)`, `RecordRenderFrame(std::uint64_t frameIndex, float renderExecMs, float presentWaitMs)`, `Snapshot(std::span<FrameTiming> out) -> std::size_t`, `FrameCount() -> std::uint64_t`, and `kCapacity`. Tasks 2–4 all consume these.

- [ ] **Step 1: Write the failing test**

Create `tests/utils/FrameTimelineTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <span>

#include "utils/FrameTimeline.hpp"

using namespace aether;

namespace
{
	FrameTiming MakeGameFrame(const std::uint64_t index, const float wallMs)
	{
		FrameTiming timing;
		timing.frameIndex = index;
		timing.wallMs = wallMs;
		timing.simDtMs = wallMs > 33.33f ? 33.33f : wallMs;
		timing.pacerWaitMs = 0.0f;
		timing.inFlightWaitMs = wallMs * 0.5f;
		timing.gameWorkMs = wallMs * 0.5f;
		return timing;
	}
} // namespace

TEST_CASE("A recorded game frame reads back with every phase intact") {
    FrameTimeline timeline;
    timeline.RecordGameFrame(MakeGameFrame(7, 16.0f));

    std::array<FrameTiming, 8> out{};
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == 1);
    CHECK(out[0].frameIndex == 7);
    CHECK(out[0].wallMs == doctest::Approx(16.0f));
    CHECK(out[0].inFlightWaitMs == doctest::Approx(8.0f));
    CHECK(out[0].gameWorkMs == doctest::Approx(8.0f));
    CHECK_FALSE(out[0].renderComplete);
}

TEST_CASE("The render thread completes a frame the game thread already published") {
    FrameTimeline timeline;
    timeline.RecordGameFrame(MakeGameFrame(3, 16.0f));
    timeline.RecordGameFrame(MakeGameFrame(4, 16.0f));
    // The render thread trails, so it lands on an older, already-published record.
    timeline.RecordRenderFrame(3, 1.25f, 12.5f);

    std::array<FrameTiming, 8> out{};
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == 2);
    CHECK(out[0].frameIndex == 3);
    CHECK(out[0].renderExecMs == doctest::Approx(1.25f));
    CHECK(out[0].presentWaitMs == doctest::Approx(12.5f));
    CHECK(out[0].renderComplete);
    // The newer frame is still awaiting its render-thread half.
    CHECK(out[1].frameIndex == 4);
    CHECK_FALSE(out[1].renderComplete);
}

TEST_CASE("Snapshot returns the newest frames in order after the ring wraps") {
    FrameTimeline timeline;
    for (std::uint64_t i = 0; i < FrameTimeline::kCapacity + 10; ++i)
    {
        timeline.RecordGameFrame(MakeGameFrame(i, 16.0f));
    }

    std::vector<FrameTiming> out(FrameTimeline::kCapacity);
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == FrameTimeline::kCapacity);
    // Oldest surviving record first, newest last, contiguous.
    CHECK(out.front().frameIndex == 10);
    CHECK(out.back().frameIndex == FrameTimeline::kCapacity + 9);
    for (std::size_t i = 1; i < count; ++i)
    {
        CHECK(out[i].frameIndex == out[i - 1].frameIndex + 1);
    }
    CHECK(timeline.FrameCount() == FrameTimeline::kCapacity + 10);
}

TEST_CASE("A render completion for an evicted frame is dropped, not misapplied") {
    FrameTimeline timeline;
    for (std::uint64_t i = 0; i < FrameTimeline::kCapacity + 10; ++i)
    {
        timeline.RecordGameFrame(MakeGameFrame(i, 16.0f));
    }
    // Frame 0 was overwritten long ago; its slot now holds a much newer frame and
    // must not be corrupted by a late completion.
    timeline.RecordRenderFrame(0, 99.0f, 99.0f);

    std::vector<FrameTiming> out(FrameTimeline::kCapacity);
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == FrameTimeline::kCapacity);
    for (std::size_t i = 0; i < count; ++i)
    {
        CHECK(out[i].renderExecMs == doctest::Approx(0.0f));
    }
}

TEST_CASE("Snapshot into a buffer smaller than the history returns the newest that fit") {
    FrameTimeline timeline;
    for (std::uint64_t i = 0; i < 50; ++i)
    {
        timeline.RecordGameFrame(MakeGameFrame(i, 16.0f));
    }

    std::array<FrameTiming, 5> out{};
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == 5);
    CHECK(out.front().frameIndex == 45);
    CHECK(out.back().frameIndex == 49);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug`
Expected: FAIL to compile — `Cannot open include file: 'utils/FrameTimeline.hpp'`

- [ ] **Step 3: Write the header**

Create `src/engine/utils/FrameTimeline.hpp`:

```cpp
#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aether
{
	// One frame's cost, split by phase. All times are milliseconds.
	//
	// wallMs is the honest loop-to-loop measurement and is NEVER clamped; simDtMs is what
	// the simulation actually received after AetherCore's 1/30 ceiling. Showing both is
	// what makes lost simulation time visible instead of hidden behind the clamp.
	struct FrameTiming
	{
		std::uint64_t frameIndex = 0;
		float wallMs = 0.0f;
		float simDtMs = 0.0f;
		float pacerWaitMs = 0.0f;
		float inFlightWaitMs = 0.0f;
		float gameWorkMs = 0.0f;
		float renderExecMs = 0.0f;
		float presentWaitMs = 0.0f;
		bool renderComplete = false;
	};

	// A fixed ring of per-frame timings, written by the game thread and completed later by
	// the render thread, readable by anyone.
	//
	// Deliberately NOT gated on AE_DEV_TOOLING or TRACY_ENABLE: the engine's existing
	// per-phase timings go to Tracy, which compiles out in Release, so the build that
	// actually ships has no frame data at all. This one always exists. The cost is a few
	// timestamp reads and stores per frame, no allocation and no locks.
	class FrameTimeline
	{
	public:
		static constexpr std::size_t kCapacity = 600; // ~10 s at 60 fps

		// Game thread. Publishes the frame; the record becomes visible to readers.
		void RecordGameFrame(const FrameTiming& timing);

		// Render thread. Fills the two fields the game thread could not know yet, on a
		// record it already published. A frame that has since been evicted is ignored.
		void RecordRenderFrame(std::uint64_t frameIndex, float renderExecMs, float presentWaitMs);

		// Copies the newest frames into `out`, oldest first. Returns how many were written.
		[[nodiscard]] std::size_t Snapshot(std::span<FrameTiming> out) const;

		// Total frames ever recorded, not the number retained.
		[[nodiscard]] std::uint64_t FrameCount() const;

	private:
		// The two-writer scheme is only sound because the ring is vastly larger than the
		// render thread's lag (Swapchain::kMaxFramesInFlight, currently 3). The game thread
		// therefore cannot wrap around and reopen a slot the render thread is still
		// writing. Shrinking kCapacity toward the in-flight count would reintroduce a data
		// race, so the relationship is asserted rather than left to a comment.
		static_assert(kCapacity >= 64, "FrameTimeline capacity must stay far above the render thread's frame lag");

		struct Slot
		{
			std::uint64_t frameIndex = 0;
			float wallMs = 0.0f;
			float simDtMs = 0.0f;
			float pacerWaitMs = 0.0f;
			float inFlightWaitMs = 0.0f;
			float gameWorkMs = 0.0f;
			// Written by the render thread after the record is published, so these are the
			// only fields two threads touch.
			std::atomic<float> renderExecMs{0.0f};
			std::atomic<float> presentWaitMs{0.0f};
			std::atomic<bool> renderComplete{false};
		};

		std::array<Slot, kCapacity> m_slots{};
		std::atomic<std::uint64_t> m_recorded{0};
	};
} // namespace aether
```

- [ ] **Step 4: Write the implementation**

Create `src/engine/utils/FrameTimeline.cpp`:

```cpp
#include "utils/FrameTimeline.hpp"

#include <algorithm>

namespace aether
{
	void FrameTimeline::RecordGameFrame(const FrameTiming& timing)
	{
		Slot& slot = m_slots[timing.frameIndex % kCapacity];

		// Retire the previous occupant's render half BEFORE publishing, so a reader can
		// never see this frame's game data paired with the last one's render data.
		slot.renderComplete.store(false, std::memory_order_relaxed);
		slot.renderExecMs.store(0.0f, std::memory_order_relaxed);
		slot.presentWaitMs.store(0.0f, std::memory_order_relaxed);

		slot.frameIndex = timing.frameIndex;
		slot.wallMs = timing.wallMs;
		slot.simDtMs = timing.simDtMs;
		slot.pacerWaitMs = timing.pacerWaitMs;
		slot.inFlightWaitMs = timing.inFlightWaitMs;
		slot.gameWorkMs = timing.gameWorkMs;

		// Release: everything above is visible to any reader that acquires m_recorded.
		m_recorded.store(timing.frameIndex + 1, std::memory_order_release);
	}

	void FrameTimeline::RecordRenderFrame(const std::uint64_t frameIndex, const float renderExecMs, const float presentWaitMs)
	{
		Slot& slot = m_slots[frameIndex % kCapacity];
		// The slot may have been reused by a newer frame while this one was rendering. A
		// late completion must be dropped, not written onto somebody else's record.
		if (slot.frameIndex != frameIndex)
		{
			return;
		}
		slot.renderExecMs.store(renderExecMs, std::memory_order_relaxed);
		slot.presentWaitMs.store(presentWaitMs, std::memory_order_relaxed);
		slot.renderComplete.store(true, std::memory_order_release);
	}

	std::size_t FrameTimeline::Snapshot(const std::span<FrameTiming> out) const
	{
		const std::uint64_t recorded = m_recorded.load(std::memory_order_acquire);
		if (recorded == 0 || out.empty())
		{
			return 0;
		}

		const std::uint64_t available = std::min<std::uint64_t>(recorded, kCapacity);
		const auto wanted = std::min<std::uint64_t>(available, out.size());
		const std::uint64_t firstIndex = recorded - wanted;

		for (std::uint64_t i = 0; i < wanted; ++i)
		{
			const std::uint64_t frameIndex = firstIndex + i;
			const Slot& slot = m_slots[frameIndex % kCapacity];
			FrameTiming& dst = out[static_cast<std::size_t>(i)];
			dst.frameIndex = slot.frameIndex;
			dst.wallMs = slot.wallMs;
			dst.simDtMs = slot.simDtMs;
			dst.pacerWaitMs = slot.pacerWaitMs;
			dst.inFlightWaitMs = slot.inFlightWaitMs;
			dst.gameWorkMs = slot.gameWorkMs;
			dst.renderExecMs = slot.renderExecMs.load(std::memory_order_relaxed);
			dst.presentWaitMs = slot.presentWaitMs.load(std::memory_order_relaxed);
			dst.renderComplete = slot.renderComplete.load(std::memory_order_acquire);
		}
		return static_cast<std::size_t>(wanted);
	}

	std::uint64_t FrameTimeline::FrameCount() const
	{
		return m_recorded.load(std::memory_order_acquire);
	}
} // namespace aether
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug` then `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe --test-case="*FrameTimeline*,*recorded game frame*,*render thread completes*,*ring wraps*,*evicted frame*,*smaller than the history*"`
Expected: PASS, 5 test cases.

- [ ] **Step 6: Run the full suite**

Run: `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`
Expected: `Status: SUCCESS!`, 654 test cases (649 + 5).

- [ ] **Step 7: Commit**

```bash
git add src/engine/utils/FrameTimeline.hpp src/engine/utils/FrameTimeline.cpp tests/utils/FrameTimelineTests.cpp
git commit -m "Add FrameTimeline, an always-on per-frame timing ring"
```

---

## Task 2: FrameStats — percentiles and the smoothness verdict

**Files:**
- Create: `src/engine/utils/FrameStats.hpp`, `src/engine/utils/FrameStats.cpp`
- Test: `tests/utils/FrameStatsTests.cpp`

**Interfaces:**
- Consumes: `aether::FrameTiming` (Task 1).
- Produces: `aether::Smoothness` (`Even`, `Alternating`, `Stuttering`), `aether::FrameStats` (fields `avgMs`, `minMs`, `maxMs`, `medianMs`, `p95Ms`, `p99Ms`, `smoothness`, `sampleCount`), `ComputeFrameStats(std::span<const FrameTiming>) -> FrameStats`, `ClassifySmoothness(std::span<const FrameTiming>) -> Smoothness`, `SmoothnessLabel(Smoothness) -> std::string_view`. Task 4 consumes all of these.

- [ ] **Step 1: Write the failing test**

Create `tests/utils/FrameStatsTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "utils/FrameStats.hpp"
#include "utils/FrameTimeline.hpp"

using namespace aether;

namespace
{
	std::vector<FrameTiming> FramesFrom(const std::vector<float>& wallMs)
	{
		std::vector<FrameTiming> frames;
		frames.reserve(wallMs.size());
		for (std::size_t i = 0; i < wallMs.size(); ++i)
		{
			FrameTiming t;
			t.frameIndex = i;
			t.wallMs = wallMs[i];
			frames.push_back(t);
		}
		return frames;
	}

	std::vector<float> Repeat(const float value, const std::size_t count)
	{
		return std::vector<float>(count, value);
	}
} // namespace

// The panel this replaces could not represent a frame worse than 33.33 ms at all.
TEST_CASE("Percentiles are computed on unclamped wall time and exceed 33.33") {
    std::vector<float> ms = Repeat(16.0f, 99);
    ms.push_back(250.0f);
    const auto frames = FramesFrom(ms);

    const FrameStats stats = ComputeFrameStats(frames);

    CHECK(stats.sampleCount == 100);
    CHECK(stats.maxMs == doctest::Approx(250.0f));
    CHECK(stats.p99Ms > 33.33f);
    CHECK(stats.minMs == doctest::Approx(16.0f));
    CHECK(stats.medianMs == doctest::Approx(16.0f));
}

TEST_CASE("An even sequence classifies as even") {
    const auto frames = FramesFrom(Repeat(16.67f, 120));

    CHECK(ClassifySmoothness(frames) == Smoothness::Even);
}

// The pattern measured in the editor: burst, then block on the in-flight wait.
TEST_CASE("An alternating short/long sequence classifies as alternating") {
    std::vector<float> ms;
    for (int i = 0; i < 120; ++i)
    {
        ms.push_back(i % 2 == 0 ? 0.4f : 33.0f);
    }
    const auto frames = FramesFrom(ms);

    CHECK(ClassifySmoothness(frames) == Smoothness::Alternating);
}

TEST_CASE("Isolated spikes classify as stuttering, and outrank alternating") {
    std::vector<float> ms = Repeat(16.0f, 119);
    ms.push_back(80.0f); // > 2x median
    const auto frames = FramesFrom(ms);

    CHECK(ClassifySmoothness(frames) == Smoothness::Stuttering);

    // An alternating sequence that ALSO spikes must report the worse of the two.
    std::vector<float> both;
    for (int i = 0; i < 119; ++i)
    {
        both.push_back(i % 2 == 0 ? 0.4f : 33.0f);
    }
    both.push_back(400.0f);
    CHECK(ClassifySmoothness(FramesFrom(both)) == Smoothness::Stuttering);
}

TEST_CASE("Empty input is reported as no samples rather than dividing by zero") {
    const FrameStats stats = ComputeFrameStats({});

    CHECK(stats.sampleCount == 0);
    CHECK(stats.avgMs == doctest::Approx(0.0f));
    CHECK(stats.smoothness == Smoothness::Even);
}

TEST_CASE("SmoothnessLabel names every value") {
    CHECK(SmoothnessLabel(Smoothness::Even) == "even");
    CHECK(SmoothnessLabel(Smoothness::Alternating) == "alternating");
    CHECK(SmoothnessLabel(Smoothness::Stuttering) == "stuttering");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug`
Expected: FAIL to compile — `Cannot open include file: 'utils/FrameStats.hpp'`

- [ ] **Step 3: Write the header**

Create `src/engine/utils/FrameStats.hpp`:

```cpp
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

	// All statistics are over FrameTiming::wallMs, never the clamped simulation delta.
	[[nodiscard]] FrameStats ComputeFrameStats(std::span<const FrameTiming> frames);

	// Uses at most the last kClassifyWindow frames, against their median m:
	//   stuttering  - any frame > 2m, or more than 1% of frames > 1.5m
	//   alternating - mean absolute difference between consecutive frames > 0.5m
	//   even        - neither
	// Checked in that order, so a sequence that both alternates and spikes reports the
	// worse of the two.
	[[nodiscard]] Smoothness ClassifySmoothness(std::span<const FrameTiming> frames);

	inline constexpr std::size_t kClassifyWindow = 120;
} // namespace aether
```

- [ ] **Step 4: Write the implementation**

Create `src/engine/utils/FrameStats.cpp`:

```cpp
#include "utils/FrameStats.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "utils/FrameTimeline.hpp"

namespace aether
{
	namespace
	{
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
		sorted.reserve(count);
		for (const FrameTiming& frame: window)
		{
			sorted.push_back(frame.wallMs);
		}
		std::ranges::sort(sorted);
		const float median = sorted[count / 2];
		if (median <= 0.0f)
		{
			return Smoothness::Even;
		}

		std::size_t over15 = 0;
		bool anyOver2 = false;
		for (const float ms: sorted)
		{
			if (ms > median * 2.0f)
			{
				anyOver2 = true;
			}
			if (ms > median * 1.5f)
			{
				++over15;
			}
		}
		if (anyOver2 || static_cast<float>(over15) > static_cast<float>(count) * 0.01f)
		{
			return Smoothness::Stuttering;
		}

		float totalDelta = 0.0f;
		for (std::size_t i = 1; i < count; ++i)
		{
			totalDelta += std::abs(window[i].wallMs - window[i - 1].wallMs);
		}
		const float meanDelta = totalDelta / static_cast<float>(count - 1);
		return meanDelta > median * 0.5f ? Smoothness::Alternating : Smoothness::Even;
	}

	FrameStats ComputeFrameStats(const std::span<const FrameTiming> frames)
	{
		FrameStats stats;
		if (frames.empty())
		{
			return stats;
		}

		std::vector<float> sorted;
		sorted.reserve(frames.size());
		float total = 0.0f;
		for (const FrameTiming& frame: frames)
		{
			sorted.push_back(frame.wallMs);
			total += frame.wallMs;
		}
		std::ranges::sort(sorted);

		stats.sampleCount = frames.size();
		stats.avgMs = total / static_cast<float>(frames.size());
		stats.minMs = sorted.front();
		stats.maxMs = sorted.back();
		stats.medianMs = sorted[sorted.size() / 2];
		stats.p95Ms = PercentileOfSorted(sorted, 0.95f);
		stats.p99Ms = PercentileOfSorted(sorted, 0.99f);
		stats.smoothness = ClassifySmoothness(frames);
		return stats;
	}
} // namespace aether
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug` then `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe --test-case="*unclamped wall time*,*classifies as*,*outrank alternating*,*no samples*,*names every value*"`
Expected: PASS, 6 test cases.

- [ ] **Step 6: Run the full suite**

Run: `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`
Expected: `Status: SUCCESS!`, 660 test cases.

- [ ] **Step 7: Commit**

```bash
git add src/engine/utils/FrameStats.hpp src/engine/utils/FrameStats.cpp tests/utils/FrameStatsTests.cpp
git commit -F - <<'EOF'
Add frame statistics over unclamped wall time

- Compute percentiles from real frame time rather than the simulation delta
- Classify delivery as even, alternating or stuttering, so an average frame rate
  can no longer hide a burst-then-block cadence
EOF
```

---

## Task 3: Instrument the engine

**Files:**
- Modify: `src/engine/AetherCore.hpp`, `src/engine/AetherCore.cpp`

**Interfaces:**
- Consumes: `aether::FrameTimeline` (Task 1).
- Produces: a `FrameTimeline` registered in the service container as `FrameTimeline`, populated every frame. Task 4 consumes it via `context.TryGet<FrameTimeline>()`.

- [ ] **Step 1: Own the timeline**

In `src/engine/AetherCore.hpp`, add the include beside the other `utils/` includes:

```cpp
#include "utils/FrameTimeline.hpp"
```

and a member beside `m_framePacer`:

```cpp
		FrameTimeline m_frameTimeline;
```

- [ ] **Step 2: Register it as a service**

In `src/engine/AetherCore.cpp`, beside the existing registrations around line 70:

```cpp
		m_services.Register<FrameTimeline>(m_frameTimeline);
```

- [ ] **Step 3: Instrument the game loop**

In `src/engine/AetherCore.cpp`, replace the top of the loop (currently lines 310-340) with the timed version. The phases are measured, not guessed: each `Wait` is bracketed so the panel can attribute a stall to the right one.

```cpp
		while (!ShouldClose())
		{
			AE_PROFILE_ZONE_N("Frame");

			const auto frameStart = std::chrono::steady_clock::now();

			m_framePacer.Wait();
			const auto afterPacer = std::chrono::steady_clock::now();

			client.OnFrameBegin();
			Logger::SetFrameNumber(m_producerFrameIndex);

			// rawDt is CLAMPED so a hitch cannot explode physics. wallSeconds is the same
			// interval unclamped, and is what the Performance panel reports - the clamped
			// value made every frame worse than 30 fps look identical.
			constexpr double kMaxDeltaTime = 1.0 / 30.0;
			const auto now = std::chrono::steady_clock::now();
			const double wallSeconds = std::chrono::duration<double>(now - previousFrameTime).count();
			const double rawDt = std::min(wallSeconds, kMaxDeltaTime);
			previousFrameTime = now;

			PumpEvents();

			// so the channel is provably empty when the render thread is parked.
			if (NeedsSwapchainOrViewportRecreate())
			{
				RunExclusive(QuiesceMode::Drain,
				        [this, &client]()
				        {
					        client.OnRenderTargetsInvalidated();
					        FlushImguiPendingTextureReleases();
					        RecreateSwapchainAndResources();
				        });
			}

			const auto beforeInFlightWait = std::chrono::steady_clock::now();
			if (m_producerFrameIndex >= Swapchain::kMaxFramesInFlight)
			{
				m_renderThread.WaitUntilFrameCompleted(m_producerFrameIndex - Swapchain::kMaxFramesInFlight);
			}
			const auto afterInFlightWait = std::chrono::steady_clock::now();
```

- [ ] **Step 4: Publish the record at the end of the loop**

In `src/engine/AetherCore.cpp`, immediately before `++m_producerFrameIndex;` (currently line 433):

```cpp
			{
				const auto frameEnd = std::chrono::steady_clock::now();
				const auto ms = [](const auto a, const auto b)
				{
					return static_cast<float>(std::chrono::duration<double, std::milli>(b - a).count());
				};
				FrameTiming timing;
				timing.frameIndex = m_producerFrameIndex;
				timing.wallMs = static_cast<float>(wallSeconds * 1000.0);
				timing.simDtMs = static_cast<float>(rawDt * 1000.0);
				timing.pacerWaitMs = ms(frameStart, afterPacer);
				timing.inFlightWaitMs = ms(beforeInFlightWait, afterInFlightWait);
				// Everything the game thread did that was not spent waiting.
				timing.gameWorkMs = std::max(0.0f, ms(frameStart, frameEnd) - timing.pacerWaitMs - timing.inFlightWaitMs);
				m_frameTimeline.RecordGameFrame(timing);
			}

			++m_producerFrameIndex;
```

- [ ] **Step 5: Instrument the render thread**

In `src/engine/AetherCore.cpp`, `ExecuteRenderFrame` already opens with `execStart` at line 799. Time the two blocking calls and publish the render half. Replace the `BeginFrame();` call near line 801 with:

```cpp
		const auto acquireStart = std::chrono::steady_clock::now();
		BeginFrame();
		const auto acquireEnd = std::chrono::steady_clock::now();
```

and replace the `EndFrame(packet);` call at line 821 with:

```cpp
		const auto presentStart = std::chrono::steady_clock::now();
		EndFrame(packet);
		const auto presentEnd = std::chrono::steady_clock::now();
```

Then, immediately after the existing `AE_PROFILE_PLOT("Frame/RenderThreadExecNs", ...)` line (currently 834):

```cpp
		{
			const auto ms = [](const auto a, const auto b)
			{
				return static_cast<float>(std::chrono::duration<double, std::milli>(b - a).count());
			};
			const auto execEnd = std::chrono::steady_clock::now();
			// presentWait is both places the render thread can block on the swapchain:
			// acquiring an image, and submitting/presenting it. With FIFO vsync this is
			// where the refresh cadence actually enters the frame.
			m_frameTimeline.RecordRenderFrame(packet.frameIndex,
			        ms(execStart, execEnd),
			        ms(acquireStart, acquireEnd) + ms(presentStart, presentEnd));
		}
```

- [ ] **Step 6: Build and confirm the engine still runs**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target Editor EngineTests --config Debug`
Expected: `0 Error(s)`

Run: `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`
Expected: `Status: SUCCESS!`, 660 test cases.

Launch the editor and confirm it still renders normally:

```bash
D:/AetherCore/build/vs2022-msvc/src/app/Debug/Editor.exe --project D:\AetherCore\projects\Whisper --no-validation
```

Expected: the Whisper title screen in the viewport, no new warnings in the console.

- [ ] **Step 7: Commit**

```bash
git add src/engine/AetherCore.hpp src/engine/AetherCore.cpp
git commit -F - <<'EOF'
Record real per-frame timings from the engine loop

- Measure unclamped wall time alongside the clamped simulation delta
- Time the frame pacer wait, the in-flight wait, render execute and present
- Publish them to FrameTimeline in every build config, not just where Tracy exists
EOF
```

---

## Task 4: Rewrite the Performance panel

**Files:**
- Modify: `src/app/debug/PerformancePanel.hpp`, `src/app/debug/PerformancePanel.cpp`

**Interfaces:**
- Consumes: `FrameTimeline` from the service container, `ComputeFrameStats`, `ClassifySmoothness`, `SmoothnessLabel`, `FrameStats`, `Smoothness` (Tasks 1–2).
- Produces: nothing other tasks depend on.

- [ ] **Step 1: Replace the header**

`src/app/debug/PerformancePanel.hpp` becomes — note the three sample arrays are gone, because the ring is now the only history:

```cpp
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "debug/DebugPanel.hpp"
#include "utils/FrameStats.hpp"   // FrameStats / Smoothness appear in the signatures below
#include "utils/FrameTimeline.hpp"

namespace aether::editor
{
	// A view over the engine's FrameTimeline. Holds no history of its own: the ring is the
	// single source of truth, which is what removed the three parallel sample arrays this
	// panel used to carry.
	class PerformancePanel final : public DebugPanel
	{
	public:
		static constexpr std::size_t kDisplayFrames = 240;

		std::string_view GetName() const override
		{
			return "Performance";
		}

		void OnImGui(app::LayerContext& context) override;
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;

	private:
		void DrawVerdict(const FrameStats& stats) const;
		void DrawPacingStrip() const;
		void DrawPhaseBreakdown() const;
		void DrawSimVsReal() const;
		void DrawStutterList(const FrameStats& stats) const;

		// Reused every frame so drawing allocates nothing.
		std::vector<FrameTiming> m_frames;

		static constexpr float kTitleUpdateInterval = 0.5f;
		float m_titleFps = 0.0f;
		float m_titleMs = 0.0f;
		float m_titleAccum = 0.0f;
	};
} // namespace aether::editor
```

- [ ] **Step 2: Rewrite the body**

`src/app/debug/PerformancePanel.cpp`. The panel resolves the timeline, snapshots it once, and every section reads that snapshot. The critical rule: **`context.deltaTimeSeconds` appears nowhere.**

```cpp
#include "debug/PerformancePanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include "layers/AppLayer.hpp"
#include "utils/FrameStats.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::editor
{
	namespace
	{
		ImVec4 SmoothnessColor(const Smoothness smoothness)
		{
			switch (smoothness)
			{
				case Smoothness::Stuttering:
					return chrome::kError;
				case Smoothness::Alternating:
					return chrome::kWarning;
				case Smoothness::Even:
					break;
			}
			return chrome::kSuccess;
		}

		const char* DominantPhase(const FrameTiming& frame)
		{
			struct Entry
			{
				const char* name;
				float ms;
			};
			const Entry entries[] = {
			        {"game work", frame.gameWorkMs},
			        {"in-flight wait", frame.inFlightWaitMs},
			        {"pacer wait", frame.pacerWaitMs},
			        {"render exec", frame.renderExecMs},
			        {"present wait", frame.presentWaitMs},
			};
			const Entry* worst = &entries[0];
			for (const Entry& entry: entries)
			{
				if (entry.ms > worst->ms)
				{
					worst = &entry;
				}
			}
			return worst->name;
		}

		float MeanOf(const std::vector<FrameTiming>& frames, float FrameTiming::*field)
		{
			if (frames.empty())
			{
				return 0.0f;
			}
			float total = 0.0f;
			for (const FrameTiming& frame: frames)
			{
				total += frame.*field;
			}
			return total / static_cast<float>(frames.size());
		}
	} // namespace

	void PerformancePanel::DrawVerdict(const FrameStats& stats) const
	{
		const float fps = stats.avgMs > 0.0f ? 1000.0f / stats.avgMs : 0.0f;
		ImGui::Text("%.0f fps", static_cast<double>(fps));
		ImGui::SameLine();
		ImGui::TextDisabled("avg %.2f ms", static_cast<double>(stats.avgMs));
		ImGui::SameLine();
		ImGui::TextColored(SmoothnessColor(stats.smoothness), "%s", std::string(SmoothnessLabel(stats.smoothness)).c_str());

		ImGui::TextDisabled("min %.2f   median %.2f   p95 %.2f   p99 %.2f   max %.2f ms",
		        static_cast<double>(stats.minMs), static_cast<double>(stats.medianMs),
		        static_cast<double>(stats.p95Ms), static_cast<double>(stats.p99Ms), static_cast<double>(stats.maxMs));
	}

	void PerformancePanel::DrawPacingStrip() const
	{
		if (m_frames.empty())
		{
			return;
		}
		ImGui::SeparatorText("Pacing");
		std::vector<float> wall;
		wall.reserve(m_frames.size());
		for (const FrameTiming& frame: m_frames)
		{
			wall.push_back(frame.wallMs);
		}
		const float scale = std::max(*std::ranges::max_element(wall), 1.0f);
		ImGui::PlotHistogram("##pacing", wall.data(), static_cast<int>(wall.size()), 0, nullptr, 0.0f, scale, ImVec2(-FLT_MIN, 72.0f));
		ImGui::TextDisabled("each column is one frame; even heights mean even delivery");
	}

	void PerformancePanel::DrawPhaseBreakdown() const
	{
		ImGui::SeparatorText("Where the time goes (mean ms)");
		if (ImGui::BeginTable("##phases", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			const struct
			{
				const char* label;
				float FrameTiming::*field;
			} rows[] = {
			        {"Game work", &FrameTiming::gameWorkMs},
			        {"In-flight wait", &FrameTiming::inFlightWaitMs},
			        {"Pacer wait", &FrameTiming::pacerWaitMs},
			        {"Render exec", &FrameTiming::renderExecMs},
			        {"Present wait", &FrameTiming::presentWaitMs},
			};
			for (const auto& row: rows)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.label);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", static_cast<double>(MeanOf(m_frames, row.field)));
			}
			ImGui::EndTable();
		}
	}

	void PerformancePanel::DrawSimVsReal() const
	{
		if (m_frames.empty())
		{
			return;
		}
		ImGui::SeparatorText("Simulation vs real");
		std::size_t clamped = 0;
		float lostMs = 0.0f;
		for (const FrameTiming& frame: m_frames)
		{
			if (frame.wallMs > frame.simDtMs + 0.01f)
			{
				++clamped;
				lostMs += frame.wallMs - frame.simDtMs;
			}
		}
		if (clamped == 0)
		{
			ImGui::TextColored(chrome::kSuccess, "Simulation received the full frame time.");
			return;
		}
		ImGui::TextColored(chrome::kWarning, "%zu of %zu frames were clamped, losing %.1f ms of simulation time.",
		        clamped, m_frames.size(), static_cast<double>(lostMs));
		ImGui::TextDisabled("The world advances slower than the clock; motion falls behind.");
	}

	void PerformancePanel::DrawStutterList(const FrameStats& stats) const
	{
		ImGui::SeparatorText("Worst frames");
		const float threshold = stats.medianMs * 1.5f;
		int shown = 0;
		for (auto it = m_frames.rbegin(); it != m_frames.rend() && shown < 6; ++it)
		{
			if (it->wallMs <= threshold)
			{
				continue;
			}
			ImGui::Text("#%llu  %.2f ms", static_cast<unsigned long long>(it->frameIndex), static_cast<double>(it->wallMs));
			ImGui::SameLine();
			ImGui::TextDisabled("mostly %s", DominantPhase(*it));
			++shown;
		}
		if (shown == 0)
		{
			ImGui::TextDisabled("No frame exceeded 1.5x the median.");
		}
	}

	void PerformancePanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		const auto* timeline = context.TryGet<FrameTimeline>();
		if (timeline == nullptr)
		{
			ImGui::Begin("Performance", VisiblePtr());
			chrome::PanelHeader("PERFORMANCE");
			// Deliberately no fallback to context.deltaTimeSeconds: reporting the clamped
			// simulation delta as if it were frame time is the bug this panel was rewritten
			// to remove, and a silent fallback would quietly reintroduce it.
			ImGui::TextColored(chrome::kError, "No frame timeline available.");
			ImGui::End();
			return;
		}

		m_frames.resize(kDisplayFrames);
		const std::size_t count = timeline->Snapshot(m_frames);
		m_frames.resize(count);

		const FrameStats stats = ComputeFrameStats(m_frames);

		m_titleAccum += ImGui::GetIO().DeltaTime;
		if (m_titleFps == 0.0f || m_titleAccum >= kTitleUpdateInterval)
		{
			m_titleFps = stats.avgMs > 0.0f ? 1000.0f / stats.avgMs : 0.0f;
			m_titleMs = stats.avgMs;
			m_titleAccum = 0.0f;
		}

		char title[96]{};
		std::snprintf(title, sizeof(title), "Performance | %.0f FPS | %.2f ms###Performance",
		        static_cast<double>(m_titleFps), static_cast<double>(m_titleMs));

		ImGui::Begin(title, VisiblePtr());
		chrome::PanelHeader("PERFORMANCE");
		DrawVerdict(stats);
		DrawPacingStrip();
		DrawPhaseBreakdown();
		DrawSimVsReal();
		DrawStutterList(stats);
		ImGui::End();
	}

	void PerformancePanel::LoadSettings(TomlConfig&, app::LayerContext&)
	{
	}

	void PerformancePanel::SaveSettings(TomlConfig&, app::LayerContext&) const
	{
	}
} // namespace aether::editor
```

- [ ] **Step 3: Build**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target Editor --config Debug`
Expected: `0 Error(s)`

- [ ] **Step 4: Confirm the panel reports honestly**

Launch the editor:

```bash
D:/AetherCore/build/vs2022-msvc/src/app/Debug/Editor.exe --project D:\AetherCore\projects\Whisper --no-validation
```

Open **Window > Diagnostics > Performance**. Expected:

- The header reads a smoothness word next to the fps, not just a number.
- `max`, `p95` and `p99` are **no longer pinned to 33.33** — this is the single most important check in the plan, since that constant is what made the old panel useless.
- The phase table shows non-zero values, with `in-flight wait` expected to dominate given the measured burst-then-block pattern.
- If any frames were clamped, "Simulation vs real" says how many and how much time was lost.

- [ ] **Step 5: Run the full suite**

Run: `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`
Expected: `Status: SUCCESS!`, 660 test cases.

- [ ] **Step 6: Commit**

```bash
git add src/app/debug/PerformancePanel.hpp src/app/debug/PerformancePanel.cpp
git commit -F - <<'EOF'
Rewrite the Performance panel as a frame timeline view

- Report unclamped wall time, so max and the percentiles stop reading 33.33
- Show where each frame's time went and which phase dominated the worst frames
- Show when the simulation clamp fires and how much time it costs
- Drop the panel's own sample buffers; the engine ring is the only history
EOF
```

---

## Task 5: Verify in Release and record the finding

**Files:** none created; verification only.

- [ ] **Step 1: Confirm the clamped delta is gone from the panel**

Run: `grep -rn "deltaTimeSeconds" src/app/debug/PerformancePanel.cpp`
Expected: no output.

- [ ] **Step 2: Build Release**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target Editor GameRuntime --config Release`
Expected: `0 Error(s)`

- [ ] **Step 3: Measure Release**

```bash
D:/AetherCore/build/vs2022-msvc/src/app/Release/Editor.exe --project D:\AetherCore\projects\Whisper
```

Open the Performance panel and record: fps, smoothness word, min/median/p95/p99/max, and the phase means.

Compare against the Debug figures measured before this work, which the spec records:

| | Debug (old panel) | Release (old panel) |
|---|---|---|
| Avg | 15.57 ms | 16.26 ms |
| Min | 6.96 ms | 0.37 ms |
| Max / P95 / P99 | 33.33 (clamped) | 33.33 (clamped) |

Expected now: `max` exceeds 33.33 whenever a real hitch occurs, and the smoothness word reads *alternating* given the 0.37 ms minimum beside a ~16 ms average.

- [ ] **Step 4: Answer the question the instrument was built for**

Record which phase dominates. The hypothesis from the spec is `inFlightWaitMs`, from
`AetherCore.cpp:337` — the game thread running up to `kMaxFramesInFlight` ahead and then
stalling. If the phase table shows that, the pacing fix has a confirmed target. **If it
shows something else, the spec's hypothesis was wrong and the next piece of work changes
accordingly** — which is exactly why this instrument was built before touching pacing.

Write the finding into the spec under a new "Measured after implementation" heading, so the
next person starts from data rather than the hypothesis.

- [ ] **Step 5: Commit and push**

```bash
git add docs/superpowers/specs/2026-07-31-frame-timeline-performance-panel-design.md
git commit -m "Record the measured frame phase breakdown"
git push origin master
```

---

## Self-Review Notes

**Spec coverage:** every spec section maps to a task — `FrameTiming`/ring/threading (Task 1), percentiles and the three-way classification with the exact thresholds (Task 2), the always-compiled-in requirement and all seven instrumentation points (Task 3), all five panel sections (Task 4), the Release verification and the "no silent fallback" rule (Tasks 4–5).

**Deviations from the spec, deliberate:**

- The spec described the pacing strip as "banded against the display refresh interval", with a fallback to the rolling median. Task 4 implements only the median-relative strip. Plumbing the monitor refresh rate out of the swapchain is a separate change, and drawing a budget line against a guessed rate would be worse than none — which the spec itself says. The strip is honest without it; the banding can be added when the refresh rate is actually available.
- The spec listed `renderComplete` handling as "treat the newest `kMaxFramesInFlight` records as incomplete". Task 1 implements this more precisely: the flag is per-record and reset on reuse, so the panel does not need to guess a lag count.

**No test for the panel itself.** `PerformancePanel` is ImGui drawing over already-tested pure functions; the logic worth pinning (`ComputeFrameStats`, `ClassifySmoothness`) is in `FrameStats` and covered there. Task 4's verification is manual and explicit about what to look at.

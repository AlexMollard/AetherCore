# Frame Timeline & Performance Panel - Design

Date: 2026-07-31
Status: Approved. Implementation plan pending.

## Goal

Make the editor able to answer "why does 60 fps look janky", by measuring real
per-frame wall time and where each frame's time went, and presenting it so uneven
delivery is obvious rather than averaged away.

Trigger: the editor and a published game both look noticeably worse than other
applications running at 60 Hz, and the existing Performance panel cannot explain it -
its numbers say everything is fine.

## Background: what is wrong today

### The panel measures one already-broken number

`PerformancePanel.cpp:27` is the panel's only input:

```cpp
PushFrameSample(static_cast<float>(context.deltaTimeSeconds * 1000.0));
```

`context.deltaTimeSeconds` is the **clamped simulation delta**. `AetherCore.cpp:318`:

```cpp
constexpr double kMaxDeltaTime = 1.0 / 30.0;
const double rawDt = std::min(std::chrono::duration<double>(now - previousFrameTime).count(), kMaxDeltaTime);
```

Every figure the panel shows - average, min, max, P95, P99, the histogram and the
graph - is derived from that single truncated value. Consequences:

- `Max`, `P95` and `P99` **cannot exceed 33.33 ms**. Measured in both Debug and
  Release, all three read exactly 33.33. That is the ceiling, not data: a 34 ms frame
  and a 400 ms hitch are indistinguishable.
- There is no breakdown. The panel cannot say whether a slow frame was the game
  thread, the render thread, or a wait.

### The phase timings exist but are invisible where they matter

The engine already measures the phases, and sends them to Tracy:

- `Frame/RenderThreadExecNs` - `AetherCore.cpp:834`
- `Frame/GameThreadTotalNs`, `Frame/ChannelSubmitNs` - configured in `Application.cpp:130-132`

`AE_PROFILE_PLOT` is Tracy, and Tracy is gated on `AE_DEV_TOOLING` (Debug and
RelWithDebInfo only). **In a Release build there is no frame timing data at all** -
which is the build a shipped game runs, and the build where "is it smooth" actually
matters.

### Measurements taken while writing this

Same project (Whisper `Title`), same layout, same window, 60 Hz display at 3840x2160:

| | Debug | Release |
|---|---|---|
| Avg frame | 15.57 ms | 16.26 ms |
| Min frame | 6.96 ms | **0.37 ms** |
| Render graph CPU | 0.099 ms | 0.047 ms |
| Max / P95 / P99 | 33.33 (clamped) | 33.33 (clamped) |

Release is not meaningfully faster on average, and the render graph costs under
0.05 ms across 29 passes - rendering is not the bottleneck. The `0.37 ms` minimum
beside a `16.26 ms` average, and a regular alternating sawtooth in the frame graph,
say frames arrive in pairs: one nearly instant, the next around a refresh interval.

### Why that produces judder rather than slowness

VSync is on and the swapchain uses `VK_PRESENT_MODE_FIFO_KHR` (`Swapchain.cpp:61`), so
the display receives one image per refresh, evenly spaced. Presentation is smooth. What
is uneven is the **simulation step**: `rawDt` is measured across the game thread's own
loop, and the game thread alternates between running free and blocking, so each evenly
displayed frame contains an unevenly advanced world.

The block is `AetherCore.cpp:337`:

```cpp
if (m_producerFrameIndex >= Swapchain::kMaxFramesInFlight)
    m_renderThread.WaitUntilFrameCompleted(m_producerFrameIndex - Swapchain::kMaxFramesInFlight);
```

With `kMaxFramesInFlight = 3` the game thread runs ahead, then stalls until the render
thread drains. `FramePacer` cannot smooth this: with `app.targetFps = 0` its `Wait()`
returns immediately (`FramePacer.hpp:38`), so the only cadence control is the present
block, which paces the *renderer*, not the simulation.

This design does not fix that. It builds the instrument that will prove or disprove it,
because changing frame pacing without per-phase measurements is guesswork.

## Decisions

1. **Measure pacing and phases, not GPU passes.** Per-pass GPU timestamp queries are a
   separate job; Tracy already covers deep CPU zones in dev builds. This design adds
   what neither provides: honest frame timing that survives into Release.
2. **The simulation clamp stays.** `kMaxDeltaTime` legitimately protects physics from a
   hitch. The panel stops reading the clamped value, and instead shows when the clamp
   fires.
3. **Always compiled in.** The collector is not Tracy-gated and not `AE_DEV_TOOLING`-gated.

## Architecture

### `FrameTimeline` (engine)

`src/engine/utils/FrameTimeline.hpp` / `.cpp`. One record per frame in a fixed ring:

```cpp
struct FrameTiming
{
    std::uint64_t frameIndex = 0;
    float wallMs = 0.0f;          // loop-to-loop, UNCLAMPED
    float simDtMs = 0.0f;         // what the simulation received (post-clamp)
    float pacerWaitMs = 0.0f;     // FramePacer::Wait
    float inFlightWaitMs = 0.0f;  // WaitUntilFrameCompleted
    float gameWorkMs = 0.0f;      // game thread minus its waits
    float renderExecMs = 0.0f;    // render thread execute
    float presentWaitMs = 0.0f;   // acquire + present block
};
```

Ownership and threading:

- The **game thread** opens a frame record, fills `wallMs`, `simDtMs`, `pacerWaitMs`,
  `inFlightWaitMs` and `gameWorkMs`, and publishes it.
- The **render thread** writes `renderExecMs` and `presentWaitMs` for the frame index it
  is executing. Because the render thread trails the game thread by up to
  `kMaxFramesInFlight`, its fields land on an already-published record; the slot is
  addressed by `frameIndex % kCapacity` so there is exactly one writer per field.
- Readers (the panel, and anything else) take a snapshot by copying the ring. A reader
  can observe a record whose render-thread fields are not yet filled; those read as 0 and
  the panel must treat the newest `kMaxFramesInFlight` records as incomplete rather than
  as zero-cost frames.

No allocation, no locks, a handful of `steady_clock` reads per frame. Capacity is a
compile-time constant sized for a few seconds of history (600 frames).

**Safety invariant:** the two-writer scheme is only sound because capacity vastly exceeds
`kMaxFramesInFlight` (600 vs 3). The render thread trails by at most 3 frames, so the game
thread cannot wrap the ring and reopen a slot the render thread is still writing. A static
assert pins this: shrinking the capacity toward the in-flight count would reintroduce a
data race, so the relationship is asserted rather than left as a comment.

The engine exposes it through the existing service container, so the panel reads it the
same way every other panel reads engine state.

### The panel (editor)

`PerformancePanel` keeps its name and slot but is rewritten as a view over
`FrameTimeline`. It holds no sample buffers of its own - the ring is the source of truth,
which removes the three parallel `std::array`s it carries today.

Sections, in order:

- **Verdict.** Real fps and frame time from `wallMs`, plus a plain-language smoothness
  call. "60 fps" alone is what misled the investigation that prompted this.

  The classification is defined so it can be tested rather than eyeballed, over the last
  120 frames and against the rolling median `m`:
  - *stuttering* - any frame exceeds `2m`, or more than 1% of frames exceed `1.5m`;
  - *alternating* - not stuttering, but consecutive frame times differ by more than
    `0.5m` on average, which is the signature of the burst-then-block pattern;
  - *even* - neither.

  Checked in that order, so an alternating sequence with real spikes reports the worse
  of the two.
- **Pacing strip.** One column per recent frame, height `wallMs`, banded against the
  display refresh interval. Alternating delivery is visible at a glance instead of
  disappearing into an average.

  The refresh interval is taken from the swapchain's present mode and the monitor's
  reported refresh rate, surfaced by the engine alongside the timeline. Where it is
  unavailable the strip falls back to the rolling median frame time and says so, because
  a budget line drawn against a guessed refresh rate would be worse than none.
- **Phase breakdown.** Game work, in-flight wait, pacer wait, render exec, present wait -
  as both current-frame values and a rolling average, so a bad frame names its culprit.
- **Simulation vs real.** `simDtMs` against `wallMs`, with the clamp marked. This is what
  exposes time being lost to `kMaxDeltaTime`.
- **Stutter list.** Recent frames above ~1.5x the rolling median, each tagged with the
  phase that dominated it.

Percentiles are computed on `wallMs`, so `Max`, `P95` and `P99` stop being a constant.

## Data flow

```
game thread   ── open(frameIndex) ─> FrameTiming slot ── publish ─┐
                  wall, simDt, pacerWait, inFlightWait, gameWork   │
                                                                   ├─> ring ─> panel
render thread ── renderExec, presentWait ─> same slot (trailing) ─┘
```

## Error handling

- A reader that finds an incomplete record (render fields still zero) shows the frame as
  pending rather than as a zero-cost frame.
- Sizing: at 600 frames the ring wraps in about ten seconds at 60 fps, which bounds how
  far back the stutter list can look. Records are overwritten silently.
- The collector never fails: it has no IO and no allocation. If timings are impossible to
  gather on a platform, they read 0 and the panel shows them as unavailable.

## Testing

`FrameTimeline` is pure data with no engine dependencies, so it goes in `EngineTests`:

- Recording and reading back a frame's phases round-trips exactly.
- The ring wraps correctly and a snapshot after wrap returns the newest N in order.
- Render-thread fields written for a trailing frame index land on the right record.
- Percentiles over a known distribution, including values above 33.33 ms, which the
  current panel cannot represent at all.
- Smoothness classification: an even sequence reads *even*, an alternating short/long
  sequence reads *alternating*, and a sequence with isolated spikes reads *stuttering*.

Manual verification: open the panel on Whisper in Release, confirm `Max`/`P95`/`P99` are
no longer pinned to 33.33, and confirm the pacing strip shows the alternating pattern the
measurements above imply.

## Consequences

- Adds a few `steady_clock` reads per frame in all configurations including Release. This
  is deliberate: timing data that vanishes in the shipping build is why the problem went
  unexplained.
- The three sample arrays in `PerformancePanel` are removed; history lives in one place.
- The panel gains a hard dependency on `FrameTimeline` being registered. Where it is
  absent the panel reports no data rather than falling back to the clamped delta - a
  silent fallback to the wrong number is what this design exists to remove.

## Out of scope

- Fixing the pacing itself. That is the next piece of work and needs this instrument
  first.
- Per-pass GPU timestamp queries.
- Any change to `kMaxDeltaTime`, `FramePacer`, or the swapchain present mode.

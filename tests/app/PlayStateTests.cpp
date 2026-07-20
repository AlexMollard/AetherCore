#include <doctest/doctest.h>

#include "PlayState.hpp"

using aether::app::PlayState;

TEST_CASE("PlayState defaults to Editing and never simulates") {
    PlayState state;
    CHECK(state.GetMode() == PlayState::Mode::Editing);
    CHECK_FALSE(state.IsPlaying());
    CHECK_FALSE(state.IsPaused());
    CHECK_FALSE(state.TakeSimulationStep());
    CHECK_FALSE(state.TakeSimulationStep());
}

TEST_CASE("Playing free-runs: every frame simulates") {
    PlayState state;
    state.SetMode(PlayState::Mode::Playing);
    CHECK(state.IsPlaying());
    CHECK_FALSE(state.IsPaused());
    CHECK(state.TakeSimulationStep());
    CHECK(state.TakeSimulationStep());
    CHECK(state.TakeSimulationStep());
}

TEST_CASE("Pause freezes the simulation") {
    PlayState state;
    state.SetMode(PlayState::Mode::Playing);
    state.SetPaused(true);
    CHECK(state.IsPaused());
    CHECK_FALSE(state.TakeSimulationStep());
    CHECK_FALSE(state.TakeSimulationStep());

    state.SetPaused(false);
    CHECK_FALSE(state.IsPaused());
    CHECK(state.TakeSimulationStep());
}

TEST_CASE("Step advances exactly one frame while paused") {
    PlayState state;
    state.SetMode(PlayState::Mode::Playing);
    state.SetPaused(true);

    state.RequestStep();
    CHECK(state.HasPendingStep());
    CHECK(state.TakeSimulationStep()); // consumes the step
    CHECK_FALSE(state.HasPendingStep());
    CHECK_FALSE(state.TakeSimulationStep()); // frozen again
    CHECK_FALSE(state.TakeSimulationStep());

    // A second explicit step advances one more frame.
    state.RequestStep();
    CHECK(state.TakeSimulationStep());
    CHECK_FALSE(state.TakeSimulationStep());
}

TEST_CASE("Step requests are ignored unless paused") {
    PlayState state;
    state.SetMode(PlayState::Mode::Playing);
    state.RequestStep(); // not paused -> no-op
    CHECK_FALSE(state.HasPendingStep());
}

TEST_CASE("Pause is a no-op outside Playing") {
    PlayState state;
    state.SetPaused(true); // Editing
    CHECK_FALSE(state.IsPaused());

    state.SetMode(PlayState::Mode::Compiling);
    state.SetPaused(true);
    CHECK_FALSE(state.IsPaused());
}

TEST_CASE("HUD timing only counts simulated frames and resets on fresh Play") {
    PlayState state;
    state.SetMode(PlayState::Mode::Playing);
    CHECK(state.PlayElapsedSeconds() == doctest::Approx(0.0));
    CHECK(state.PlayFrameCount() == 0);

    // Three simulated frames.
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(state.TakeSimulationStep());
        state.RecordSimulatedFrame(0.5);
    }
    CHECK(state.PlayElapsedSeconds() == doctest::Approx(1.5));
    CHECK(state.PlayFrameCount() == 3);
    CHECK(state.LastFrameSeconds() == doctest::Approx(0.5));

    // Paused frames do not tick the sim, so timing must not advance.
    state.SetPaused(true);
    CHECK_FALSE(state.TakeSimulationStep());
    CHECK(state.PlayFrameCount() == 3);
    CHECK(state.PlayElapsedSeconds() == doctest::Approx(1.5));

    // Stopping and re-entering Play resets the HUD.
    state.SetMode(PlayState::Mode::Editing);
    state.SetMode(PlayState::Mode::Playing);
    CHECK(state.PlayElapsedSeconds() == doctest::Approx(0.0));
    CHECK(state.PlayFrameCount() == 0);
}

TEST_CASE("Time scale clamps to the authoring range") {
    PlayState state;
    CHECK(state.TimeScale() == doctest::Approx(1.0f)); // default normal speed

    state.SetTimeScale(2.0f);
    CHECK(state.TimeScale() == doctest::Approx(2.0f));

    state.SetTimeScale(0.5f);
    CHECK(state.TimeScale() == doctest::Approx(0.5f));

    // Below the floor and above the ceiling both clamp.
    state.SetTimeScale(0.0f);
    CHECK(state.TimeScale() == doctest::Approx(PlayState::kMinTimeScale));
    state.SetTimeScale(1000.0f);
    CHECK(state.TimeScale() == doctest::Approx(PlayState::kMaxTimeScale));
    state.SetTimeScale(-5.0f);
    CHECK(state.TimeScale() == doctest::Approx(PlayState::kMinTimeScale));
}

TEST_CASE("Time scale persists across Play sessions") {
    PlayState state;
    state.SetMode(PlayState::Mode::Playing);
    state.SetTimeScale(4.0f);

    // Stop and re-enter Play: the chosen speed must survive (unlike pause/step,
    // which reset), while HUD timing resets.
    state.SetMode(PlayState::Mode::Editing);
    CHECK(state.TimeScale() == doctest::Approx(4.0f));
    state.SetMode(PlayState::Mode::Playing);
    CHECK(state.TimeScale() == doctest::Approx(4.0f));
    CHECK(state.PlayFrameCount() == 0);
}

TEST_CASE("Mode transitions drop pause and pending step") {
    PlayState state;
    state.SetMode(PlayState::Mode::Playing);
    state.SetPaused(true);
    state.RequestStep();
    CHECK(state.IsPaused());
    CHECK(state.HasPendingStep());

    // Leaving Playing must clear the frozen/step sub-state.
    state.SetMode(PlayState::Mode::Editing);
    CHECK_FALSE(state.IsPaused());
    CHECK_FALSE(state.HasPendingStep());
}

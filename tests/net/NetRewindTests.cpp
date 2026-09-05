// NetRewind: reconstructing where a replicated entity was from a specific
// connection's own point of view, so a host can judge a claim ("my shot hit you")
// against what the claimant actually saw instead of the victim's current, more
// up-to-date position. See NetRewind.hpp for the full design and the honesty
// boundary it documents.
//
// None of this compiles against the pre-change codebase - NetRewind.hpp/.cpp did
// not exist before this capability was added, so every case here is new coverage,
// not a regression pin on existing behaviour.

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "net/NetComponents.hpp"
#include "net/NetInterpolation.hpp"
#include "net/NetRewind.hpp"
#include "net/NetworkContext.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether;

// ── ClampRewindDelaySeconds ────────────────────────────────────────────────────
//
// The property the whole design leans on: a claim's rewind window comes from this
// host's own estimate, never from anything the far end says, and even that
// estimate is bounded. These pin the bound directly - no NetworkContext,
// connection, or entity involved.

TEST_CASE("An absurd requested delay is clamped to the cap, not honoured")
{
	CHECK(net::ClampRewindDelaySeconds(9999.f) == doctest::Approx(net::kMaxRewindSeconds));
}

TEST_CASE("A negative requested delay is clamped to zero, never rewinding into the future")
{
	CHECK(net::ClampRewindDelaySeconds(-5.f) == doctest::Approx(0.f));
}

TEST_CASE("An ordinary requested delay passes through the clamp unchanged")
{
	REQUIRE(0.05f < net::kMaxRewindSeconds);
	CHECK(net::ClampRewindDelaySeconds(0.05f) == doctest::Approx(0.05f));
}

// ── RewindFromHistory ───────────────────────────────────────────────────────────
//
// The pure sampling step RewindTransform composes with entity/connection
// resolution: given an already-resolved InterpolationBuffer and a host-time
// instant, reconstruct a pose or admit there is none.

TEST_CASE("RewindFromHistory interpolates between two samples like the buffer it reads")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 10.f, .position = {0.f, 0.f, 0.f}});
	buf.Push({.time = 11.f, .position = {10.f, 0.f, 0.f}});

	// hostNow - delaySeconds = 10.5, the midpoint between the two samples.
	const net::RewindSample result = net::RewindFromHistory(&buf, 11.5f, 1.0f);
	REQUIRE(result.hasHistory);
	CHECK(result.position.x == doctest::Approx(5.f));
	CHECK(result.appliedDelaySeconds == doctest::Approx(1.0f));
}

TEST_CASE("RewindFromHistory admits no history for an empty buffer")
{
	net::InterpolationBuffer buf;
	const net::RewindSample result = net::RewindFromHistory(&buf, 5.f, 0.1f);
	CHECK_FALSE(result.hasHistory);
	CHECK(result.position == glm::vec3(0.f));
}

TEST_CASE("RewindFromHistory admits no history when there is no buffer at all")
{
	const net::RewindSample result = net::RewindFromHistory(nullptr, 5.f, 0.1f);
	CHECK_FALSE(result.hasHistory);
}

TEST_CASE("RewindFromHistory returns the single sample a one-entry history holds, at any query time")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 3.f, .position = {7.f, 8.f, 9.f}});

	// Query far past the one sample - Sample() clamps to it rather than refusing,
	// and RewindFromHistory must carry that through rather than treating "a value
	// came back" and "there were two samples to interpolate" as the same thing.
	const net::RewindSample result = net::RewindFromHistory(&buf, 100.f, 50.f);
	REQUIRE(result.hasHistory);
	CHECK(result.position.x == doctest::Approx(7.f));
	CHECK(result.position.y == doctest::Approx(8.f));
	CHECK(result.position.z == doctest::Approx(9.f));
}

// ── EstimateViewDelaySeconds / RewindTransform ─────────────────────────────────
//
// The real public entry points, through a live NetworkContext - proving the clamp
// and the "no history" admission hold at the seam a game script actually calls
// through (NetExports.cpp's aether_net_rewind_transform / Net.TryRewind), not only
// in the pure helper above.

TEST_CASE("EstimateViewDelaySeconds clamps an absurdly large pinned render delay to the cap")
{
	ServiceContainer services;
	World world;
	net::NetworkContext context(services);

	const Entity victim = world.Create();
	// autoInterpolationDelay = false pins the render-delay term to this value
	// exactly, with no measurement involved - the most direct way to feed the
	// estimate an absurd number without a live connection's RTT.
	world.Emplace<net::NetworkTransform>(victim,
	        net::NetworkTransform{.interpolationDelaySeconds = 50.f, .autoInterpolationDelay = false});

	// Offline: RoundTripMs reads 0 (see NetworkContext::RoundTripMs), so the whole
	// 50 seconds comes from the pinned render-delay term alone.
	const float delay = net::EstimateViewDelaySeconds(world, context, net::kInvalidConnection, victim);
	CHECK(delay == doctest::Approx(net::kMaxRewindSeconds));
}

TEST_CASE("RewindTransform admits no history for an entity with no NetworkTransform")
{
	ServiceContainer services;
	World world;
	net::NetworkContext context(services);

	const Entity plain = world.Create(); // no NetworkIdentity, no NetworkTransform

	const net::RewindSample result = net::RewindTransform(world, context, net::kInvalidConnection, plain);
	CHECK_FALSE(result.hasHistory);
	// Still a real, clamped estimate - just nothing to sample it against, and never
	// negative or past the cap even on this fallback path.
	CHECK(result.appliedDelaySeconds >= 0.f);
	CHECK(result.appliedDelaySeconds <= net::kMaxRewindSeconds);
}

TEST_CASE("RewindTransform admits no history with no NetworkReceiveSystem registered at all")
{
	// A bare test World, exactly like every other NetworkContext unit test in this
	// suite - no system is ever registered on it. RewindTransform must degrade to
	// "no compensation available" rather than dereferencing a system that is not
	// there.
	ServiceContainer services;
	World world;
	net::NetworkContext context(services);

	const Entity victim = world.Create();
	world.Emplace<net::NetworkTransform>(victim);
	// Bound to a real net id, unlike the "no NetworkTransform" case above - this is
	// what makes FindHistory actually reach world.FindSystem("NetworkReceiveSystem")
	// instead of bailing out earlier on an unbound entity.
	context.Session().Bind(7, victim);

	const net::RewindSample result = net::RewindTransform(world, context, net::kInvalidConnection, victim);
	CHECK_FALSE(result.hasHistory);
}

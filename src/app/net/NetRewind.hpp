#pragma once

// Lag-compensated hit validation - "shoot where you saw them, not where they are now".
//
// THE PROBLEM. A shooter aims at whatever their own screen shows, which is a remote
// player rendered slightly in the past (see InterpolationBuffer's class remarks). By
// the time their claim ("I hit connection X") reaches the host, the host validates it
// against `victim`'s CURRENT position - which, on any real link, has already moved.
// The claim is honest and still gets rejected, or accepted only through a spatial
// slack constant that has to guess how far is "close enough" in both directions.
//
// THE FIX. NetworkReceiveSystem already keeps a short history of authoritative
// positions per remote entity - the same InterpolationBuffer it renders motion from.
// RewindTransform below reads THAT buffer at the moment the host estimates the
// claimant's own screen was showing, instead of adding a second history mechanism
// that could disagree with the one already driving what players see.
//
// THE HONESTY BOUNDARY, stated once here rather than at every call site:
//   - The rewind time is NEVER a number the caller supplies. It is computed from
//     THIS HOST'S OWN estimate of `viewer`'s view delay - see EstimateViewDelaySeconds -
//     and hard-clamped to kMaxRewindSeconds regardless of how that estimate turns out.
//     A client-supplied timestamp would be a licence to rewind arbitrarily far into the
//     past and claim a hit on wherever the victim used to stand; nothing here accepts one.
//   - This is display information promoted into a validation input, not a second grant
//     of trust: it answers "where would an honest claimant plausibly have seen this",
//     never "believe what the claimant said about the world". A caller still owns every
//     other check (self-hit, range, pacing, one-resolution-per-claim, ...) exactly as it
//     did before this existed.
//   - THE ACCEPTED COST. Rewinding the victim toward where the shooter saw them means the
//     victim CAN be hit slightly after they believe they have already moved away - the
//     same trade every shooter that does this makes. The window is bounded (see
//     kMaxRewindSeconds) precisely so that cost has a ceiling: a target can be hit for
//     compensated lag, never for an unbounded slice of the past.
//   - THE ESTIMATE ITSELF IS APPROXIMATE, not measured. `viewer`'s network leg comes from
//     this host's own round-trip reading (halved, and RTT is not perfectly symmetric);
//     `victim`'s render delay is approximated by the delay THIS host already renders
//     `victim` with, which shares `victim`'s sender but not `viewer`'s own last-mile
//     jitter. Neither of those approximations is corrected for - they are simply capped,
//     via ClampRewindDelaySeconds, at a value small enough that being wrong about them
//     costs at most one bounded window, never an arbitrarily large one.
//   - A DEGRADED LINK DEGRADES, IT DOES NOT BREAK. A `viewer` whose estimated view delay
//     would exceed the cap is clamped down to it rather than refused: RewindTransform
//     still answers, with a position that is somewhat less compensated than that
//     connection's real delay would justify, rather than nothing at all.
//   - NO ENTITY IS REWOUND UNLESS A CALLER ASKS. Nothing here changes what gets
//     rendered, what ResolveTransforms writes, or any existing accept/reject decision -
//     this is a capability a claim-validation path opts into explicitly.

#include <cstdint>

#include <glm/glm.hpp>

#include "net/NetTypes.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
} // namespace aether

namespace aether::net
{
	class NetworkContext;
	class InterpolationBuffer;

	// Hard ceiling on the rewind window, in seconds, no matter how large a connection's
	// estimated view delay turns out to be.
	//
	// Sized from this project's own numbers, not an imported constant: the default send
	// rate is 20 Hz (one interval = 50 ms), InterpolationBuffer's OWN render-delay ceiling
	// is 500 ms (kMaxDelaySeconds in NetInterpolation.hpp), and a broadband link's RTT is
	// typically well under 150 ms, worse only on a genuinely struggling connection. 300 ms
	// covers a poor-but-still-playable link's one-way transit (half of a ~300 ms round
	// trip) plus a meaningful render-delay margin, while staying BELOW the render-delay
	// ceiling alone - so even a link whose jitter alone would justify the full 500 ms of
	// buffering never gets the full benefit of it here. That is a deliberate asymmetry:
	// widening this number trades directly against the victim (a longer window is a
	// longer slice of the past a shot can still land in), so it is held to the tighter of
	// the two costs rather than the more generous one. A project that has measured its
	// own player base and wants a different number changes this one constant; nothing
	// else in this file assumes 300 ms specifically.
	inline constexpr float kMaxRewindSeconds = 0.3f;

	// Clamps a computed view-delay estimate into [0, kMaxRewindSeconds]. Split out from
	// EstimateViewDelaySeconds so the one property that actually matters - an absurd
	// estimate is clamped, never honoured verbatim - is directly testable with no
	// NetworkContext, connection, or entity involved at all.
	[[nodiscard]] float ClampRewindDelaySeconds(float requestedDelaySeconds);

	// This host's own estimate of how far behind `viewer`'s screen the world is right
	// now for `victim`: half of `viewer`'s measured round trip (the network leg) plus
	// `victim`'s own render/interpolation delay (approximated - see the file remarks
	// above). Always run through ClampRewindDelaySeconds before it is returned.
	//
	// 0 for an entity with no NetworkTransform (interpolation was never opted into for
	// it, so there is no render-delay term to add) and 0 for `viewer == kInvalidConnection`
	// offline or on the host's own connection, matching RoundTripMs's own "no link, 0 ms"
	// convention.
	[[nodiscard]] float EstimateViewDelaySeconds(World& world, NetworkContext& context, ConnectionId viewer, Entity victim);

	// What RewindTransform reconstructs, or the explicit admission that it could not.
	struct RewindSample
	{
		// Meaningful only when hasHistory is true; left at the default otherwise, so a
		// caller that forgets to check the flag gets an inert (0,0,0), never a fabricated
		// position that happens to look plausible.
		glm::vec3 position{0.f};
		glm::vec3 rotation{0.f};

		// The view-delay estimate actually used, in seconds, AFTER clamping - what a
		// caller (or a test) reads to tell whether the cap engaged, and the number worth
		// logging or showing on a network debug overlay. Never something to feed back
		// into anything: see the file remarks on why a claimed time is never accepted.
		float appliedDelaySeconds = 0.f;

		// False for exactly the entities NetworkReceiveSystem never buffers history for:
		// not replicated, no NetworkTransform (interpolation is opt-in), owned by this
		// peer (an owner's own state is never buffered - see ResolveTransforms's
		// zero-correction rule), or simply not yet changed since it appeared. The caller
		// then has only the entity's LIVE transform to fall back on and should treat this
		// as "no compensation available", not "the shot happened in the present".
		bool hasHistory = false;
	};

	// The pure sampling step RewindTransform composes with entity/connection
	// resolution: given an already-resolved history buffer (or nullptr - see
	// RewindSample::hasHistory) and the host-time instant to sample it at, produces
	// the reconstructed pose or the honest admission that there is none.
	//
	// Split out so the interpolated-lookup and empty/single-sample cases are
	// testable against a plain InterpolationBuffer, with no World, NetworkContext,
	// or connection involved at all - `history` is READ ONLY here, never pushed to.
	// `hostNow - delaySeconds` is the instant actually sampled; `delaySeconds` is
	// copied verbatim into the result's appliedDelaySeconds.
	[[nodiscard]] RewindSample RewindFromHistory(const InterpolationBuffer* history, float hostNow, float delaySeconds);

	// The capability: reconstructs where `victim` was, from `viewer`'s own point of view,
	// at the moment EstimateViewDelaySeconds above says that view was showing. Read-only -
	// it samples NetworkReceiveSystem's existing interpolation history and touches nothing.
	//
	// Host-only in practice, not by an enforced role check: `viewer`'s round trip and
	// `victim`'s replicated history both come from data only the host accumulates for
	// every connection, so calling this from a client answers with this client's own
	// single link regardless of `viewer` - harmless, but not what "a specific connection's
	// point of view" is asking for. A claim-validation RPC only ever runs on the host
	// anyway (see NetRpcTarget::Server), so this is never reachable from anywhere else in
	// the framework's own model.
	[[nodiscard]] RewindSample RewindTransform(World& world, NetworkContext& context, ConnectionId viewer, Entity victim);
} // namespace aether::net

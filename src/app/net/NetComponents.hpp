#pragma once

#include <cstdint>
#include <string>

#include "net/NetTypes.hpp"
#include "physics2d/Physics2DComponents.hpp"

namespace aether::net
{
	// Marks an entity as replicated and carries its network identity. `netId` is
	// host-assigned and stable for the entity's lifetime; `owner` is the connection
	// allowed to drive it (0 = the host owns it).
	struct NetworkIdentity
	{
		std::uint32_t netId = 0;
		ConnectionId owner = kInvalidConnection;
		// Prefab this entity was spawned from, so a joining client can recreate it.
		// Empty for scene-placed entities, which both ends already have.
		std::string spawnPrefab;
		bool scenePlaced = false;
	};

	// Smoothing for a replicated entity's transform, and it applies to REMOTE
	// entities only: their buffer is rendered some delay in the past, so there is a
	// sample on both sides of the render time to interpolate between and motion is
	// smooth between packets - see NetworkReceiveSystem::ResolveTransforms and
	// InterpolationBuffer::RecommendedDelaySeconds for how that delay is chosen.
	//
	// There is deliberately nothing here for the locally-owned entity. The owner of
	// an entity is authoritative for it, so its own simulation IS the truth and
	// receives no correction of any kind - see NetworkReceiveSystem::ResolveTransforms.
	// The correction-rate and snap-distance knobs that used to live here belonged to
	// the host-authoritative model and were removed with it: a tuning field nothing
	// reads is worse than none.
	struct NetworkTransform
	{
		// With `autoInterpolationDelay` on (the default), this is read only before
		// any interval has been measured for this entity - the first sample of a
		// session, or the first one after a detected stall - and the delay actually
		// rendered with is derived from the observed spacing and jitter of samples
		// as they arrive instead: fresher than a fixed guess on a tight, steady
		// link, and wider than one on a jittery link that needs the margin. See
		// InterpolationBuffer::RecommendedDelaySeconds.
		//
		// Set `autoInterpolationDelay` to false to pin the delay to exactly this
		// value instead, ignoring measurement entirely - a game mode with its own
		// reason to want a fixed, deterministic lag (a replay, a spectator feed
		// synced to a broadcast delay) can still ask for one.
		float interpolationDelaySeconds = 0.1f;
		bool autoInterpolationDelay = true;

		// Dead-reckons this entity forward using the velocity between its last two
		// arrivals instead of only ever showing confirmed data - see
		// InterpolationBuffer::SampleForward. The guess is wrong the instant the
		// entity changes direction; a bounded, decaying correction (also
		// SampleForward) turns that into a brief smooth slide onto the right
		// position rather than a visible snap - it is still a GUESS, never a claim
		// anything trusts: NetworkReceiveSystem::ResolveTransforms applies zero
		// correction of any kind to a locally-owned entity, and this setting only
		// ever touches how someone ELSE's entity is drawn.
		//
		// Defaults ON for the same reason auto-delay defaults on: freshness is the
		// common case worth having without asking for it, and the guess is bounded
		// and self-correcting rather than free-running. Turn it off per entity for
		// anything whose exact recent position matters more than its freshness -
		// something that changes direction erratically at close range, or a replay
		// /spectator feed that would rather hold the last real sample than guess.
		bool extrapolate = true;

		// How much of the render delay to spend as a PROACTIVE guess every frame,
		// shrinking how far in the past this entity is rendered instead of only
		// projecting forward when a packet happens to be late - see
		// NetworkReceiveSystem::ResolveTransforms, which computes
		// `effectiveDelay = delay - min(delay, budget)` and renders at
		// `now - effectiveDelay`, `budget` itself computed below according to
		// `autoExtrapolationBudget`.
		//
		// The delay this spends from is roughly (smoothed interval + 4x jitter) -
		// see InterpolationBuffer::RecommendedDelaySeconds. The INTERVAL term exists
		// purely because samples arrive discretely, and extrapolation substitutes
		// for it cleanly on ordinary, roughly-steady motion; the JITTER term exists
		// to protect against a genuinely late packet, and spending it too means a
		// real hiccup more often falls back on the cap/freeze instead of a full
		// margin's worth of real buffered slack. The cost of spending EITHER is
		// CONTINUOUS, not occasional: every frame a budget is spent, a chance of an
		// actual direction change since the last sample is on screen as a
		// (bounded, corrected) guess rather than confirmed data.
		//
		// With `autoExtrapolationBudget` on (the default), the budget is
		// SELF-TUNING: `extrapolationBudgetFraction` (default 1.0, i.e. all of it)
		// times InterpolationBuffer::MeasuredIntervalSeconds() - the entity's own
		// observed spacing, not a guessed constant. Spending exactly the interval
		// and leaving the jitter term untouched is principled and self-correcting
		// regardless of link or send rate: on a tight, low-jitter 60Hz LAN link the
		// interval is tiny and the render ends up close to the present with a small
		// residual jitter margin; on a bad link the (large) jitter term stays fully
		// protected either way. In both cases the render floor this converges to is
		// exactly the jitter margin (delay - interval = 4*jitter, algebraically),
		// which is the real, physical bound on how fresh a display-only guess can
		// responsibly be - see the send-rate/staleness numbers in the report for
		// this batch. Lower `extrapolationBudgetFraction` to spend less than the
		// full interval and keep some of it as buffered margin too, at the cost of
		// a smaller freshness gain.
		//
		// Set `autoExtrapolationBudget` to false to PIN the budget to
		// `extrapolationBudgetSeconds` instead (default 0.f - extrapolation then
		// only ever covers a packet that is actually late, its original, most
		// conservative shape) - a game with its own reason to want a fixed,
		// deterministic number rather than one that moves with the measured
		// interval can still ask for one. Either way the budget is clamped to
		// `maxExtrapolationSeconds` below before it is spent, so it can never ask
		// the projection to exceed its own cap, and can never drive the effective
		// delay negative.
		bool autoExtrapolationBudget = true;
		float extrapolationBudgetFraction = 1.0f;
		float extrapolationBudgetSeconds = 0.f;

		// How far past the newest sample this entity may ever be projected, in
		// seconds - the hard ceiling on ANY single projection, whether it comes
		// from the proactive budget above, a genuinely late packet, or both at
		// once. Past this it FREEZES at the position the projection had reached AT
		// the cap - it does not keep sliding further from the last real
		// observation, and it does not jump back to the raw last sample either,
		// which would be its own visible pop the instant the cap is crossed.
		//
		// 0.15s is three send intervals at the framework's default 20Hz
		// (NetworkContext::SendRateHz): enough to bridge one dropped packet plus
		// ordinary jitter without a visible stutter, short enough that a genuinely
		// stopped-sending entity freezes within a third of a second rather than
		// coasting off into the distance. A fast, mostly-straight-line mover (a
		// thrown projectile) can afford a larger cap than a player character that
		// jukes - hence per-entity rather than a single framework-wide constant.
		float maxExtrapolationSeconds = 0.15f;
	};

	// A connected player's display name and link quality. Framework-level rather than
	// game-level: name tags, chat attribution and disconnect notices all read this one
	// field instead of each tracking names separately.
	//
	// Both fields are REPLICATED and both are AUTHORED BY THE OWNER, which is the only
	// arrangement that works under client authority - a peer writing either of these on
	// somebody else's player is writing a value that player's owner overwrites on its
	// next send. `displayName` learned that the hard way (see Net.SetPlayerName's
	// refusal), and `pingMs` is the same shape of state: only the peer at one end of a
	// link knows what that link costs, so only it may say.
	struct NetPlayer
	{
		std::string displayName;

		// Round-trip time to the HOST, in milliseconds, as measured by this player's
		// own peer. 0 on the host's own player, which has no link to itself, and 0
		// offline - both of which read correctly as "no latency to speak of".
		//
		// Stamped by NetworkSendSystem on the entities this peer owns, so a game gets a
		// per-player ping on every peer with no code of its own and no second message
		// on the wire. Replication carries it outward like any other owned field.
		std::uint32_t pingMs = 0;
	};

	// Present on a replicated entity whose 2D body this peer has taken OFF local
	// simulation because it is not authoritative for it - see
	// NetworkContext::SyncSimulationAuthority. Runtime-only and deliberately
	// unreflected: it is never authored, never serialized, and never replicated.
	//
	// It carries the authored body type so the switch is reversible, and its mere
	// PRESENCE is what makes the switch idempotent: an entity already carrying one
	// is already handed over, so the reconcile pass leaves it alone instead of
	// rebuilding its body every frame.
	struct NetSimulationOverride
	{
		Body2DType authoredBodyType = Body2DType::Dynamic;
	};
} // namespace aether::net

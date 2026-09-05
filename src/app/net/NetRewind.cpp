#include "net/NetRewind.hpp"

#include <algorithm>
#include <optional>

#include "net/NetComponents.hpp"
#include "net/NetInterpolation.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	namespace
	{
		// Reaches the per-entity interpolation history NetworkReceiveSystem already
		// keeps for a remote entity - the SAME buffer ResolveTransforms samples to
		// decide what to render (see NetworkSystems.cpp). Reused rather than duplicated:
		// two independent notions of "where has this entity been" would eventually
		// disagree, and only one of them is what a player's screen actually showed.
		//
		// nullptr for an entity that is not replicated, has no NetworkTransform (the
		// history only exists for entities that opted into interpolation), is owned by
		// this peer (an owner's own state is never buffered), or has no
		// NetworkReceiveSystem registered at all (a headless/offline world).
		const InterpolationBuffer* FindHistory(World& world, NetworkContext& context, Entity entity)
		{
			const std::uint32_t netId = context.Session().NetIdFor(entity);
			if (netId == 0)
			{
				return nullptr;
			}
			auto* receive = static_cast<NetworkReceiveSystem*>(world.FindSystem("NetworkReceiveSystem"));
			return receive != nullptr ? receive->FindRemoteBuffer(netId) : nullptr;
		}
	} // namespace

	float ClampRewindDelaySeconds(float requestedDelaySeconds)
	{
		return std::clamp(requestedDelaySeconds, 0.f, kMaxRewindSeconds);
	}

	float EstimateViewDelaySeconds(World& world, NetworkContext& context, ConnectionId viewer, Entity victim)
	{
		// Half the measured round trip: the one-way cost of a snapshot travelling from
		// this host to `viewer`, which is the leg that actually delays what `viewer`'s
		// screen can be showing right now. ENet reports a round trip, not the two legs
		// separately, and a real link is rarely perfectly symmetric - halving it is an
		// estimate, not a measurement, which is exactly why the total is clamped below
		// rather than trusted outright.
		const float oneWaySeconds = (static_cast<float>(context.RoundTripMs(viewer)) / 1000.f) * 0.5f;

		// `victim`'s own render delay, approximated by the delay THIS host already
		// renders `victim` with - the identical formula ResolveTransforms uses (see
		// NetworkSystems.cpp), because both this host and `viewer` receive `victim`'s
		// updates at the same cadence from the same sender. It is only a stand-in:
		// `viewer`'s own last-mile jitter is its own and unobservable from here. Zero
		// for an entity with no NetworkTransform - interpolation was never opted into
		// for it, so there is no render-delay term to add.
		float renderDelaySeconds = 0.f;
		if (const auto* tuning = world.TryGet<NetworkTransform>(victim))
		{
			const float anchor = std::max(0.f, tuning->interpolationDelaySeconds);
			if (tuning->autoInterpolationDelay)
			{
				const InterpolationBuffer* history = FindHistory(world, context, victim);
				renderDelaySeconds = history != nullptr ? history->RecommendedDelaySeconds(anchor) : anchor;
			}
			else
			{
				renderDelaySeconds = anchor;
			}
		}

		return ClampRewindDelaySeconds(oneWaySeconds + renderDelaySeconds);
	}

	RewindSample RewindFromHistory(const InterpolationBuffer* history, float hostNow, float delaySeconds)
	{
		RewindSample result;
		result.appliedDelaySeconds = delaySeconds;
		if (history == nullptr)
		{
			return result; // hasHistory stays false - see RewindSample's remarks
		}

		const std::optional<TransformSample> sample = history->Sample(hostNow - delaySeconds);
		if (!sample.has_value())
		{
			// An entry exists but has never been pushed to - see InterpolationBuffer::
			// Sample's own empty-buffer case. Same admission as no entry at all.
			return result;
		}

		result.position = sample->position;
		result.rotation = sample->rotation;
		result.hasHistory = true;
		return result;
	}

	RewindSample RewindTransform(World& world, NetworkContext& context, ConnectionId viewer, Entity victim)
	{
		// Computed and clamped BEFORE the history lookup, so a caller diagnosing "why
		// did this claim get no compensation" can still see what window would have
		// applied even on the hasHistory == false path.
		const float delaySeconds = EstimateViewDelaySeconds(world, context, viewer, victim);
		return RewindFromHistory(FindHistory(world, context, victim), context.Now(), delaySeconds);
	}
} // namespace aether::net

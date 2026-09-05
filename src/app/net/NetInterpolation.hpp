#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace aether::net
{
	struct TransformSample
	{
		float time = 0.f;
		glm::vec3 position{0.f};
		glm::vec3 rotation{0.f};
	};

	// Holds recent authoritative samples for one remote entity so it can be rendered
	// slightly in the past, which is what turns discrete packets into smooth motion.
	//
	// It also tracks the timing of its OWN arrivals (the `time` field of every
	// pushed sample, which is the receiver's clock, not the sender's) and derives a
	// recommended interpolation delay from them - see RecommendedDelaySeconds. Doing
	// that HERE rather than one level up keeps the statistic where its inputs
	// already live: NetworkReceiveSystem keeps exactly one of these per remote
	// entity, and a per-entity estimate is the only granularity that stays correct
	// on a host, where different replicated entities can be owned by, and therefore
	// arrive over, entirely different connections - a single shared/per-connection
	// estimate would blend links that have nothing to do with each other. The cost
	// is three floats and a comparison per entity, negligible next to the sample
	// buffer already kept here.
	class InterpolationBuffer
	{
	public:
		// Inserts in time order: UDP delivers out of order, and a late sample still
		// carries information about the interval it belongs to. Also feeds the
		// arrival-timing tracker behind RecommendedDelaySeconds - see there for how a
		// duplicate or out-of-order `time` (this insert lands before the current
		// back()) is handled: it is kept in the buffer but excluded from the timing
		// statistics, since it does not describe a new arrival on the live end of
		// the stream.
		void Push(const TransformSample& sample);

		// Interpolates between the samples bracketing `renderTime`. Outside the
		// buffer's range it holds the nearest end - extrapolating a player forward
		// sends them through walls, and a brief freeze reads better than a rubber-band.
		[[nodiscard]] std::optional<TransformSample> Sample(float renderTime) const;

		// How far in the past to render this entity, in the same time space as
		// TransformSample::time, derived from the OBSERVED spacing and jitter of
		// samples actually pushed rather than a fixed guess:
		//
		//   - Before any interval has been measured - the first sample of a
		//     session, or the first one after a detected stall - there is nothing
		//     to derive from, so this returns `anchorSeconds` outright. That is the
		//     same fixed value the framework used before this existed, read live
		//     rather than cached, so a runtime change to the tuning field takes
		//     effect on the next call.
		//   - Once an interval is established, the delay settles toward (smoothed
		//     interval + a jitter margin), clamped to [kMinDelaySeconds,
		//     kMaxDelaySeconds]: never so small that ordinary jitter starves the
		//     buffer of a bracketing pair, never so large that one bad link makes an
		//     entity unplayably laggy.
		//   - The settle is deliberate, not instantaneous: see Push for the
		//     smoothing, the stall reset, and the per-sample slew limit that keep a
		//     single spike from being visible as its own motion artefact.
		//
		// `anchorSeconds` is also what a project pins the delay to outright by
		// turning auto-delay off - see NetworkTransform - which never calls this at
		// all; it is not a floor on the auto-computed value; the sole point of
		// letting it fall below `anchorSeconds` is what makes a tight, steady link
		// render fresher than a one-size-fits-all guess.
		[[nodiscard]] float RecommendedDelaySeconds(float anchorSeconds) const;

		void Clear();

		[[nodiscard]] std::size_t Size() const
		{
			return m_samples.size();
		}

	private:
		void TrackArrival(float time);

		// A few seconds at typical send rates. Bounded so a long session cannot grow
		// this without limit.
		static constexpr std::size_t kMaxSamples = 64;

		// Never smaller than this even on a perfectly steady, zero-jitter link: a
		// buffer with exactly zero slack starves the instant one packet is a fraction
		// of a millisecond late, which is the normal case, not the exceptional one.
		static constexpr float kMinDelaySeconds = 0.02f;
		// Never larger than this regardless of how bad the measured jitter gets. Past
		// this point Sample() already holds the last position rather than reaching
		// for state that far in the past, so a bigger ceiling would not even smooth
		// anything further - it would only add latency to a link that is already
		// struggling.
		static constexpr float kMaxDelaySeconds = 0.5f;
		// interval + N * mean-deviation is the standard RFC 3550-style sizing for a
		// playout buffer; 4 mean-deviations covers the large majority of arrival
		// variance without chasing its full tail.
		static constexpr float kJitterMultiplier = 4.f;
		// A gap this many multiples of the established interval - or this many
		// seconds, whichever is larger, so a very tight link's normal jitter is never
		// mistaken for one - is not "the link got slower", it is a stall: a paused
		// game, a scene load, or simply an idle entity that stopped sending changes
		// for a while. Folding it into the running average would spike the estimate
		// by however long the gap lasted and then take many samples at
		// kIntervalSmoothingAlpha's weight each to decay back down.
		static constexpr float kStallIntervalMultiplier = 8.f;
		static constexpr float kStallAbsoluteSeconds = 1.f;
		// Jacobson's classic RTO smoothing constant: slow enough that one outlier
		// packet cannot swing the estimate, fast enough to track a real link change
		// inside a handful of seconds.
		static constexpr float kIntervalSmoothingAlpha = 0.125f;
		// The settled delay may move by at most this fraction of the current
		// smoothed interval per ACCEPTED sample (a stall reset or a duplicate/late
		// arrival do not count). Bounding the step in the interval's own units keeps
		// the limit meaningful at any send rate, and forcing a spike to take several
		// samples to fully land is what keeps the rendered delay from being a second
		// source of visible motion artefacts on top of the jitter it exists to hide.
		static constexpr float kMaxDelayStepFraction = 0.5f;

		std::vector<TransformSample> m_samples;

		bool m_hasLastArrival = false;
		float m_lastArrivalTime = 0.f;
		bool m_hasIntervalEstimate = false;
		float m_smoothedIntervalSeconds = 0.f;
		float m_jitterSeconds = 0.f;
		bool m_hasCurrentDelay = false;
		float m_currentDelaySeconds = 0.f;
	};
} // namespace aether::net

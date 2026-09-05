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
	// close to present time - either interpolated inside the buffer, or dead-reckoned
	// forward past its newest sample once the render clock has caught up to it. See
	// SampleForward for the forward projection and its bound, and Sample for the
	// unextrapolated form NetRewind's hit-validation history queries still use
	// verbatim - see the shared-ownership note on that method.
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
		// the stream. The same exclusion applies to the velocity trend and the
		// misprediction correction behind SampleForward, and for the same reason: a
		// stale packet delivered late describes a moment already superseded, not a
		// new point on the entity's actual trend.
		void Push(const TransformSample& sample);

		// Interpolates between the samples bracketing `renderTime`. Outside the
		// buffer's range it holds the nearest end - extrapolating a player forward
		// sends them through walls, and a brief freeze reads better than a rubber-band.
		//
		// UNCHANGED signature and meaning even after SampleForward below was added:
		// this is the form NetRewind's hit-validation rewind uses to ask where an
		// entity WAS at a past instant, and a rewind must never receive a
		// forward-projected guess in place of history.
		[[nodiscard]] std::optional<TransformSample> Sample(float renderTime) const;

		// Like Sample(), but for RENDERING rather than history: past the newest
		// sample it dead-reckons the entity forward using the velocity between the
		// last two GENUINE arrivals instead of freezing on the spot, so a remote
		// entity can be shown close to present time instead of a fixed delay behind
		// it - see the class comment. Two bounds keep the guess from ever reading
		// as wrong:
		//
		//   - The projection is capped at `maxExtrapolationSeconds` past the newest
		//     sample. Past the cap the entity FREEZES at the position the
		//     projection had reached AT the cap - it does not keep sliding further
		//     from the last real observation (which would visibly run away from
		//     where the entity actually is), and it does not jump back to the raw
		//     last sample either (its own visible pop the instant the cap is
		//     crossed).
		//   - Every result also carries a bounded, DECAYING correction: the moment
		//     a new sample disagrees with what the trend had projected for it (an
		//     entity that changed direction, most obviously), the difference is
		//     folded into an offset that fades to exactly zero over
		//     kCorrectionWindowSeconds instead of being applied all at once - see
		//     UpdateMotion. That is what turns a misprediction into a brief, smooth
		//     slide onto the correct position instead of a teleport, and because
		//     the offset always decays to zero on its own clock, a caller sampling
		//     well after the correction pays no added lag for it: this is a rare,
		//     self-terminating repair, not a second, permanent interpolation delay.
		//
		// Below the newest sample this is identical to Sample(). With fewer than
		// two samples, or before any interval has established a trend, there is no
		// velocity to derive and this also matches Sample()'s frozen hold exactly.
		[[nodiscard]] std::optional<TransformSample> SampleForward(float renderTime, float maxExtrapolationSeconds) const;

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

		// The interval component of RecommendedDelaySeconds' own sizing, with NO
		// jitter margin added - the smoothed inter-arrival spacing this entity has
		// actually measured, or 0.f before any interval has been established (the
		// same "nothing to derive from yet" window RecommendedDelaySeconds' own doc
		// names, and reset on the same stall). Exists so a caller can spend exactly
		// the discreteness-driven part of a chosen delay proactively - extrapolation
		// substitutes for that part cleanly - while leaving the jitter-driven
		// remainder as real, unspent margin against a genuinely late packet; see
		// NetworkTransform::extrapolationBudgetFraction for where this is consumed.
		[[nodiscard]] float MeasuredIntervalSeconds() const
		{
			return m_hasIntervalEstimate ? m_smoothedIntervalSeconds : 0.f;
		}

		void Clear();

		[[nodiscard]] std::size_t Size() const
		{
			return m_samples.size();
		}

	private:
		void TrackArrival(const TransformSample& sample);

		// Updates the velocity trend and the misprediction-correction offset behind
		// SampleForward. Called from TrackArrival for every GENUINE new arrival -
		// never a duplicate, an out-of-order straggler, or the arrival right after a
		// detected stall, all of which TrackArrival already filters out before
		// calling this, because none of them describe the next point on the
		// entity's live trend.
		//
		// The correction is computed only once a velocity ALREADY existed before
		// this arrival - i.e. from the third genuine sample onward. The transition
		// from the very first held sample into the first real interpolation is
		// already continuous by construction (Sample() ramps FROM that exact
		// point), so there is nothing yet to correct on the second sample; treating
		// it as a misprediction would manufacture a correction for a jump that was
		// never actually visible.
		void UpdateMotion(const TransformSample& sample, float dt);

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
		// How long a misprediction correction takes to fade to zero once a new
		// sample lands and disagrees with the trend - see UpdateMotion and
		// SampleForward. Long enough to genuinely read as a slide rather than a
		// snap; short enough that a wrong guess is fully gone well inside a second,
		// so it can never be mistaken for a second, permanent source of render lag
		// stacked on top of RecommendedDelaySeconds.
		static constexpr float kCorrectionWindowSeconds = 0.15f;

		std::vector<TransformSample> m_samples;

		bool m_hasLastArrival = false;
		float m_lastArrivalTime = 0.f;
		bool m_hasIntervalEstimate = false;
		float m_smoothedIntervalSeconds = 0.f;
		float m_jitterSeconds = 0.f;
		bool m_hasCurrentDelay = false;
		float m_currentDelaySeconds = 0.f;

		// The trend SampleForward projects forward from: the rate of change between
		// the last two GENUINE arrivals (see UpdateMotion), not simply the last two
		// entries in m_samples - an out-of-order straggler inserted between them
		// must not be allowed to silently change what "the trend" means.
		bool m_hasVelocity = false;
		glm::vec3 m_velocityPosition{0.f};
		glm::vec3 m_velocityRotation{0.f};

		// The still-decaying gap between a trend's guess and the truth that
		// disproved it - see UpdateMotion for where it is set and SampleForward for
		// where it is applied and how it fades out.
		bool m_hasCorrection = false;
		glm::vec3 m_correctionPosition{0.f};
		glm::vec3 m_correctionRotation{0.f};
		float m_correctionTime = 0.f;
	};
} // namespace aether::net

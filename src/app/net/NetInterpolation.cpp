#include "net/NetInterpolation.hpp"

#include <algorithm>
#include <cmath>

namespace aether::net
{
	void InterpolationBuffer::Push(const TransformSample& sample)
	{
		// upper_bound, NOT lower_bound: two samples can legitimately carry the same
		// timestamp, because ResolveTransforms captures `now` once per call and keys every
		// push it makes to that one value. lower_bound inserts at the first element not
		// less than the key, which places the NEWER sample before the older one - and
		// Sample() clamps to back(), so the entity renders the stale value. Once the sender
		// stops moving, no further delta is ever sent and it stays stale permanently.
		const auto at = std::upper_bound(m_samples.begin(), m_samples.end(), sample.time,
		        [](float t, const TransformSample& s) { return t < s.time; });
		m_samples.insert(at, sample);

		if (m_samples.size() > kMaxSamples)
		{
			m_samples.erase(m_samples.begin(), m_samples.begin() + static_cast<std::ptrdiff_t>(m_samples.size() - kMaxSamples));
		}

		TrackArrival(sample);
	}

	void InterpolationBuffer::TrackArrival(const TransformSample& sample)
	{
		if (!m_hasLastArrival)
		{
			// The very first sample of the session: there is exactly one arrival on
			// record, which is a point, not yet an interval.
			m_hasLastArrival = true;
			m_lastArrivalTime = sample.time;
			return;
		}

		const float raw = sample.time - m_lastArrivalTime;
		if (raw <= 0.f)
		{
			// A duplicate (raw == 0 - two channels landing the same tick, or the
			// same-timestamp case Push's own doc calls out) or a genuinely
			// out-of-order arrival (raw < 0 - a stale packet delivered after a newer
			// one). Push() still re-sorts it into the buffer; it tells the timing
			// tracker nothing new about the CURRENT interval, so it must not perturb
			// the running estimate, the velocity trend, or the correction offset.
			// m_lastArrivalTime is left alone: it already holds the latest real
			// arrival, which this sample is not.
			return;
		}

		// A gap far past the established rhythm - a paused game, a scene load, a
		// sender (or an idle, unchanging entity) that has been quiet a while - is not
		// a bigger interval, it is the ABSENCE of a rhythm. Checked BEFORE
		// `m_lastArrivalTime` moves, against the estimate this gap is being measured
		// from.
		const bool stalled = m_hasIntervalEstimate
		        && raw > std::max(kStallIntervalMultiplier * m_smoothedIntervalSeconds, kStallAbsoluteSeconds);
		m_lastArrivalTime = sample.time;

		if (stalled)
		{
			// Forget the estimate entirely rather than seeding it from the gap - the
			// gap's length describes how long nothing arrived, not what a normal
			// interval looks like. This arrival becomes the new baseline point,
			// exactly like the very first sample of a session; the delay falls back
			// to the anchor until the NEXT arrival measures a real interval against
			// it. The alternative - folding the gap in - would spike the smoothed
			// interval by however long the stall lasted and then take many samples
			// at kIntervalSmoothingAlpha's weight each to decay back down.
			//
			// The velocity trend and any in-flight correction are forgotten for the
			// same reason: dead reckoning across a multi-second gap was never
			// bridging anything real, and SampleForward's own cap already stopped
			// projecting long before this arrival - there is no continuity here
			// worth preserving.
			m_hasIntervalEstimate = false;
			m_hasCurrentDelay = false;
			m_hasVelocity = false;
			m_hasCorrection = false;
			return;
		}

		UpdateMotion(sample, raw);

		if (!m_hasIntervalEstimate)
		{
			// Nothing established yet - the first interval ever measured, or the
			// first one after a stall - so there is no average to smooth into; adopt
			// it outright as the new baseline.
			m_smoothedIntervalSeconds = raw;
			m_jitterSeconds = 0.f;
			m_hasIntervalEstimate = true;
		}
		else
		{
			const float delta = raw - m_smoothedIntervalSeconds;
			m_smoothedIntervalSeconds += kIntervalSmoothingAlpha * delta;
			m_jitterSeconds += kIntervalSmoothingAlpha * (std::abs(delta) - m_jitterSeconds);
		}

		const float target = std::clamp(m_smoothedIntervalSeconds + kJitterMultiplier * m_jitterSeconds,
		        kMinDelaySeconds, kMaxDelaySeconds);

		if (!m_hasCurrentDelay)
		{
			// Nothing to settle FROM yet: adopt the first real measurement of this
			// run outright rather than crawling toward it from a stale guess.
			m_currentDelaySeconds = target;
			m_hasCurrentDelay = true;
		}
		else
		{
			// Slew-limit the move so one noisy sample cannot itself become a visible
			// pop in the render delay - see kMaxDelayStepFraction.
			const float maxStep = std::max(kMinDelaySeconds, kMaxDelayStepFraction * m_smoothedIntervalSeconds);
			const float step = std::clamp(target - m_currentDelaySeconds, -maxStep, maxStep);
			m_currentDelaySeconds += step;
		}
	}

	void InterpolationBuffer::UpdateMotion(const TransformSample& sample, float dt)
	{
		if (m_samples.size() < 2)
		{
			// This sample IS the second entry ever pushed (the first already sits at
			// m_samples[0]); nothing has produced a velocity yet to correct against -
			// see the class comment on why the second sample is exempt.
			return;
		}
		if (dt <= 1e-6f)
		{
			// Defensive: TrackArrival's own `raw > 0` guard should already ensure
			// this never fires, but a near-zero divisor here would poison the
			// velocity estimate with a huge or infinite value.
			return;
		}

		// The previous GENUINE arrival, not simply "the second-to-last entry" - by
		// construction (see Push and TrackArrival) they are the same element here,
		// because this branch only runs for a sample that inserted at the true back
		// of the buffer, and an out-of-order straggler between them never reaches
		// this far.
		const TransformSample& previous = m_samples[m_samples.size() - 2];

		if (m_hasVelocity)
		{
			// What SampleForward would have projected for this exact instant, using
			// the trend established before this arrival landed - i.e. what was
			// actually on screen a moment ago, before this packet either confirmed
			// or defeated it.
			glm::vec3 predictedPosition = previous.position + m_velocityPosition * dt;
			glm::vec3 predictedRotation = previous.rotation + m_velocityRotation * dt;
			if (m_hasCorrection)
			{
				// Whatever of an EARLIER correction is still fading in is folded in
				// too, so a second misprediction landing before the first has fully
				// blended out continues from what is actually on screen rather than
				// discarding the remainder and popping to a fresh blend.
				const float elapsed = sample.time - m_correctionTime;
				const float weight = std::clamp(1.f - elapsed / kCorrectionWindowSeconds, 0.f, 1.f);
				predictedPosition += m_correctionPosition * weight;
				predictedRotation += m_correctionRotation * weight;
			}

			// The gap between that guess and the truth that just arrived becomes the
			// new correction, blended out by SampleForward instead of applied
			// instantly - see the class comment on SampleForward.
			m_correctionPosition = predictedPosition - sample.position;
			m_correctionRotation = predictedRotation - sample.rotation;
			m_correctionTime = sample.time;
			m_hasCorrection = true;
		}

		if (sample.hasVelocity)
		{
			// Authoritative, not derived: the sender's own physics body already
			// knows this, more precisely and with none of the noise two position
			// samples a send-interval apart carry. `rotation` is Euler degrees;
			// `angularVelocity` is radians/second axis-angle, which the linear
			// extrapolation below (predictedRotation = previous.rotation + rate *
			// dt) already treats as a small-angle rate - the same precision
			// Sample()'s plain glm::mix interpolation already accepts elsewhere in
			// this class, and bounded the same way every other imperfect trend
			// here is: SampleForward's extrapolation cap, plus this very
			// correction mechanism healing the gap against the next real arrival.
			// See TransformSample::angularVelocity's own comment for why a full
			// quaternion conversion was not done instead.
			m_velocityPosition = sample.linearVelocity;
			m_velocityRotation = glm::degrees(sample.angularVelocity);
		}
		else
		{
			m_velocityPosition = (sample.position - previous.position) / dt;
			m_velocityRotation = (sample.rotation - previous.rotation) / dt;
		}
		m_hasVelocity = true;
	}

	float InterpolationBuffer::RecommendedDelaySeconds(float anchorSeconds) const
	{
		return m_hasCurrentDelay ? m_currentDelaySeconds : anchorSeconds;
	}

	std::optional<TransformSample> InterpolationBuffer::Sample(float renderTime) const
	{
		if (m_samples.empty())
		{
			return std::nullopt;
		}
		if (renderTime <= m_samples.front().time)
		{
			return m_samples.front();
		}
		if (renderTime >= m_samples.back().time)
		{
			return m_samples.back();
		}

		for (std::size_t i = 1; i < m_samples.size(); ++i)
		{
			const TransformSample& b = m_samples[i];
			if (b.time < renderTime)
			{
				continue;
			}
			const TransformSample& a = m_samples[i - 1];
			const float span = b.time - a.time;
			const float t = span > 1e-6f ? (renderTime - a.time) / span : 0.f;
			TransformSample out;
			out.time = renderTime;
			out.position = glm::mix(a.position, b.position, t);
			out.rotation = glm::mix(a.rotation, b.rotation, t);
			return out;
		}
		return m_samples.back();
	}

	std::optional<TransformSample> InterpolationBuffer::SampleForward(float renderTime, float maxExtrapolationSeconds) const
	{
		if (m_samples.empty())
		{
			return std::nullopt;
		}

		std::optional<TransformSample> base;
		if (renderTime <= m_samples.back().time || m_samples.size() < 2 || !m_hasVelocity)
		{
			// Within the buffer, or nothing established to extrapolate FROM (a lone
			// sample, or no trend measured yet) - identical to the plain Sample()
			// answer.
			base = Sample(renderTime);
		}
		else
		{
			const TransformSample& newest = m_samples.back();
			const float ahead = std::clamp(renderTime - newest.time, 0.f, std::max(0.f, maxExtrapolationSeconds));
			TransformSample projected;
			projected.time = renderTime;
			projected.position = newest.position + m_velocityPosition * ahead;
			projected.rotation = newest.rotation + m_velocityRotation * ahead;
			base = projected;
		}

		if (m_hasCorrection)
		{
			// Bounded, decaying repair for a recent misprediction - see UpdateMotion
			// for where this is set and the class comment above for why it cannot
			// become a permanent source of lag: `weight` reaches exactly zero
			// kCorrectionWindowSeconds after it was set, not asymptotically.
			const float elapsed = renderTime - m_correctionTime;
			const float weight = std::clamp(1.f - elapsed / kCorrectionWindowSeconds, 0.f, 1.f);
			base->position += m_correctionPosition * weight;
			base->rotation += m_correctionRotation * weight;
		}
		return base;
	}

	void InterpolationBuffer::Clear()
	{
		m_samples.clear();
		m_hasLastArrival = false;
		m_lastArrivalTime = 0.f;
		m_hasIntervalEstimate = false;
		m_smoothedIntervalSeconds = 0.f;
		m_jitterSeconds = 0.f;
		m_hasCurrentDelay = false;
		m_currentDelaySeconds = 0.f;
		m_hasVelocity = false;
		m_velocityPosition = glm::vec3{0.f};
		m_velocityRotation = glm::vec3{0.f};
		m_hasCorrection = false;
		m_correctionPosition = glm::vec3{0.f};
		m_correctionRotation = glm::vec3{0.f};
		m_correctionTime = 0.f;
	}
} // namespace aether::net

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

		TrackArrival(sample.time);
	}

	void InterpolationBuffer::TrackArrival(float time)
	{
		if (!m_hasLastArrival)
		{
			// The very first sample of the session: there is exactly one arrival on
			// record, which is a point, not yet an interval.
			m_hasLastArrival = true;
			m_lastArrivalTime = time;
			return;
		}

		const float raw = time - m_lastArrivalTime;
		if (raw <= 0.f)
		{
			// A duplicate (raw == 0 - two channels landing the same tick, or the
			// same-timestamp case Push's own doc calls out) or a genuinely
			// out-of-order arrival (raw < 0 - a stale packet delivered after a newer
			// one). Push() still re-sorts it into the buffer; it tells the timing
			// tracker nothing new about the CURRENT interval, so it must not perturb
			// the running estimate. m_lastArrivalTime is left alone: it already holds
			// the latest real arrival, which this sample is not.
			return;
		}

		// A gap far past the established rhythm - a paused game, a scene load, a
		// sender (or an idle, unchanging entity) that has been quiet a while - is not
		// a bigger interval, it is the ABSENCE of a rhythm. Checked BEFORE
		// `m_lastArrivalTime` moves, against the estimate this gap is being measured
		// from.
		const bool stalled = m_hasIntervalEstimate
		        && raw > std::max(kStallIntervalMultiplier * m_smoothedIntervalSeconds, kStallAbsoluteSeconds);
		m_lastArrivalTime = time;

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
			m_hasIntervalEstimate = false;
			m_hasCurrentDelay = false;
			return;
		}

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
	}
} // namespace aether::net

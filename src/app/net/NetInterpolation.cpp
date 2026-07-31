#include "net/NetInterpolation.hpp"

#include <algorithm>

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
	}
} // namespace aether::net

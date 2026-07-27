#include "net/NetInterpolation.hpp"

#include <algorithm>
#include <cmath>

namespace aether::net
{
	void InterpolationBuffer::Push(const TransformSample& sample)
	{
		const auto at = std::lower_bound(m_samples.begin(), m_samples.end(), sample.time,
		        [](const TransformSample& s, float t) { return s.time < t; });
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

	glm::vec3 EaseToward(glm::vec3 current, glm::vec3 target, float rate, float dt, float snapDistance)
	{
		const glm::vec3 delta = target - current;
		if (glm::dot(delta, delta) >= snapDistance * snapDistance)
		{
			return target;
		}
		// Exponential decay, so the result does not depend on frame rate.
		const float t = 1.f - std::exp(-std::max(rate, 0.f) * std::max(dt, 0.f));
		return current + delta * t;
	}
} // namespace aether::net

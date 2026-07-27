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
	class InterpolationBuffer
	{
	public:
		// Inserts in time order: UDP delivers out of order, and a late sample still
		// carries information about the interval it belongs to.
		void Push(const TransformSample& sample);

		// Interpolates between the samples bracketing `renderTime`. Outside the
		// buffer's range it holds the nearest end - extrapolating a player forward
		// sends them through walls, and a brief freeze reads better than a rubber-band.
		[[nodiscard]] std::optional<TransformSample> Sample(float renderTime) const;

		void Clear();

		[[nodiscard]] std::size_t Size() const
		{
			return m_samples.size();
		}

	private:
		// A few seconds at typical send rates. Bounded so a long session cannot grow
		// this without limit.
		static constexpr std::size_t kMaxSamples = 64;
		std::vector<TransformSample> m_samples;
	};

	// Frame-rate independent ease toward an authoritative position. Past
	// `snapDistance` it cuts instead: gliding a player across a large error looks
	// far worse than a single jump, and usually means they were teleported anyway.
	[[nodiscard]] glm::vec3 EaseToward(glm::vec3 current, glm::vec3 target, float rate, float dt, float snapDistance);
} // namespace aether::net

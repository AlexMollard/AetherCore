#pragma once

#include <cstdint>
#include <string>
#include <glm/vec3.hpp>
#include "io/IOPriority.hpp"

namespace aether::streaming
{
	using RequestId = std::uint64_t;

	enum class State : std::uint32_t
	{
		Unknown,
		Queued,
		Loading,
		Uploading,
		Loaded,
		Active,
		Saving,
		Retiring,
		Cancelled,
		Failed,
		Blocked
	};

	enum class Demand : std::uint8_t
	{
		None,
		Prefetch,
		Active
	};

	struct Budgets
	{
		std::uint64_t cpuBytes = 256ull * 1024 * 1024;
		std::uint64_t gpuBytes = 512ull * 1024 * 1024;
		std::uint64_t stagingBytes = 32ull * 1024 * 1024;
		std::uint64_t uploadBytesPerFrame = 4ull * 1024 * 1024;
		std::uint32_t maxPendingRequests = 128;
		std::uint32_t activationEntitiesPerFrame = 64;
	};

	struct Region
	{
		std::string id;
		std::string scene;
		glm::vec3 boundsMin{0.0f};
		glm::vec3 boundsMax{0.0f};
		float unloadMargin = 16.0f;
		io::IOPriority priority = io::IOPriority::Normal;
		bool alwaysLoaded = false;
	};

	struct Source
	{
		std::string id;
		glm::vec3 position{0.0f};
		float loadRadius = 128.0f;
		float prefetchRadius = 256.0f;
		io::IOPriority priority = io::IOPriority::Normal;
	};
}

#include "scene/SceneSerializer.hpp"

#include <mutex>
#include <random>

namespace aether::app::scene
{
	std::uint64_t GenerateSceneNodeId()
	{
		static std::mutex mutex;
		static std::mt19937_64 rng{std::random_device{}()};
		const std::lock_guard lock(mutex);
		std::uint64_t id = 0;
		while (id == 0)
		{
			id = rng();
		}
		return id;
	}

	bool IsValidAssetName(std::string_view assetName)
	{
		if (assetName.empty())
		{
			return false;
		}
		constexpr std::string_view kForbidden = "<>:\"/\\|?*";
		for (const char c: assetName)
		{
			if (kForbidden.find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20)
			{
				return false;
			}
		}
		return assetName.find_first_not_of('.') != std::string_view::npos;
	}
} // namespace aether::app::scene

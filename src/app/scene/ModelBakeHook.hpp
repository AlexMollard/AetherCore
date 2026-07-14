#pragma once

#include <functional>
#include <string>

namespace aether::app::scene
{
	// the model has never been imported, that resolve fails and the entity loses
	struct ModelBakeHook
	{
		std::function<bool(const std::string& vfsModelPath, std::string& error)> ensureBaked;
	};
} // namespace aether::app::scene

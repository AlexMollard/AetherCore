#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aether
{
	class ServiceContainer;
}

namespace aether::editor
{
	// render graph, so they only ever run on the main thread (see
	struct MethodContext
	{
		ServiceContainer& services;
		std::uint64_t frameIndex = 0;
		double fps = 0.0;
	};

	using MethodHandler = std::function<nlohmann::json(const nlohmann::json& params, MethodContext& ctx)>;

	struct ControlMethod
	{
		std::string name;
		std::string tool;
		std::string description;
		bool mutates = false;
		nlohmann::json paramsSchema;
		MethodHandler handler;
	};

	[[nodiscard]] std::vector<ControlMethod> BuildControlMethods();

	// 2D authoring group (tile painting, atlas slicing, animation creation,
	// editor camera, asset discovery) - defined in ControlMethods2D.cpp.
	void Append2DAuthoringMethods(std::vector<ControlMethod>& methods);

	// Pixel-art canvas group (new/open/save, per-pixel + shape edits, read-back) -
	// defined in ControlMethodsPixel.cpp. Operates on the shared PixelArtDocument.
	void AppendPixelArtMethods(std::vector<ControlMethod>& methods);
} // namespace aether::editor

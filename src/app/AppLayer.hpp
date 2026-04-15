#pragma once

#include <cstdint>

namespace aether
{
	class AetherCore;
	class Scene;
	class World;
	class Input;
	class CameraManager;
	class Renderer;
	class AssetManager;
	class UIRenderer;
} // namespace aether

namespace aether::app
{
	struct LayerContext
	{
		aether::AetherCore& engine;
		double deltaTimeSeconds = 0.0;
		std::uint64_t frameIndex = 0;
		aether::Scene* scene = nullptr;
		aether::World* world = nullptr;
		aether::Input* input = nullptr;
		aether::CameraManager* cameras = nullptr;
		aether::Renderer* renderer = nullptr;
		aether::AssetManager* assets = nullptr;
		aether::UIRenderer* ui = nullptr;
	};

	class AppLayer
	{
	public:
		virtual ~AppLayer() = default;

		virtual void OnAttach(LayerContext& context);
		virtual void OnDetach(LayerContext& context);
		virtual void OnUpdate(LayerContext& context);
		virtual void OnGui(LayerContext& context);
	};
} // namespace aether::app

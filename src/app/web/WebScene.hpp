#pragma once

#include <array>
#include <functional>

#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "animation/AnimationSystem.hpp"
#include "physics/PhysicsSystem.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/TileAssetStore.hpp"
#include "material/TextureRegistry.hpp"
#include "rendering/SpriteSystem.hpp"
#include "rendering/TileMapSystem.hpp"
#include "webgpu/WebGpuDevice.hpp"
#include "web/WebModels.hpp"

namespace aether::app::web
{
	class WebTextureSink final : public ITextureSlotSink
	{
	public:

		explicit WebTextureSink(webgpu::WebGpuDevice& graphics)
		      : m_graphics(graphics)
		{
		}

		std::string ResolvePath(std::string_view path) const override;
		Expected<TextureResource> Load(std::string_view path, TextureColorSpace colorSpace) override;

		std::uint32_t Capacity() const override
		{
			return 255;
		}

		std::string error;
		bool colorManaged = false;

	private:
		webgpu::WebGpuDevice& m_graphics;
		std::uint32_t m_next_slot = 2;
	};

	struct WebScriptRecord
	{
		Entity entity;
		scene::ScriptRecord record;
	};

	struct WebInstantiated
	{
		Entity root;
		std::vector<WebScriptRecord> scripts;
	};

	// Runs the gameplay (script) tick at ScriptComponentSystem's native position in the world
	// system order: after Animation/Physics/Physics2D, before Particles.
	class WebScriptSystem final : public System
	{
	public:
		[[nodiscard]] const char* GetName() const override
		{
			return "WebScriptSystem";
		}

		void Update(World&, float dt) override
		{
			if (tick)
			{
				tick(dt);
			}
		}

		std::function<void(float)> tick;
	};

	class WebScene
	{
	public:
		static std::string ProjectPath(std::string_view kind, std::string_view name, std::string_view suffix);
		WebScene(World& world, webgpu::WebGpuDevice& graphics);
		~WebScene();
		Expected<std::vector<WebScriptRecord>> Load(std::string_view name);
		// Same instances the registered systems are; valid for the lifetime of the scene.
		[[nodiscard]] PhysicsSystem& Physics()
		{
			return *m_physics;
		}

		[[nodiscard]] AnimationSystem& Animation()
		{
			return *m_animation;
		}

		[[nodiscard]] const ::aether::scripting::gameplay3d::ClipCatalog* Clips() const
		{
			return m_models.Clips();
		}

		void SetScriptTick(std::function<void(float)> tick)
		{
			m_script_system->tick = std::move(tick);
		}
		Expected<WebInstantiated> Instantiate(std::string_view name, glm::vec3 position);
		void Tick(float dt);
		Expected<void> ValidateEffects();
		std::vector<Entity> TakeRetiredParticles();
		Expected<void> Extract(RenderFramePacket& packet, unsigned width, unsigned height, float elapsed);
		World& world;

	private:
		Expected<std::vector<Entity>> Apply(const scene::SceneDescription& description);
		Expected<std::vector<WebScriptRecord>> ResolveScripts(const scene::SceneDescription& description, const std::vector<Entity>& created);
		Expected<void> ApplyPhysics3D(const scene::SceneDescription& description, const std::vector<Entity>& created);
		PhysicsSystem* m_physics = nullptr;
		AnimationSystem* m_animation = nullptr;
		WebScriptSystem* m_script_system = nullptr;
		WebTextureSink m_sink;
		TextureRegistry m_textures;
		TextureRegistry m_model_textures;
		WebModels m_models;
		SpriteAssetStore m_sprites;
		TileAssetStore m_tiles;
		SpriteSystem m_sprite_system;
		TileMapSystem m_tile_system;
		webgpu::WebGpuDevice& m_graphics;
		std::optional<scene::EnvironmentRecord> m_environment;
		bool m_default_lighting = false;
		float m_light_time = 0.0f;
		Expected<void> ExtractLighting(RenderFramePacket& packet);
		std::array<float, 256> m_advances{};
		bool m_font_ready = false;
		std::unordered_map<std::string, TextureHandle> m_ui_textures;
		std::vector<Entity> m_retired_particles;
		std::uint64_t m_scene_generation = 0;
		Expected<void> ExtractUi(RenderFramePacket& packet, unsigned width, unsigned height);
	};
} // namespace aether::app::web

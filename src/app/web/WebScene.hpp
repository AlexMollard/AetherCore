#pragma once

#include <array>
#include <functional>
#include <unordered_map>

#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "animation/AnimationSystem.hpp"
#include "physics/PhysicsSystem.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/TileAssetStore.hpp"
#include "material/TextureRegistry.hpp"
#include "ui/FontRegistry.hpp"
#include "rendering/SpriteSystem.hpp"
#include "rendering/TileMapSystem.hpp"
#include "webgpu/WebGpuDevice.hpp"
#include "web/WebModels.hpp"
#include "scene/SceneStreamingReferences.hpp"

namespace aether::app::web
{
	class WebTextureSink final : public ITextureSlotSink
	{
	public:

		explicit WebTextureSink(webgpu::WebGpuDevice& graphics)
		      : m_graphics(graphics), m_retirement(std::make_shared<RetirementState>(&graphics))
		{
		}
		~WebTextureSink() override { m_retirement->graphics = nullptr; }

		std::string ResolvePath(std::string_view path) const override;
		Expected<TextureResource> Load(std::string_view path, TextureColorSpace colorSpace) override;
		void SetStreamingBudget(std::shared_ptr<streaming::BudgetLedger> budget) { m_budget = std::move(budget); }
		void Retire(std::vector<std::uint32_t> slots);

		std::uint32_t Capacity() const override
		{
			return 256;
		}

		std::string error;
		bool colorManaged = false;

	private:
		struct RetirementState
		{
			explicit RetirementState(webgpu::WebGpuDevice* value) : graphics(value) {}
			webgpu::WebGpuDevice* graphics;
		};
		webgpu::WebGpuDevice& m_graphics;
		std::uint32_t m_next_slot = 2;
		std::vector<std::uint32_t> m_free_slots;
		std::shared_ptr<streaming::BudgetLedger> m_budget;
		std::shared_ptr<RetirementState> m_retirement;
	};

	struct WebScriptRecord
	{
		Entity entity;
		scene::ScriptRecord record;
		std::uint32_t scriptIndex;
	};

	struct WebInstantiated
	{
		Entity root;
		std::vector<Entity> entities;
		std::vector<WebScriptRecord> scripts;
	};


	struct WebRegion
	{
		std::vector<Entity> entities;
		std::vector<WebScriptRecord> scripts;
		std::string id;
		Entity activationGate{};
		std::vector<Entity> promoted;
	};

	struct RegionSnapshotStorage
	{
		std::uint64_t recordBytes = 0;
		std::uint64_t stringBytes = 0;
		std::uint64_t logicalNodes = 0;
		std::uint64_t entityCount = 0;
	};

	struct WebRegionActivation
	{
		scene::SceneDescription description;
		WebRegion region;
		std::size_t cursor = 0;
		unsigned phase = 0;
		std::span<const std::byte> prepared;
		std::size_t preparedOffset = 0;
		std::uint32_t preparedEntities = 0;
		std::shared_ptr<streaming::BudgetLedger> budget;
		std::uint64_t documentCharge = 0;
		std::uint64_t restoreGateNode = 0;
		bool restoredGateSeen = false;
		std::unordered_map<std::uint64_t, Entity> nodes;
		std::shared_ptr<scene::StreamingNodeIndex> fieldNodes;
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
		void SetStreamingBudget(std::shared_ptr<streaming::BudgetLedger> budget) { m_streaming_budget = budget; m_sink.SetStreamingBudget(budget); m_models.SetStreamingBudget(std::move(budget)); }
		Expected<std::vector<WebScriptRecord>> Load(std::string_view name);
		Expected<WebRegionActivation> BeginRegion(std::string_view sceneReference);
		Expected<WebRegionActivation> BeginPreparedRegion(std::span<const std::byte> prepared, std::uint64_t restoreGateNode);
		Expected<bool> StepRegion(WebRegionActivation& activation, std::uint32_t entityBudget);
		void CancelRegion(WebRegionActivation& activation);
		void DetachRegion(WebRegion& region);
		Expected<scene::SceneDescription> CaptureRegionDocument(const WebRegion& region);
		Expected<RegionSnapshotStorage> MeasureRegionSnapshotStorage(const WebRegion& region) const;
		Expected<std::string> CapturePersistentAssets(Entity root);
		Expected<std::uint64_t> MeasurePersistentAssets(Entity root) const;
		[[nodiscard]] const std::vector<streaming::Region>& StreamingRegions() const { return m_regions; }
		// Same instances the registered systems are; valid for the lifetime of the scene.
		[[nodiscard]] PhysicsSystem& Physics()
		{
			return *m_physics;
		}

		[[nodiscard]] AnimationSystem& Animation()
		{
			return *m_animation;
		}
		[[nodiscard]] ui::FontRegistry& Fonts()
		{
			return m_fonts;
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
		[[nodiscard]] bool RenderReady() const { return m_models.RenderReady(); }
		[[nodiscard]] bool StreamingPending() const { return m_models.StreamingPending(); }
		[[nodiscard]] bool StreamingBudgetBlocked() const { return m_models.StreamingBudgetBlocked() || m_font_budget_blocked; }
		World& world;

	private:
		Expected<std::vector<Entity>> Apply(const scene::SceneDescription& description);
		Expected<std::vector<WebScriptRecord>> ResolveScripts(const scene::SceneDescription& description, const std::vector<Entity>& created, std::size_t begin = 0, std::size_t end = SIZE_MAX);
		Expected<void> ApplyPhysics3D(const scene::SceneDescription& description, const std::vector<Entity>& created);
		PhysicsSystem* m_physics = nullptr;
		AnimationSystem* m_animation = nullptr;
		WebScriptSystem* m_script_system = nullptr;
		WebTextureSink m_sink;
		TextureRegistry m_textures;
		TextureRegistry m_model_textures;
		ui::FontRegistry m_fonts;
		std::unordered_map<std::string, bool> m_streaming_fonts;
		bool m_font_budget_blocked = false;
		WebModels m_models;
		SpriteAssetStore m_sprites;
		TileAssetStore m_tiles;
		SpriteSystem m_sprite_system;
		TileMapSystem m_tile_system;
		webgpu::WebGpuDevice& m_graphics;
		std::shared_ptr<streaming::BudgetLedger> m_streaming_budget;
		std::optional<scene::EnvironmentRecord> m_environment;
		std::vector<streaming::Region> m_regions;
		bool m_default_lighting = false;
		float m_light_time = 0.0f;
		Expected<void> ExtractLighting(RenderFramePacket& packet);
		std::vector<Entity> m_retired_particles;
		std::uint64_t m_scene_generation = 0;
		Expected<void> ExtractUi(RenderFramePacket& packet, unsigned width, unsigned height);
	};
} // namespace aether::app::web

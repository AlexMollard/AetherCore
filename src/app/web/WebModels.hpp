#pragma once

#include <map>
#include "scripting/Gameplay3D.hpp"
#include "assets/GltfAsset.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/SceneSerializer.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "mesh/GrassMeshData.hpp"
#include "webgpu/WebGpuDevice.hpp"

namespace aether::app::web
{
	class WebTextureSink;

	// Scene meshes share immutable GPU slots until scene replacement retires them.
	class WebModels
	{
	public:
		WebModels(webgpu::WebGpuDevice& graphics, TextureRegistry& textures, WebTextureSink& sink);
		// Clip names/counts of the cooked glTF animations, for the shared 3D animation ops.
		[[nodiscard]] const ::aether::scripting::gameplay3d::ClipCatalog* Clips() const
		{
			return &m_catalog;
		}
		Expected<void> Apply(World& world, Entity entity, const scene::EntityRecord& record);
		Expected<void> Extract(World& world, RenderFramePacket& packet);
		[[nodiscard]] bool RenderReady() const { return m_initial_ready; }
		[[nodiscard]] bool StreamingPending() const { return m_streaming_pending || !m_pending_uploads.empty(); }
		// AnimationSystem's duration source: the playing and fading clip lengths of a skinned
		// entity, defined exactly like AnimationDatabase (longest channel key time).
		[[nodiscard]] bool ClipDurations(World& world, Entity entity, const SkinnedMeshComponent& skin, float& clip, float& fade) const;
		void ClearEntities();

	private:
		struct Model
		{
			assets::GltfAsset data;
			std::vector<std::uint32_t> slots;
			std::vector<float> clipDurations;
		};

		struct Binding
		{
			Entity entity;
			std::uint32_t slot;
			Model* model = nullptr;
			std::int32_t skin = -1;
			glm::vec4 localSphere{0.0f};
			bool grass = false;
			glm::vec3 origin{0.0f};
			glm::vec2 grassFade{0.0f};
		};

		struct GrassSlot
		{
			std::uint32_t slot;
			glm::vec3 origin;
			std::size_t bytes = 0;
		};

		struct GrassResident
		{
			MeshGen::GrassField field;
			std::map<MeshGen::GrassChunkKey, GrassSlot> chunks;
			std::vector<MeshGen::GrassChunkKey> desired;
		};

		struct PendingUpload
		{
			std::string source;
			std::uint32_t slot;
			std::size_t primitive;
			bool model;
		};

		Expected<std::uint32_t> ResolveShader(std::string_view uri);
		Expected<MaterialAsset> ResolveMaterial(const scene::MaterialRecord& record);
		Expected<MaterialAsset> ResolveTextures(MaterialAsset asset, const std::array<std::string, 5>& paths);
		void RetireGrass(std::vector<std::uint32_t> slots);
		std::uint32_t TakeSlot();
		Expected<void> FlushUploads();
		webgpu::WebGpuDevice& m_graphics;
		TextureRegistry& m_textures;
		WebTextureSink& m_sink;
		std::map<std::string, Model> m_models;
		std::map<std::string, std::uint32_t> m_shaders;
		std::map<std::string, std::uint32_t> m_primitives;
		std::map<std::uint64_t, GrassResident> m_grass;
		std::map<std::pair<std::string, TextureColorSpace>, TextureHandle> m_texture_cache;
		std::vector<Binding> m_bindings;
		std::uint32_t m_next_slot = 0;
		std::uint32_t m_next_virtual_slot = 1024;
		std::vector<std::uint32_t> m_free_grass_slots;
		std::vector<PendingUpload> m_pending_uploads;
		std::vector<MeshGen::GrassChunkKey> m_selected_grass;
		bool m_initial_ready = false;
		bool m_streaming_pending = false;

		class Catalog final : public ::aether::scripting::gameplay3d::ClipCatalog
		{
		public:
			explicit Catalog(const WebModels& owner)
			      : m_owner(owner)
			{
			}

			[[nodiscard]] std::uint32_t ClipCount(Entity animator) const override;
			[[nodiscard]] std::string_view ClipName(Entity animator, std::uint32_t index) const override;
			[[nodiscard]] float ClipDuration(Entity animator, std::uint32_t index) const override;

		private:
			const WebModels& m_owner;
		};
		const Model* FindModel(Entity entity) const;
		Catalog m_catalog{*this};
	};
} // namespace aether::app::web

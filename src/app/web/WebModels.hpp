#pragma once

#include <map>
#include "scripting/Gameplay3D.hpp"
#include "assets/GltfAsset.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/SceneSerializer.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "webgpu/WebGpuDevice.hpp"

namespace aether::app::web
{
	class WebTextureSink;

	// Assets survive scene switches; immutable GPU slots are uploaded once per source.
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
		};

		Expected<std::uint32_t> ResolveShader(std::string_view uri);
		Expected<MaterialAsset> ResolveMaterial(const scene::MaterialRecord& record);
		Expected<MaterialAsset> ResolveTextures(MaterialAsset asset, const std::array<std::string, 5>& paths);
		webgpu::WebGpuDevice& m_graphics;
		TextureRegistry& m_textures;
		WebTextureSink& m_sink;
		std::map<std::string, Model> m_models;
		std::map<std::string, std::uint32_t> m_shaders;
		std::map<std::string, std::uint32_t> m_primitives;
		std::map<std::pair<std::string, TextureColorSpace>, TextureHandle> m_texture_cache;
		std::vector<Binding> m_bindings;
		std::uint32_t m_next_slot = 0;

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

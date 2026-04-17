#include "AssetManager.hpp"

#include "assets/GltfAsset.hpp"
#include "BindlessManager.hpp"
#include "EcsHelpers.hpp"
#include "Logger.hpp"
#include "MaterialBuffer.hpp"
#include "RenderQueue.hpp"
#include "RenderTargetService.hpp"
#include "ShadowService.hpp"
#include "VulkanContext.hpp"
#include "World.hpp"

namespace aether
{
	void AssetManager::Initialize(VulkanContext& context, BindlessManager& bindlessManager, MaterialBuffer& materialBuffer, RenderQueue& renderQueue, ShadowService& shadowService, RenderTargetService& renderTargetService, World& world, const VkCommandPool uploadPool)
	{
		m_context = &context;
		m_bindlessManager = &bindlessManager;
		m_materialBuffer = &materialBuffer;
		m_renderQueue = &renderQueue;
		m_shadowService = &shadowService;
		m_renderTargetService = &renderTargetService;
		m_world = &world;
		m_uploadPool = uploadPool;
	}

	Mesh AssetManager::CreateMesh(std::span<const Mesh::Vertex> vertices)
	{
		return Mesh::Create(m_context->GetDevice().device, m_context->GetAllocator(), m_context->GetGraphicsQueue(), m_uploadPool, vertices);
	}

	Mesh AssetManager::CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices)
	{
		return Mesh::Create(m_context->GetDevice().device, m_context->GetAllocator(), m_context->GetGraphicsQueue(), m_uploadPool, vertices, indices);
	}

	Texture AssetManager::CreateTexture(std::string_view path)
	{
		return Texture::LoadFromFile(path, m_context->GetDevice().device, m_context->GetAllocator(), m_context->GetGraphicsQueue(), m_uploadPool, *m_bindlessManager);
	}

	GraphicsPipeline AssetManager::CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc)
	{
		return GraphicsPipeline::Create(m_context->GetDevice().device, desc);
	}

	void AssetManager::RegisterMaterial(Material& mat)
	{
		if (mat.materialSlot != Material::kNoTexture)
		{
			GpuMaterial gpu{};
			gpu.baseColorFactor = mat.baseColorFactor;
			gpu.metallicFactor = mat.metallicFactor;
			gpu.roughnessFactor = mat.roughnessFactor;
			gpu.occlusionStrength = mat.occlusionStrength;
			gpu.alphaCutoff = mat.alphaCutoff;
			gpu.emissiveFactor = glm::vec4(mat.emissiveFactor, 0.0f);
			gpu.flags = (mat.doubleSided ? GpuMaterial::kDoubleSided : 0u) | (mat.alphaBlend ? GpuMaterial::kAlphaBlend : 0u) | (mat.alphaMask ? GpuMaterial::kAlphaMask : 0u);
			gpu.albedoSlot = mat.albedoSlot;
			gpu.normalSlot = mat.normalSlot;
			gpu.metallicRoughnessSlot = mat.metallicRoughnessSlot;
			gpu.occlusionSlot = mat.occlusionSlot;
			gpu.emissiveSlot = mat.emissiveSlot;
			m_materialBuffer->Write(mat.materialSlot, gpu);
			return;
		}

		const std::uint32_t slot = m_materialBuffer->AllocateSlot();
		if (slot == MaterialBuffer::kInvalidSlot)
		{
			WARN(LogCategory::Engine, "RegisterMaterial: MaterialBuffer is full - material will render as default.");
			return;
		}

		GpuMaterial gpu{};
		gpu.baseColorFactor = mat.baseColorFactor;
		gpu.metallicFactor = mat.metallicFactor;
		gpu.roughnessFactor = mat.roughnessFactor;
		gpu.occlusionStrength = mat.occlusionStrength;
		gpu.alphaCutoff = mat.alphaCutoff;
		gpu.emissiveFactor = glm::vec4(mat.emissiveFactor, 0.0f);
		gpu.flags = (mat.doubleSided ? GpuMaterial::kDoubleSided : 0u) | (mat.alphaBlend ? GpuMaterial::kAlphaBlend : 0u) | (mat.alphaMask ? GpuMaterial::kAlphaMask : 0u);
		gpu.albedoSlot = mat.albedoSlot;
		gpu.normalSlot = mat.normalSlot;
		gpu.metallicRoughnessSlot = mat.metallicRoughnessSlot;
		gpu.occlusionSlot = mat.occlusionSlot;
		gpu.emissiveSlot = mat.emissiveSlot;

		m_materialBuffer->Write(slot, gpu);
		mat.materialSlot = slot;
	}

	void AssetManager::UnregisterMaterial(Material& mat)
	{
		if (mat.materialSlot == Material::kNoTexture)
		{
			return;
		}
		m_materialBuffer->FreeSlot(mat.materialSlot);
		mat.materialSlot = Material::kNoTexture;
	}

	LoadedModel AssetManager::LoadModel(std::string_view path)
	{
		const assets::GltfAsset source = assets::GltfAsset::LoadFromVfsPath(path);
		LoadedModel loaded;

		std::vector<std::uint32_t> imageSlots(source.images.size(), Material::kNoTexture);
		loaded.textures.reserve(source.images.size());
		for (std::size_t imageIndex = 0; imageIndex < source.images.size(); ++imageIndex)
		{
			const assets::GltfImage& image = source.images[imageIndex];
			if (image.uri.empty() || std::string_view(image.uri).starts_with("data:"))
			{
				continue;
			}

			Texture texture = CreateTexture(image.uri);
			imageSlots[imageIndex] = texture.GetBindlessSlot();
			loaded.textures.push_back(std::move(texture));
		}

		std::vector<glm::mat4> localNodeTransforms(source.nodes.size(), glm::mat4(1.0f));
		for (std::size_t nodeIndex = 0; nodeIndex < source.nodes.size(); ++nodeIndex)
		{
			const assets::GltfNode& node = source.nodes[nodeIndex];
			if (node.hasMatrix)
			{
				localNodeTransforms[nodeIndex] = node.matrix;
				continue;
			}

			const glm::mat4 t = glm::translate(glm::mat4(1.0f), node.translation);
			const glm::mat4 r = glm::mat4_cast(node.rotation);
			const glm::mat4 s = glm::scale(glm::mat4(1.0f), node.scale);
			localNodeTransforms[nodeIndex] = t * r * s;
		}

		std::vector<glm::mat4> worldNodeTransforms(source.nodes.size(), glm::mat4(1.0f));
		for (std::size_t nodeIndex = 0; nodeIndex < source.nodes.size(); ++nodeIndex)
		{
			glm::mat4 transform = localNodeTransforms[nodeIndex];
			std::int32_t parent = source.nodes[nodeIndex].parentIndex;
			while (parent >= 0)
			{
				transform = localNodeTransforms[static_cast<std::size_t>(parent)] * transform;
				parent = source.nodes[static_cast<std::size_t>(parent)].parentIndex;
			}
			worldNodeTransforms[nodeIndex] = transform;
		}

		loaded.primitives.reserve(source.primitives.size());
		for (const assets::GltfPrimitive& primitive: source.primitives)
		{
			if (primitive.vertices.empty())
			{
				continue;
			}

			LoadedModelPrimitive loadedPrim;
			loadedPrim.mesh = CreateMesh(primitive.vertices, primitive.indices);
			loadedPrim.skinIndex = primitive.skinIndex;

			if (primitive.skinIndex < 0 && primitive.nodeIndex < worldNodeTransforms.size())
			{
				loadedPrim.localTransform = worldNodeTransforms[primitive.nodeIndex];
			}

			if (primitive.materialIndex >= 0 && static_cast<std::size_t>(primitive.materialIndex) < source.materials.size())
			{
				const assets::GltfMaterial& srcMat = source.materials[static_cast<std::size_t>(primitive.materialIndex)];
				Material& mat = loadedPrim.material;

				mat.baseColorFactor = srcMat.baseColorFactor;
				mat.metallicFactor = srcMat.metallicFactor;
				mat.roughnessFactor = srcMat.roughnessFactor;
				mat.emissiveFactor = srcMat.emissiveFactor;
				mat.alphaCutoff = srcMat.alphaCutoff;
				mat.doubleSided = srcMat.doubleSided;
				mat.alphaBlend = srcMat.alphaBlend;
				mat.alphaMask = srcMat.alphaMask;

				auto resolveSlot = [&](const std::int32_t texIdx) -> std::uint32_t
				{
					if (texIdx < 0 || static_cast<std::size_t>(texIdx) >= source.textures.size())
					{
						return Material::kNoTexture;
					}
					const assets::GltfTexture& tex = source.textures[static_cast<std::size_t>(texIdx)];
					if (tex.imageIndex < 0 || static_cast<std::size_t>(tex.imageIndex) >= imageSlots.size())
					{
						return Material::kNoTexture;
					}
					return imageSlots[static_cast<std::size_t>(tex.imageIndex)];
				};

				mat.albedoSlot = resolveSlot(srcMat.baseColorTexture);
				mat.normalSlot = resolveSlot(srcMat.normalTexture);
				mat.metallicRoughnessSlot = resolveSlot(srcMat.metallicRoughnessTexture);
				mat.occlusionSlot = resolveSlot(srcMat.occlusionTexture);
				mat.emissiveSlot = resolveSlot(srcMat.emissiveTexture);

				RegisterMaterial(mat);
			}

			loaded.primitives.push_back(std::move(loadedPrim));
		}

		INFO(LogCategory::Engine, "Loaded glTF '{}': {} primitive(s), {} texture(s), {} animation(s).", std::string(path), loaded.primitives.size(), loaded.textures.size(), source.animations.size());

		if (!source.skins.empty())
		{
			loaded.animator = ModelAnimator::Create(m_context->GetDevice().device, m_context->GetAllocator(), source);
			if (!source.animations.empty())
			{
				loaded.animationDb = AnimationDatabase::Create(m_context->GetDevice().device, m_context->GetAllocator(), source);
			}
		}

		return loaded;
	}

	std::vector<Entity> AssetManager::SpawnModel(LoadedModel& model, GraphicsPipeline& pipeline, float scale)
	{
		std::vector<Entity> entities;
		entities.reserve(model.primitives.size());

		if (model.animationDb.IsValid())
		{
			m_renderQueue->SetAnimationDatabase(&model.animationDb);
			m_shadowService->SetAnimationDatabase(&model.animationDb);
			m_renderTargetService->SetAnimationDatabase(&model.animationDb);
		}

		const glm::mat4 scaleMat = glm::scale(glm::mat4(1.0f), glm::vec3(scale));

		for (const LoadedModelPrimitive& primitive: model.primitives)
		{
			const Entity entity = aether::ecs::SpawnMesh(*m_world, pipeline, primitive.mesh, primitive.material, scaleMat * primitive.localTransform);

			if (model.animator && primitive.skinIndex >= 0)
			{
				const VkDeviceAddress addr = model.animator->GetSkinBufferAddr(primitive.skinIndex);
				const std::uint32_t joints = model.animator->GetSkinJointCount(primitive.skinIndex);
				if (addr != 0 && joints > 0)
				{
					m_world->EmplaceOrReplace<SkinComponent>(entity,
					        SkinComponent{
					                .sourceSkinBufferAddr = addr,
					                .skinIndex = primitive.skinIndex,
					                .jointCount = joints,
					        });
				}
			}

			entities.push_back(entity);
		}

		return entities;
	}
} // namespace aether

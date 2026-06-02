#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
#include "vulkan/volk.hpp"

#include "vulkan/UniqueBuffer.hpp"
#include "vulkan/UniqueImage.hpp"

namespace aether
{
	class BindlessManager;

	enum class ResourceQueueClass : std::uint8_t
	{
		Graphics,
		Compute,
		Transfer,
		Present,
		External,
	};

	enum class ResourceVisibility : std::uint8_t
	{
		LocalOnly,
		BindlessSampled,
		BindlessStorage,
	};

	struct UsageScope
	{
		VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
		VkAccessFlags2 access = VK_ACCESS_2_NONE;
		bool writes = false;
	};

	struct AliasConstraints
	{
		bool allowAutomaticAliasing = true;
		bool allowExplicitAliasing = true;
		bool allowCrossQueueAliasing = false;
	};

	struct LifetimeWindow
	{
		std::uint32_t firstPass = 0;
		std::uint32_t lastPass = 0;
		UsageScope firstUse{};
		UsageScope lastUse{};
		ResourceQueueClass queue = ResourceQueueClass::Graphics;

		[[nodiscard]] bool Overlaps(const LifetimeWindow& other) const;
		[[nodiscard]] bool IsValid() const;
	};

	struct ResourceContract
	{
		LifetimeWindow lifetime{};
		AliasConstraints alias{};
		ResourceVisibility visibility = ResourceVisibility::LocalOnly;
		bool transient = true;
	};

	struct BufferResourceDesc
	{
		VkDeviceSize size = 0;
		VkBufferUsageFlags usage = 0;
	};

	struct ImageResourceDesc
	{
		VkExtent3D extent{};
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags usage = 0;
		std::uint32_t mipLevels = 1;
		std::uint32_t arrayLayers = 1;
	};

	struct VirtualBufferHandle
	{
		std::uint32_t id = 0;
		std::uint32_t generation = 0;

		[[nodiscard]] bool IsValid() const;
	};

	struct VirtualImageHandle
	{
		std::uint32_t id = 0;
		std::uint32_t generation = 0;

		[[nodiscard]] bool IsValid() const;
	};

	// A render-graph-oriented resource pool that supports virtual resources,
	// automatic and explicit aliasing, and bindless visibility bindings.
	//
	// Thread safety: NOT thread-safe. All create/materialize calls must occur
	// on a single thread (the render thread in AetherCore). Use external
	// synchronization if concurrent access is required.
	class ResourcePool
	{
	public:
		void Shutdown();

		struct BindlessImageConfig
		{
			BindlessManager* manager = nullptr;
			VkDevice device = VK_NULL_HANDLE;
			VkImageAspectFlags sampledAspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			VkImageLayout sampledLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		};

		using BufferFactory = std::function<UniqueBuffer(const BufferResourceDesc&)>;
		using ImageFactory = std::function<UniqueImage(const ImageResourceDesc&)>;

		[[nodiscard]] VirtualBufferHandle CreateVirtualBuffer(const BufferResourceDesc& desc, const ResourceContract& contract);

		[[nodiscard]] VirtualImageHandle CreateVirtualImage(const ImageResourceDesc& desc, const ResourceContract& contract);

		[[nodiscard]] VirtualBufferHandle CreateVirtualBuffer(const BufferResourceDesc& desc, const LifetimeWindow& lifetime, bool transient = true);

		[[nodiscard]] VirtualImageHandle CreateVirtualImage(const ImageResourceDesc& desc, const LifetimeWindow& lifetime, bool transient = true);

		void ConfigureBindlessImages(const BindlessImageConfig& config);

		void SetLifetime(VirtualBufferHandle handle, const LifetimeWindow& lifetime);
		void SetLifetime(VirtualImageHandle handle, const LifetimeWindow& lifetime);

		bool AliasBuffer(VirtualBufferHandle aliasHandle, VirtualBufferHandle sourceHandle);
		bool AliasImage(VirtualImageHandle aliasHandle, VirtualImageHandle sourceHandle);

		UniqueBuffer& MaterializeBuffer(VirtualBufferHandle handle, const BufferFactory& factory);
		UniqueImage& MaterializeImage(VirtualImageHandle handle, const ImageFactory& factory);

		[[nodiscard]] const UniqueBuffer* TryGetBuffer(VirtualBufferHandle handle) const;
		[[nodiscard]] const UniqueImage* TryGetImage(VirtualImageHandle handle) const;

	private:
		struct BufferVirtualRecord
		{
			BufferResourceDesc desc{};
			ResourceContract contract{};
			std::uint32_t generation = 1;
			std::optional<std::uint32_t> physicalId;
			std::optional<std::uint32_t> aliasSourceId;
		};

		struct ImageVirtualRecord
		{
			ImageResourceDesc desc{};
			ResourceContract contract{};
			std::uint32_t generation = 1;
			std::optional<std::uint32_t> physicalId;
			std::optional<std::uint32_t> aliasSourceId;
		};

		struct BufferPhysicalRecord
		{
			UniqueBuffer resource;
			std::vector<std::uint32_t> owners;
			ResourceVisibility visibility = ResourceVisibility::LocalOnly;
			ResourceQueueClass queue = ResourceQueueClass::Graphics;
		};

		struct ImagePhysicalRecord
		{
			UniqueImage resource;
			std::vector<std::uint32_t> owners;
			ResourceVisibility visibility = ResourceVisibility::LocalOnly;
			ResourceQueueClass queue = ResourceQueueClass::Graphics;
		};

		BufferVirtualRecord& RequireBufferRecord(VirtualBufferHandle handle);
		const BufferVirtualRecord& RequireBufferRecord(VirtualBufferHandle handle) const;
		ImageVirtualRecord& RequireImageRecord(VirtualImageHandle handle);
		const ImageVirtualRecord& RequireImageRecord(VirtualImageHandle handle) const;

		[[nodiscard]] bool CanAliasWithOwners(const ResourceContract& candidateContract, const std::vector<std::uint32_t>& ownerIds, const std::vector<BufferVirtualRecord>& records) const;

		[[nodiscard]] bool CanAliasWithOwners(const ResourceContract& candidateContract, const std::vector<std::uint32_t>& ownerIds, const std::vector<ImageVirtualRecord>& records) const;

		static bool IsBufferCompatible(const BufferResourceDesc& requested, const UniqueBuffer& existing);
		static bool IsImageCompatible(const ImageResourceDesc& requested, const UniqueImage& existing);

		void EnsureImageVisibilityBindings(UniqueImage& image, const ResourceContract& contract);

		BindlessImageConfig m_bindlessImageConfig{};
		std::vector<BufferVirtualRecord> m_virtualBuffers;
		std::vector<ImageVirtualRecord> m_virtualImages;
		std::vector<BufferPhysicalRecord> m_physicalBuffers;
		std::vector<ImagePhysicalRecord> m_physicalImages;
	};
} // namespace aether

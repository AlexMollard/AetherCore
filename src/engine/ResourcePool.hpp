#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <vulkan/vulkan.h>

#include "UniqueBuffer.hpp"
#include "UniqueImage.hpp"

namespace meow
{
	struct LifetimeWindow
	{
		std::uint32_t firstPass = 0;
		std::uint32_t lastPass = 0;

		[[nodiscard]] bool Overlaps(const LifetimeWindow& other) const;
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

	class ResourcePool
	{
	public:
		using BufferFactory = std::function<UniqueBuffer(const BufferResourceDesc&)>;
		using ImageFactory = std::function<UniqueImage(const ImageResourceDesc&)>;

		[[nodiscard]] VirtualBufferHandle CreateVirtualBuffer(
			const BufferResourceDesc& desc,
			const LifetimeWindow& lifetime,
			bool transient = true);

		[[nodiscard]] VirtualImageHandle CreateVirtualImage(
			const ImageResourceDesc& desc,
			const LifetimeWindow& lifetime,
			bool transient = true);

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
			LifetimeWindow lifetime{};
			bool transient = true;
			std::uint32_t generation = 1;
			std::optional<std::uint32_t> physicalId;
			std::optional<std::uint32_t> aliasSourceId;
		};

		struct ImageVirtualRecord
		{
			ImageResourceDesc desc{};
			LifetimeWindow lifetime{};
			bool transient = true;
			std::uint32_t generation = 1;
			std::optional<std::uint32_t> physicalId;
			std::optional<std::uint32_t> aliasSourceId;
		};

		struct BufferPhysicalRecord
		{
			UniqueBuffer resource;
			std::vector<std::uint32_t> owners;
		};

		struct ImagePhysicalRecord
		{
			UniqueImage resource;
			std::vector<std::uint32_t> owners;
		};

		BufferVirtualRecord& RequireBufferRecord(VirtualBufferHandle handle);
		const BufferVirtualRecord& RequireBufferRecord(VirtualBufferHandle handle) const;
		ImageVirtualRecord& RequireImageRecord(VirtualImageHandle handle);
		const ImageVirtualRecord& RequireImageRecord(VirtualImageHandle handle) const;

		[[nodiscard]] bool CanAliasWithOwners(
			const LifetimeWindow& candidateLifetime,
			const std::vector<std::uint32_t>& ownerIds,
			const std::vector<BufferVirtualRecord>& records) const;

		[[nodiscard]] bool CanAliasWithOwners(
			const LifetimeWindow& candidateLifetime,
			const std::vector<std::uint32_t>& ownerIds,
			const std::vector<ImageVirtualRecord>& records) const;

		static bool IsBufferCompatible(const BufferResourceDesc& requested, const UniqueBuffer& existing);
		static bool IsImageCompatible(const ImageResourceDesc& requested, const UniqueImage& existing);

		std::vector<BufferVirtualRecord> m_virtualBuffers;
		std::vector<ImageVirtualRecord> m_virtualImages;
		std::vector<BufferPhysicalRecord> m_physicalBuffers;
		std::vector<ImagePhysicalRecord> m_physicalImages;
	};
}

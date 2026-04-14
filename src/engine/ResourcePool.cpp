#include "ResourcePool.hpp"

#include <algorithm>

#include "BindlessManager.hpp"
#include "MeowExceptions.hpp"

namespace aether
{
	namespace
	{
		bool ContainsOwner(const std::vector<std::uint32_t>& owners, const std::uint32_t ownerId)
		{
			return std::find(owners.begin(), owners.end(), ownerId) != owners.end();
		}

		bool CanCrossQueueAlias(const ResourceContract& a, const ResourceContract& b)
		{
			if (a.lifetime.queue == b.lifetime.queue)
			{
				return true;
			}

			return a.alias.allowCrossQueueAliasing && b.alias.allowCrossQueueAliasing;
		}
	}

	bool LifetimeWindow::Overlaps(const LifetimeWindow& other) const
	{
		return firstPass <= other.lastPass && other.firstPass <= lastPass;
	}

	bool LifetimeWindow::IsValid() const
	{
		return firstPass <= lastPass;
	}

	bool VirtualBufferHandle::IsValid() const
	{
		return id != 0 && generation != 0;
	}

	bool VirtualImageHandle::IsValid() const
	{
		return id != 0 && generation != 0;
	}

	VirtualBufferHandle ResourcePool::CreateVirtualBuffer(
		const BufferResourceDesc& desc,
		const ResourceContract& contract)
	{
		if (!contract.lifetime.IsValid())
		{
			throw VulkanError("Invalid buffer lifetime window. firstPass must be <= lastPass.");
		}

		m_virtualBuffers.push_back(BufferVirtualRecord{
			.desc = desc,
			.contract = contract,
			});

		return VirtualBufferHandle{
			.id = static_cast<std::uint32_t>(m_virtualBuffers.size()),
			.generation = m_virtualBuffers.back().generation,
		};
	}

	VirtualImageHandle ResourcePool::CreateVirtualImage(
		const ImageResourceDesc& desc,
		const ResourceContract& contract)
	{
		if (!contract.lifetime.IsValid())
		{
			throw VulkanError("Invalid image lifetime window. firstPass must be <= lastPass.");
		}

		m_virtualImages.push_back(ImageVirtualRecord{
			.desc = desc,
			.contract = contract,
			});

		return VirtualImageHandle{
			.id = static_cast<std::uint32_t>(m_virtualImages.size()),
			.generation = m_virtualImages.back().generation,
		};
	}

	VirtualBufferHandle ResourcePool::CreateVirtualBuffer(
		const BufferResourceDesc& desc,
		const LifetimeWindow& lifetime,
		const bool transient)
	{
		ResourceContract contract{};
		contract.lifetime = lifetime;
		contract.transient = transient;
		return CreateVirtualBuffer(desc, contract);
	}

	VirtualImageHandle ResourcePool::CreateVirtualImage(
		const ImageResourceDesc& desc,
		const LifetimeWindow& lifetime,
		const bool transient)
	{
		ResourceContract contract{};
		contract.lifetime = lifetime;
		contract.transient = transient;
		return CreateVirtualImage(desc, contract);
	}

	void ResourcePool::ConfigureBindlessImages(const BindlessImageConfig& config)
	{
		m_bindlessImageConfig = config;
	}

	void ResourcePool::SetLifetime(const VirtualBufferHandle handle, const LifetimeWindow& lifetime)
	{
		auto& record = RequireBufferRecord(handle);
		record.contract.lifetime = lifetime;
	}

	void ResourcePool::SetLifetime(const VirtualImageHandle handle, const LifetimeWindow& lifetime)
	{
		auto& record = RequireImageRecord(handle);
		record.contract.lifetime = lifetime;
	}

	bool ResourcePool::AliasBuffer(const VirtualBufferHandle aliasHandle, const VirtualBufferHandle sourceHandle)
	{
		auto& alias = RequireBufferRecord(aliasHandle);
		const auto& source = RequireBufferRecord(sourceHandle);

		if (!alias.contract.alias.allowExplicitAliasing || !source.contract.alias.allowExplicitAliasing)
		{
			return false;
		}

		if (!CanCrossQueueAlias(alias.contract, source.contract))
		{
			return false;
		}

		if (alias.contract.visibility != source.contract.visibility)
		{
			return false;
		}

		if (source.physicalId.has_value())
		{
			if (!IsBufferCompatible(alias.desc, m_physicalBuffers[*source.physicalId].resource))
			{
				return false;
			}
		}
		else
		{
			if (alias.desc.size > source.desc.size)
			{
				return false;
			}

			if ((alias.desc.usage & ~source.desc.usage) != 0)
			{
				return false;
			}
		}

		if (alias.contract.lifetime.Overlaps(source.contract.lifetime))
		{
			return false;
		}

		alias.aliasSourceId = sourceHandle.id - 1;
		alias.physicalId.reset();
		return true;
	}

	bool ResourcePool::AliasImage(const VirtualImageHandle aliasHandle, const VirtualImageHandle sourceHandle)
	{
		auto& alias = RequireImageRecord(aliasHandle);
		const auto& source = RequireImageRecord(sourceHandle);

		if (!alias.contract.alias.allowExplicitAliasing || !source.contract.alias.allowExplicitAliasing)
		{
			return false;
		}

		if (!CanCrossQueueAlias(alias.contract, source.contract))
		{
			return false;
		}

		if (alias.contract.visibility != source.contract.visibility)
		{
			return false;
		}

		if (source.physicalId.has_value())
		{
			if (!IsImageCompatible(alias.desc, m_physicalImages[*source.physicalId].resource))
			{
				return false;
			}
		}
		else
		{
			if (alias.desc.extent.width > source.desc.extent.width ||
				alias.desc.extent.height > source.desc.extent.height ||
				alias.desc.extent.depth > source.desc.extent.depth)
			{
				return false;
			}

			if (alias.desc.format != source.desc.format)
			{
				return false;
			}

			if (alias.desc.mipLevels > source.desc.mipLevels || alias.desc.arrayLayers > source.desc.arrayLayers)
			{
				return false;
			}

			if ((alias.desc.usage & ~source.desc.usage) != 0)
			{
				return false;
			}
		}

		if (alias.contract.lifetime.Overlaps(source.contract.lifetime))
		{
			return false;
		}

		alias.aliasSourceId = sourceHandle.id - 1;
		alias.physicalId.reset();
		return true;
	}

	UniqueBuffer& ResourcePool::MaterializeBuffer(const VirtualBufferHandle handle, const BufferFactory& factory)
	{
		auto& record = RequireBufferRecord(handle);

		if (record.aliasSourceId.has_value())
		{
			const auto sourceHandle = VirtualBufferHandle{
				.id = *record.aliasSourceId + 1,
				.generation = m_virtualBuffers[*record.aliasSourceId].generation,
			};
			auto& sourceRecord = RequireBufferRecord(sourceHandle);

			if (record.contract.lifetime.Overlaps(sourceRecord.contract.lifetime))
			{
				throw VulkanError("Aliased buffer resources have overlapping lifetime windows.");
			}

			auto& sourceResource = MaterializeBuffer(sourceHandle, factory);
			record.physicalId = sourceRecord.physicalId;
			if (record.physicalId.has_value())
			{
				auto& physical = m_physicalBuffers[*record.physicalId];
				if (!ContainsOwner(physical.owners, handle.id - 1))
				{
					physical.owners.push_back(handle.id - 1);
				}
			}
			return sourceResource;
		}

		if (record.physicalId.has_value())
		{
			return m_physicalBuffers[*record.physicalId].resource;
		}

		if (record.contract.transient && record.contract.alias.allowAutomaticAliasing)
		{
			for (std::uint32_t i = 0; i < m_physicalBuffers.size(); ++i)
			{
				auto& physical = m_physicalBuffers[i];
				if (!IsBufferCompatible(record.desc, physical.resource))
				{
					continue;
				}

				if (physical.visibility != record.contract.visibility)
				{
					continue;
				}

				if (physical.queue != record.contract.lifetime.queue && !record.contract.alias.allowCrossQueueAliasing)
				{
					continue;
				}

				if (!CanAliasWithOwners(record.contract, physical.owners, m_virtualBuffers))
				{
					continue;
				}

				record.physicalId = i;
				physical.owners.push_back(handle.id - 1);
				return physical.resource;
			}
		}

		UniqueBuffer resource = factory(record.desc);
		m_physicalBuffers.push_back(BufferPhysicalRecord{
			.resource = std::move(resource),
			.owners = { handle.id - 1 },
			.visibility = record.contract.visibility,
			.queue = record.contract.lifetime.queue,
			});

		record.physicalId = static_cast<std::uint32_t>(m_physicalBuffers.size() - 1);
		return m_physicalBuffers.back().resource;
	}

	UniqueImage& ResourcePool::MaterializeImage(const VirtualImageHandle handle, const ImageFactory& factory)
	{
		auto& record = RequireImageRecord(handle);

		if (record.aliasSourceId.has_value())
		{
			const auto sourceHandle = VirtualImageHandle{
				.id = *record.aliasSourceId + 1,
				.generation = m_virtualImages[*record.aliasSourceId].generation,
			};
			auto& sourceRecord = RequireImageRecord(sourceHandle);

			if (record.contract.lifetime.Overlaps(sourceRecord.contract.lifetime))
			{
				throw VulkanError("Aliased image resources have overlapping lifetime windows.");
			}

			auto& sourceResource = MaterializeImage(sourceHandle, factory);
			record.physicalId = sourceRecord.physicalId;
			if (record.physicalId.has_value())
			{
				auto& physical = m_physicalImages[*record.physicalId];
				if (!ContainsOwner(physical.owners, handle.id - 1))
				{
					physical.owners.push_back(handle.id - 1);
				}
			}
			EnsureImageVisibilityBindings(sourceResource, record.contract);
			return sourceResource;
		}

		if (record.physicalId.has_value())
		{
			auto& existing = m_physicalImages[*record.physicalId].resource;
			EnsureImageVisibilityBindings(existing, record.contract);
			return existing;
		}

		if (record.contract.transient && record.contract.alias.allowAutomaticAliasing)
		{
			for (std::uint32_t i = 0; i < m_physicalImages.size(); ++i)
			{
				auto& physical = m_physicalImages[i];
				if (!IsImageCompatible(record.desc, physical.resource))
				{
					continue;
				}

				if (physical.visibility != record.contract.visibility)
				{
					continue;
				}

				if (physical.queue != record.contract.lifetime.queue && !record.contract.alias.allowCrossQueueAliasing)
				{
					continue;
				}

				if (!CanAliasWithOwners(record.contract, physical.owners, m_virtualImages))
				{
					continue;
				}

				record.physicalId = i;
				physical.owners.push_back(handle.id - 1);
				EnsureImageVisibilityBindings(physical.resource, record.contract);
				return physical.resource;
			}
		}

		UniqueImage resource = factory(record.desc);
		EnsureImageVisibilityBindings(resource, record.contract);
		m_physicalImages.push_back(ImagePhysicalRecord{
			.resource = std::move(resource),
			.owners = { handle.id - 1 },
			.visibility = record.contract.visibility,
			.queue = record.contract.lifetime.queue,
			});

		record.physicalId = static_cast<std::uint32_t>(m_physicalImages.size() - 1);
		return m_physicalImages.back().resource;
	}

	const UniqueBuffer* ResourcePool::TryGetBuffer(const VirtualBufferHandle handle) const
	{
		const auto& record = RequireBufferRecord(handle);
		if (!record.physicalId.has_value())
		{
			return nullptr;
		}

		return &m_physicalBuffers[*record.physicalId].resource;
	}

	const UniqueImage* ResourcePool::TryGetImage(const VirtualImageHandle handle) const
	{
		const auto& record = RequireImageRecord(handle);
		if (!record.physicalId.has_value())
		{
			return nullptr;
		}

		return &m_physicalImages[*record.physicalId].resource;
	}

	ResourcePool::BufferVirtualRecord& ResourcePool::RequireBufferRecord(const VirtualBufferHandle handle)
	{
		if (!handle.IsValid() || handle.id > m_virtualBuffers.size())
		{
			throw VulkanError("Invalid virtual buffer handle.");
		}

		auto& record = m_virtualBuffers[handle.id - 1];
		if (record.generation != handle.generation)
		{
			throw VulkanError("Stale virtual buffer handle generation.");
		}

		return record;
	}

	const ResourcePool::BufferVirtualRecord& ResourcePool::RequireBufferRecord(const VirtualBufferHandle handle) const
	{
		if (!handle.IsValid() || handle.id > m_virtualBuffers.size())
		{
			throw VulkanError("Invalid virtual buffer handle.");
		}

		const auto& record = m_virtualBuffers[handle.id - 1];
		if (record.generation != handle.generation)
		{
			throw VulkanError("Stale virtual buffer handle generation.");
		}

		return record;
	}

	ResourcePool::ImageVirtualRecord& ResourcePool::RequireImageRecord(const VirtualImageHandle handle)
	{
		if (!handle.IsValid() || handle.id > m_virtualImages.size())
		{
			throw VulkanError("Invalid virtual image handle.");
		}

		auto& record = m_virtualImages[handle.id - 1];
		if (record.generation != handle.generation)
		{
			throw VulkanError("Stale virtual image handle generation.");
		}

		return record;
	}

	const ResourcePool::ImageVirtualRecord& ResourcePool::RequireImageRecord(const VirtualImageHandle handle) const
	{
		if (!handle.IsValid() || handle.id > m_virtualImages.size())
		{
			throw VulkanError("Invalid virtual image handle.");
		}

		const auto& record = m_virtualImages[handle.id - 1];
		if (record.generation != handle.generation)
		{
			throw VulkanError("Stale virtual image handle generation.");
		}

		return record;
	}

	bool ResourcePool::CanAliasWithOwners(
		const ResourceContract& candidateContract,
		const std::vector<std::uint32_t>& ownerIds,
		const std::vector<BufferVirtualRecord>& records) const
	{
		for (const auto ownerId : ownerIds)
		{
			if (ownerId >= records.size())
			{
				continue;
			}

			const auto& ownerContract = records[ownerId].contract;
			if (candidateContract.lifetime.Overlaps(ownerContract.lifetime))
			{
				return false;
			}

			if (!CanCrossQueueAlias(candidateContract, ownerContract))
			{
				return false;
			}

			if (candidateContract.visibility != ownerContract.visibility)
			{
				return false;
			}
		}

		return true;
	}

	bool ResourcePool::CanAliasWithOwners(
		const ResourceContract& candidateContract,
		const std::vector<std::uint32_t>& ownerIds,
		const std::vector<ImageVirtualRecord>& records) const
	{
		for (const auto ownerId : ownerIds)
		{
			if (ownerId >= records.size())
			{
				continue;
			}

			const auto& ownerContract = records[ownerId].contract;
			if (candidateContract.lifetime.Overlaps(ownerContract.lifetime))
			{
				return false;
			}

			if (!CanCrossQueueAlias(candidateContract, ownerContract))
			{
				return false;
			}

			if (candidateContract.visibility != ownerContract.visibility)
			{
				return false;
			}
		}

		return true;
	}

	void ResourcePool::EnsureImageVisibilityBindings(UniqueImage& image, const ResourceContract& contract)
	{
		if (contract.visibility == ResourceVisibility::BindlessSampled)
		{
			if (m_bindlessImageConfig.manager == nullptr || m_bindlessImageConfig.device == VK_NULL_HANDLE)
			{
				throw VulkanError("Bindless sampled image requested, but ResourcePool bindless image config is not set.");
			}

			image.EnsureBindlessSampled(
				*m_bindlessImageConfig.manager,
				m_bindlessImageConfig.device,
				m_bindlessImageConfig.sampledAspectMask,
				m_bindlessImageConfig.sampledLayout);
		}
	}

	bool ResourcePool::IsBufferCompatible(const BufferResourceDesc& requested, const UniqueBuffer& existing)
	{
		if (!existing)
		{
			return false;
		}

		if (requested.size > existing.GetSize())
		{
			return false;
		}

		return (requested.usage & ~existing.GetUsage()) == 0;
	}

	bool ResourcePool::IsImageCompatible(const ImageResourceDesc& requested, const UniqueImage& existing)
	{
		if (!existing)
		{
			return false;
		}

		const auto extent = existing.GetExtent();
		if (requested.extent.width > extent.width ||
			requested.extent.height > extent.height ||
			requested.extent.depth > extent.depth)
		{
			return false;
		}

		if (requested.format != existing.GetFormat())
		{
			return false;
		}

		if (requested.mipLevels > existing.GetMipLevels() || requested.arrayLayers > existing.GetArrayLayers())
		{
			return false;
		}

		return (requested.usage & ~existing.GetUsage()) == 0;
	}
}

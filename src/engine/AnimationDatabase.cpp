#include "AnimationDatabase.hpp"

#include <algorithm>
#include <cstring>

#include "AetherExceptions.hpp"

namespace aether
{
	AnimationDatabase AnimationDatabase::Create(VkDevice device, VmaAllocator allocator, const assets::GltfAsset& asset)
	{
		AnimationDatabase db;
		db.m_device = device;
		db.m_allocator = allocator;
		db.m_nodeCount = static_cast<std::uint32_t>(asset.nodes.size());
		db.m_skinCount = static_cast<std::uint32_t>(asset.skins.size());

		if (asset.animations.empty())
		{
			// Return empty but valid database.
			return db;
		}

		// First pass: compute sizes and offsets.
		std::vector<GpuClip> gpuClips;
		std::vector<GpuChannel> gpuChannels;
		std::vector<float> allTimes;
		std::vector<glm::vec4> allValues;
		std::string allStrings;
		std::vector<std::int32_t> nodeParents;
		std::vector<glm::vec4> bindTranslations;
		std::vector<glm::vec4> bindRotations;
		std::vector<glm::vec4> bindScales;
		std::vector<GpuSkinMeta> skinMetas;
		std::vector<std::uint32_t> skinJoints;
		std::vector<glm::mat4> skinInverseBinds;

		std::uint32_t currentChannelOffset = 0;

		for (const auto& srcClip: asset.animations)
		{
			GpuClip gpuClip{};
			gpuClip.nameOffset = static_cast<std::uint32_t>(allStrings.size());
			gpuClip.nameLength = static_cast<std::uint32_t>(srcClip.name.size());
			gpuClip.channelOffset = currentChannelOffset;
			gpuClip.channelCount = static_cast<std::uint32_t>(srcClip.channels.size());
			gpuClip.duration = 0.0f;

			allStrings += srcClip.name;

			for (const auto& srcCh: srcClip.channels)
			{
				GpuChannel gpuCh{};
				gpuCh.nodeIndex = srcCh.nodeIndex;
				gpuCh.animPath = static_cast<std::uint8_t>(srcCh.path);
				gpuCh.interpolation = static_cast<std::uint8_t>(srcCh.interpolation);
				gpuCh.timesOffset = static_cast<std::uint32_t>(allTimes.size() * sizeof(float));
				gpuCh.timesCount = static_cast<std::uint32_t>(srcCh.times.size());
				allTimes.insert(allTimes.end(), srcCh.times.begin(), srcCh.times.end());

				if (!srcCh.times.empty())
				{
					gpuClip.duration = std::max(gpuClip.duration, srcCh.times.back());
				}

				gpuCh.valuesOffset = static_cast<std::uint32_t>(allValues.size() * sizeof(glm::vec4));
				gpuCh.valuesCount = static_cast<std::uint32_t>(srcCh.values.size());
				allValues.insert(allValues.end(), srcCh.values.begin(), srcCh.values.end());

				gpuChannels.push_back(gpuCh);
				++currentChannelOffset;
			}

			gpuClips.push_back(gpuClip);
		}

		db.m_clips = gpuClips;
		db.m_clipNames = allStrings;

		nodeParents.reserve(asset.nodes.size());
		bindTranslations.reserve(asset.nodes.size());
		bindRotations.reserve(asset.nodes.size());
		bindScales.reserve(asset.nodes.size());
		for (const auto& n: asset.nodes)
		{
			nodeParents.push_back(n.parentIndex);
			bindTranslations.emplace_back(n.translation, 0.0f);
			bindRotations.emplace_back(n.rotation.x, n.rotation.y, n.rotation.z, n.rotation.w);
			bindScales.emplace_back(n.scale, 0.0f);
		}

		skinMetas.reserve(asset.skins.size());
		for (const auto& s: asset.skins)
		{
			GpuSkinMeta meta{};
			meta.jointOffset = static_cast<std::uint32_t>(skinJoints.size());
			meta.jointCount = static_cast<std::uint32_t>(s.joints.size());
			meta.inverseBindOffset = static_cast<std::uint32_t>(skinInverseBinds.size() * sizeof(glm::mat4));

			skinJoints.insert(skinJoints.end(), s.joints.begin(), s.joints.end());

			if (!s.inverseBindMatrices.empty())
			{
				skinInverseBinds.insert(skinInverseBinds.end(), s.inverseBindMatrices.begin(), s.inverseBindMatrices.end());
			}
			else
			{
				for (std::size_t i = 0; i < s.joints.size(); ++i)
				{
					skinInverseBinds.emplace_back(1.0f);
				}
			}

			skinMetas.push_back(meta);
		}

		const VmaAllocationCreateInfo uploadAllocInfo{
			.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
			.usage = VMA_MEMORY_USAGE_AUTO,
		};

		if (!gpuClips.empty())
		{
			const VkBufferCreateInfo clipsInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = gpuClips.size() * sizeof(GpuClip),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_clipsBuffer = UniqueBuffer::Create(allocator, device, clipsInfo, uploadAllocInfo);

			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_clipsBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation clips buffer");
			}
			std::memcpy(mapped, gpuClips.data(), static_cast<size_t>(clipsInfo.size));
			vmaUnmapMemory(allocator, db.m_clipsBuffer.GetAllocation());
		}

		if (!gpuChannels.empty())
		{
			const VkBufferCreateInfo channelsInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = gpuChannels.size() * sizeof(GpuChannel),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_channelsBuffer = UniqueBuffer::Create(allocator, device, channelsInfo, uploadAllocInfo);

			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_channelsBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation channels buffer");
			}
			std::memcpy(mapped, gpuChannels.data(), static_cast<size_t>(channelsInfo.size));
			vmaUnmapMemory(allocator, db.m_channelsBuffer.GetAllocation());
		}

		if (!allTimes.empty())
		{
			const VkBufferCreateInfo timesInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = allTimes.size() * sizeof(float),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_timesBuffer = UniqueBuffer::Create(allocator, device, timesInfo, uploadAllocInfo);

			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_timesBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation times buffer");
			}
			std::memcpy(mapped, allTimes.data(), static_cast<size_t>(timesInfo.size));
			vmaUnmapMemory(allocator, db.m_timesBuffer.GetAllocation());
		}

		if (!allValues.empty())
		{
			const VkBufferCreateInfo valuesInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = allValues.size() * sizeof(glm::vec4),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_valuesBuffer = UniqueBuffer::Create(allocator, device, valuesInfo, uploadAllocInfo);

			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_valuesBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation values buffer");
			}
			std::memcpy(mapped, allValues.data(), static_cast<size_t>(valuesInfo.size));
			vmaUnmapMemory(allocator, db.m_valuesBuffer.GetAllocation());
		}

		if (!allStrings.empty())
		{
			const VkBufferCreateInfo stringsInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = allStrings.size(),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_stringsBuffer = UniqueBuffer::Create(allocator, device, stringsInfo, uploadAllocInfo);

			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_stringsBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation strings buffer");
			}
			std::memcpy(mapped, allStrings.data(), allStrings.size());
			vmaUnmapMemory(allocator, db.m_stringsBuffer.GetAllocation());
		}

		if (!nodeParents.empty())
		{
			const VkBufferCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = nodeParents.size() * sizeof(std::int32_t),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_nodeParentsBuffer = UniqueBuffer::Create(allocator, device, info, uploadAllocInfo);
			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_nodeParentsBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation node-parents buffer");
			}
			std::memcpy(mapped, nodeParents.data(), static_cast<size_t>(info.size));
			vmaUnmapMemory(allocator, db.m_nodeParentsBuffer.GetAllocation());
		}

		if (!bindTranslations.empty())
		{
			const VkBufferCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = bindTranslations.size() * sizeof(glm::vec4),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_bindTranslationsBuffer = UniqueBuffer::Create(allocator, device, info, uploadAllocInfo);
			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_bindTranslationsBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation bind-translation buffer");
			}
			std::memcpy(mapped, bindTranslations.data(), static_cast<size_t>(info.size));
			vmaUnmapMemory(allocator, db.m_bindTranslationsBuffer.GetAllocation());
		}

		if (!bindRotations.empty())
		{
			const VkBufferCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = bindRotations.size() * sizeof(glm::vec4),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_bindRotationsBuffer = UniqueBuffer::Create(allocator, device, info, uploadAllocInfo);
			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_bindRotationsBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation bind-rotation buffer");
			}
			std::memcpy(mapped, bindRotations.data(), static_cast<size_t>(info.size));
			vmaUnmapMemory(allocator, db.m_bindRotationsBuffer.GetAllocation());
		}

		if (!bindScales.empty())
		{
			const VkBufferCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = bindScales.size() * sizeof(glm::vec4),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_bindScalesBuffer = UniqueBuffer::Create(allocator, device, info, uploadAllocInfo);
			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_bindScalesBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation bind-scale buffer");
			}
			std::memcpy(mapped, bindScales.data(), static_cast<size_t>(info.size));
			vmaUnmapMemory(allocator, db.m_bindScalesBuffer.GetAllocation());
		}

		if (!skinMetas.empty())
		{
			const VkBufferCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = skinMetas.size() * sizeof(GpuSkinMeta),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_skinMetasBuffer = UniqueBuffer::Create(allocator, device, info, uploadAllocInfo);
			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_skinMetasBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation skin-meta buffer");
			}
			std::memcpy(mapped, skinMetas.data(), static_cast<size_t>(info.size));
			vmaUnmapMemory(allocator, db.m_skinMetasBuffer.GetAllocation());
		}

		if (!skinJoints.empty())
		{
			const VkBufferCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = skinJoints.size() * sizeof(std::uint32_t),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_skinJointsBuffer = UniqueBuffer::Create(allocator, device, info, uploadAllocInfo);
			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_skinJointsBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation skin-joints buffer");
			}
			std::memcpy(mapped, skinJoints.data(), static_cast<size_t>(info.size));
			vmaUnmapMemory(allocator, db.m_skinJointsBuffer.GetAllocation());
		}

		if (!skinInverseBinds.empty())
		{
			const VkBufferCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = skinInverseBinds.size() * sizeof(glm::mat4),
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			db.m_skinInverseBindsBuffer = UniqueBuffer::Create(allocator, device, info, uploadAllocInfo);
			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, db.m_skinInverseBindsBuffer.GetAllocation(), &mapped);
			if (mapResult != VK_SUCCESS || mapped == nullptr)
			{
				throw VulkanError("Failed to map animation inverse-bind buffer");
			}
			std::memcpy(mapped, skinInverseBinds.data(), static_cast<size_t>(info.size));
			vmaUnmapMemory(allocator, db.m_skinInverseBindsBuffer.GetAllocation());
		}

		return db;
	}

	void AnimationDatabase::Destroy()
	{
		m_skinInverseBindsBuffer.Reset();
		m_skinJointsBuffer.Reset();
		m_skinMetasBuffer.Reset();
		m_bindScalesBuffer.Reset();
		m_bindRotationsBuffer.Reset();
		m_bindTranslationsBuffer.Reset();
		m_nodeParentsBuffer.Reset();
		m_stringsBuffer.Reset();
		m_valuesBuffer.Reset();
		m_timesBuffer.Reset();
		m_channelsBuffer.Reset();
		m_clipsBuffer.Reset();
		m_clips.clear();
		m_clipNames.clear();
		m_nodeCount = 0;
		m_skinCount = 0;
		m_device = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
	}

	VkDeviceAddress AnimationDatabase::GetClipsAddr() const
	{
		return m_clipsBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetChannelsAddr() const
	{
		return m_channelsBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetTimesAddr() const
	{
		return m_timesBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetValuesAddr() const
	{
		return m_valuesBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetStringsAddr() const
	{
		return m_stringsBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetNodeParentsAddr() const
	{
		return m_nodeParentsBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetBindTranslationsAddr() const
	{
		return m_bindTranslationsBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetBindRotationsAddr() const
	{
		return m_bindRotationsBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetBindScalesAddr() const
	{
		return m_bindScalesBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetSkinMetasAddr() const
	{
		return m_skinMetasBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetSkinJointsAddr() const
	{
		return m_skinJointsBuffer.GetDeviceAddress();
	}

	VkDeviceAddress AnimationDatabase::GetSkinInverseBindsAddr() const
	{
		return m_skinInverseBindsBuffer.GetDeviceAddress();
	}

	std::string_view AnimationDatabase::GetClipName(std::uint32_t clipIndex) const
	{
		if (clipIndex >= m_clips.size())
		{
			return "";
		}
		const auto& clip = m_clips[clipIndex];
		return std::string_view(m_clipNames.data() + clip.nameOffset, clip.nameLength);
	}
} // namespace aether

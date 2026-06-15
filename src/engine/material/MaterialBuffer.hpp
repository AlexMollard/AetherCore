#pragma once

#include <cstdint>
#include <mutex>
#include <vector>
#include "gpu/GpuTypes.hpp"

#include "material/GpuMaterial.hpp"
#include "vulkan/UniqueBuffer.hpp"

namespace aether
{
	class VulkanContext;

	class MaterialBuffer
	{
	public:
		static constexpr std::uint32_t kMaxMaterials = 4096;
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		MaterialBuffer() = default;
		~MaterialBuffer();

		MaterialBuffer(const MaterialBuffer&) = AE_DELETE_MSG("use std::move");
		MaterialBuffer& operator=(const MaterialBuffer&) = AE_DELETE_MSG("use std::move");

		void Initialize(const VulkanContext& ctx);
		void Shutdown();

		[[nodiscard]] bool IsInitialized() const
		{
			return m_device != nullptr;
		}

		[[nodiscard]] std::uint32_t AllocateSlot();

		void FreeSlot(std::uint32_t slot);

		void Write(std::uint32_t slot, const GpuMaterial& material);

		[[nodiscard]] gpu::DeviceAddress GetDeviceAddress() const
		{
			return m_address;
		}

		[[nodiscard]] std::uint64_t GetDeviceAddressU64() const
		{
			return static_cast<std::uint64_t>(m_address);
		}

	private:
		mutable std::mutex m_mutex;
		gpu::Device m_device = nullptr;
		gpu::Allocator m_allocator = nullptr;
		UniqueBuffer m_buffer;
		GpuMaterial* m_mapped = nullptr;
		gpu::DeviceAddress m_address = 0;
		std::vector<uint32_t> m_freeSlots;
	};
} // namespace aether

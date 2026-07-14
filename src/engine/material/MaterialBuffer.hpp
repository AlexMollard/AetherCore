#pragma once

#include <cstdint>
#include <mutex>
#include <vector>
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"

#include "material/DeferredSlotFreeList.hpp"
#include "material/GpuMaterial.hpp"
#include "material/IMaterialSlotSink.hpp"

namespace aether
{
	class MaterialBuffer final : public IMaterialSlotSink
	{
	public:
		static constexpr std::uint32_t kMaxMaterials = 4096;
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		MaterialBuffer() = default;
		~MaterialBuffer() override;

		MaterialBuffer(const MaterialBuffer&) = AE_DELETE_MSG("use std::move");
		MaterialBuffer& operator=(const MaterialBuffer&) = AE_DELETE_MSG("use std::move");

		void Initialize();
		void Shutdown();

		[[nodiscard]] bool IsInitialized() const
		{
			return m_handle.IsValid();
		}

		[[nodiscard]] std::uint32_t AllocateSlot() override;

		void FreeSlot(std::uint32_t slot) override;

		void Write(std::uint32_t slot, const GpuMaterial& material) override;

		[[nodiscard]] std::uint32_t Capacity() const override
		{
			return kMaxMaterials;
		}

		void AdvanceFrame(std::uint64_t frameIndex);

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
		gpu::BufferHandle m_handle{};
		GpuMaterial* m_mapped = nullptr;
		gpu::DeviceAddress m_address = 0;
		DeferredSlotFreeList m_slotAllocator;
	};
} // namespace aether

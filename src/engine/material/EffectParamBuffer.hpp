#pragma once

#include <cstdint>
#include <mutex>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"

#include "material/DeferredSlotFreeList.hpp"
#include "material/EffectParams.hpp"

namespace aether
{
	// hands out a unique slot per effect entity so two same-effect entities never
	class EffectParamBuffer
	{
	public:
		static constexpr std::uint32_t kMaxEffects = 4096;
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		EffectParamBuffer() = default;
		~EffectParamBuffer();

		EffectParamBuffer(const EffectParamBuffer&) = AE_DELETE_MSG("use std::move");
		EffectParamBuffer& operator=(const EffectParamBuffer&) = AE_DELETE_MSG("use std::move");

		void Initialize();
		void Shutdown();

		[[nodiscard]] bool IsInitialized() const
		{
			return m_handle.IsValid();
		}

		[[nodiscard]] std::uint32_t AllocateSlot();
		void FreeSlot(std::uint32_t slot);
		void Write(std::uint32_t slot, const EffectParams& params);
		void AdvanceFrame(std::uint64_t frameIndex);

		[[nodiscard]] std::uint64_t GetDeviceAddressU64() const
		{
			return static_cast<std::uint64_t>(m_address);
		}

	private:
		mutable std::mutex m_mutex;
		gpu::BufferHandle m_handle{};
		EffectParams* m_mapped = nullptr;
		gpu::DeviceAddress m_address = 0;
		DeferredSlotFreeList m_slotAllocator;
	};
} // namespace aether

#pragma once

#include <cstdint>

#include "material/GpuMaterial.hpp"

namespace aether
{
	// Backend that owns GPU material slots. MaterialBuffer is the production impl;
	// tests inject a fake. No Vulkan types leak through this interface.
	class IMaterialSlotSink
	{
	public:
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		virtual ~IMaterialSlotSink() = default;

		[[nodiscard]] virtual std::uint32_t AllocateSlot() = 0;
		virtual void FreeSlot(std::uint32_t slot) = 0;
		virtual void Write(std::uint32_t slot, const GpuMaterial& material) = 0;
		[[nodiscard]] virtual std::uint32_t Capacity() const = 0;
	};
} // namespace aether

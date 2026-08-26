#pragma once

#include <cstdint>
#include <vector>

#include "gpu/GpuFormat.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"

namespace aether
{
	class BindlessManager;

	// Shelf-packs a 4K x 4K R32_SFLOAT local shadow atlas. The image itself belongs to the
	// render graph; this is only the layout.
	//
	// The extent is not arbitrary: LocalShadowService renders a hard-capped number
	// of fixed-resolution shadow entries per frame, so the packer can never need
	// more room than this. LocalShadowService static_asserts that bound against
	// these constants - raise the entry cap or the per-entry resolution there and
	// the assert tells you the atlas has to grow with it. Every byte here is paid
	// four times over (atlas + depth + two full-size VSM blur scratch buffers), so
	// do not round it up "just in case".
	class ShadowAtlasManager
	{
	public:
		static constexpr std::uint32_t kAtlasWidth = 4096u;
		static constexpr std::uint32_t kAtlasHeight = 4096u;
		static constexpr gpu::Format kAtlasFormat = gpu::Format::R32Sfloat;

		struct Region
		{
			std::uint32_t x = 0;
			std::uint32_t y = 0;
			std::uint32_t width = 0;
			std::uint32_t height = 0;

			[[nodiscard]] bool IsValid() const
			{
				return width > 0 && height > 0;
			}
		};

		// Reset all shelves for a new frame. Must be called once per frame
		void Reset();

		[[nodiscard]] Region Allocate(std::uint32_t width, std::uint32_t height);

		[[nodiscard]] Region GetUsedBounds() const
		{
			return m_usedBounds;
		}

	private:
		struct Shelf
		{
			std::uint32_t y = 0;
			std::uint32_t height = 0;
			std::uint32_t cursorX = 0;
		};

		std::vector<Shelf> m_shelves;
		Region m_usedBounds{};
	};
} // namespace aether

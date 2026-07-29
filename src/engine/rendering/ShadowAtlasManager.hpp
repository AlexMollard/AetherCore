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

	// Manages a 4K x 4K R32G32_SFLOAT VSM shadow atlas with shelf-packing.
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
		static constexpr gpu::Format kAtlasFormat = gpu::Format::R32G32Sfloat;

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

		void Initialize(BindlessManager& bindless);
		void Shutdown();

		// Reset all shelves for a new frame. Must be called once per frame
		void Reset();

		[[nodiscard]] Region Allocate(std::uint32_t width, std::uint32_t height);

		[[nodiscard]] Region GetUsedBounds() const
		{
			return m_usedBounds;
		}

		[[nodiscard]] gpu::Image GetAtlasImage() const
		{
			return m_atlasImage;
		}

		[[nodiscard]] gpu::ImageView GetAtlasView() const
		{
			return m_atlasView;
		}

		[[nodiscard]] std::uint32_t GetBindlessSlot() const
		{
			return m_bindlessSlot;
		}

	private:
		struct Shelf
		{
			std::uint32_t y = 0;
			std::uint32_t height = 0;
			std::uint32_t cursorX = 0;
		};

		gpu::TextureHandle m_atlasHandle{};
		gpu::Image m_atlasImage = nullptr;
		gpu::ImageView m_atlasView = nullptr;
		std::uint32_t m_bindlessSlot = 0xFFFFFFFFu;
		BindlessManager* m_bindless = nullptr;
		std::vector<Shelf> m_shelves;
		Region m_usedBounds{};
	};
} // namespace aether

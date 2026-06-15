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
	class VulkanContext;

	// Manages an 8K x 8K R32G32_SFLOAT VSM shadow atlas with shelf-packing
	// allocation. The atlas is re-packed every frame (reset + re-allocate)
	// based on the current set of active shadow-casting lights.
	//
	// Shelf-packing: shelves are rows of a fixed height series. Each shelf
	// fills left-to-right. When a shelf overflows, a new shelf is created
	// below.
	//
	// Storage path: stores a single gpu::TextureHandle (8 bytes, typed,
	// generation-checked). Allocated through gpu::ResourceRegistry::
	// CreateTexture which uses the registry's 3-frame deferred-destruction
	// ring. The cached image/view handles are resolved once at Init.
	class ShadowAtlasManager
	{
	public:
		static constexpr std::uint32_t kAtlasWidth = 8192u;
		static constexpr std::uint32_t kAtlasHeight = 8192u;
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

		void Initialize(const VulkanContext& ctx, BindlessManager& bindless);
		void Shutdown();

		// Reset all shelves for a new frame. Must be called once per frame
		// before any Allocate calls.
		void Reset();

		// Allocate a region of the given size in the atlas.
		// Returns an invalid region (width=0) on failure (atlas full).
		[[nodiscard]] Region Allocate(std::uint32_t width, std::uint32_t height);

		// Get the bounding box of all allocated regions this frame.
		// Returns {0,0,0,0} if nothing is allocated.
		[[nodiscard]] Region GetUsedBounds() const
		{
			return m_usedBounds;
		}

		// Access the atlas image for render graph registration.
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
		gpu::Image m_atlasImage = nullptr; // cached for render-graph registration
		gpu::ImageView m_atlasView = nullptr; // cached for shader bindings
		std::uint32_t m_bindlessSlot = 0xFFFFFFFFu;
		BindlessManager* m_bindless = nullptr;
		std::vector<Shelf> m_shelves;
		Region m_usedBounds{}; // Bounding box of all allocated regions this frame
	};
} // namespace aether

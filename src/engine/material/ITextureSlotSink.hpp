#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "material/Texture.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	// Move-only holder the TextureRegistry Entry owns. Production wraps a real
	// Texture (owns a gpu::TextureHandle + bindless slot); the test fake wraps a
	// bare slot. The registry only ever calls GetBindlessSlot(), so it never needs
	// a live GPU. Mirrors how MaterialRegistry stores a plain slot index.
	class TextureResource
	{
	public:
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		TextureResource() = default;

		explicit TextureResource(Texture&& texture)
		      : m_texture(std::move(texture)), m_slot(m_texture.GetBindlessSlot())
		{
		}

		// Test/GPU-free construction: a bare slot with no backing Texture.
		explicit TextureResource(std::uint32_t slot)
		      : m_slot(slot)
		{
		}

		TextureResource(const TextureResource&) = delete;
		TextureResource& operator=(const TextureResource&) = delete;
		TextureResource(TextureResource&&) noexcept = default;
		TextureResource& operator=(TextureResource&&) noexcept = default;

		[[nodiscard]] std::uint32_t GetBindlessSlot() const
		{
			return m_slot;
		}

	private:
		Texture m_texture{}; // empty (no-op Destroy) for the bare-slot path
		std::uint32_t m_slot = kInvalidSlot;
	};

	// Backend that resolves + loads textures for the registry. AssetTextureSink is
	// the production impl; tests inject a fake. No Vulkan types leak through.
	class ITextureSlotSink
	{
	public:
		virtual ~ITextureSlotSink() = default;

		// Resolve the VFS path to the canonical string the sink will actually load
		// (after .texture-sibling + mount normalization). The registry hashes and
		// dedups on THIS string, not the raw argument.
		[[nodiscard]] virtual std::string ResolvePath(std::string_view path) const = 0;

		// Blocking (v1): decode + upload; returns a resident TextureResource that
		// owns its bindless slot, or an error. Called with the already-resolved path.
		[[nodiscard]] virtual Expected<TextureResource> Load(std::string_view resolvedPath) = 0;

		// Max number of live entries the sink can back (bindless capacity).
		[[nodiscard]] virtual std::uint32_t Capacity() const = 0;
	};
} // namespace aether

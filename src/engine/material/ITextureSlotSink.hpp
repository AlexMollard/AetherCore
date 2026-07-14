#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "material/Texture.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	// bare slot. The registry only ever calls GetBindlessSlot(), so it never needs
	class TextureResource
	{
	public:
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		TextureResource() = default;
		~TextureResource() = default;

		explicit TextureResource(Texture&& texture)
		      : m_texture(std::move(texture)), m_slot(m_texture.GetBindlessSlot())
		{
		}

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
		Texture m_texture;
		std::uint32_t m_slot = kInvalidSlot;
	};

	class ITextureSlotSink
	{
	public:
		virtual ~ITextureSlotSink() = default;

		[[nodiscard]] virtual std::string ResolvePath(std::string_view path) const = 0;

		[[nodiscard]] virtual Expected<TextureResource> Load(std::string_view resolvedPath) = 0;

		[[nodiscard]] virtual std::uint32_t Capacity() const = 0;
	};
} // namespace aether

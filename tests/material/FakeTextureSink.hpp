#pragma once
#include <string>
#include "material/ITextureSlotSink.hpp"

class FakeTextureSink final : public aether::ITextureSlotSink
{
public:
	explicit FakeTextureSink(std::uint32_t capacity = 8)
	      : m_capacity(capacity)
	{
	}

	std::string ResolvePath(std::string_view path) const override
	{
		std::string s(path);
		const auto dot = s.find_last_of('.');
		if (dot != std::string::npos)
		{
			return s.substr(0, dot) + ".texture";
		}
		return s;
	}

	aether::Expected<aether::TextureResource> Load(std::string_view resolvedPath, aether::TextureColorSpace colorSpace) override
	{
		(void) resolvedPath;
		// Recorded so a test can assert that a slot asked for as data was not loaded
		// as colour, which is the whole point of the parameter.
		lastColorSpace = colorSpace;
		if (m_live >= m_capacity)
		{
			return std::unexpected(aether::AetherError::Engine("FakeTextureSink: at capacity"));
		}
		++loadCount;
		++m_live;
		return aether::TextureResource{m_nextSlot++};
	}

	std::uint32_t Capacity() const override
	{
		return m_capacity;
	}

	int loadCount = 0;
	aether::TextureColorSpace lastColorSpace = aether::TextureColorSpace::Srgb;
	std::uint32_t m_nextSlot = 0;

private:
	std::uint32_t m_capacity;
	std::uint32_t m_live = 0;
};

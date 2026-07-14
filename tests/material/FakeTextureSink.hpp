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

	aether::Expected<aether::TextureResource> Load(std::string_view resolvedPath) override
	{
		(void) resolvedPath;
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
	std::uint32_t m_nextSlot = 0;

private:
	std::uint32_t m_capacity;
	std::uint32_t m_live = 0;
};

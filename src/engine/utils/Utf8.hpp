#pragma once

#include <cstddef>
#include <cstdint>

namespace aether
{
	inline bool IsValidUtf8(const char* data, size_t length) noexcept
	{
		if (data == nullptr)
		{
			return length == 0;
		}
		const auto* bytes = reinterpret_cast<const unsigned char*>(data);
		for (size_t index = 0; index < length;)
		{
			const uint32_t lead = bytes[index++];
			if (lead < 0x80)
			{
				continue;
			}
			uint32_t continuationCount;
			uint32_t codePoint;
			uint32_t minimum;
			if (lead >= 0xC2 && lead <= 0xDF)
			{
				continuationCount = 1;
				codePoint = lead & 0x1F;
				minimum = 0x80;
			}
			else if (lead >= 0xE0 && lead <= 0xEF)
			{
				continuationCount = 2;
				codePoint = lead & 0x0F;
				minimum = 0x800;
			}
			else if (lead >= 0xF0 && lead <= 0xF4)
			{
				continuationCount = 3;
				codePoint = lead & 0x07;
				minimum = 0x10000;
			}
			else
			{
				return false;
			}
			if (continuationCount > length - index)
			{
				return false;
			}
			for (uint32_t offset = 0; offset < continuationCount; ++offset)
			{
				const uint32_t byte = bytes[index++];
				if ((byte & 0xC0) != 0x80)
				{
					return false;
				}
				codePoint = (codePoint << 6) | (byte & 0x3F);
			}
			if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
			{
				return false;
			}
		}
		return true;
	}
} // namespace aether

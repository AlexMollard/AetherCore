#include "SpirvProcessor.hpp"

#include <cstdint>
#include <cstring>

namespace SpirvProcessor
{
	namespace
	{
		constexpr uint32_t kMagic       = 0x07230203u;
		constexpr uint32_t kHeaderWords = 5u;

		// Returns true for instructions that carry only debug/name metadata and
		// have no semantic effect on execution or interface layout.
		bool IsStrippable(uint32_t opcode)
		{
			switch (opcode)
			{
				case 2:   // OpSourceContinued
				case 3:   // OpSource
				case 4:   // OpSourceExtension
				case 5:   // OpName
				case 6:   // OpMemberName
				case 7:   // OpString
				case 8:   // OpLine
				case 317: // OpNoLine
				case 330: // OpModuleProcessed
					return true;
				default:
					return false;
			}
		}
	} // namespace

	std::vector<std::byte> Strip(const std::vector<std::byte>& spv)
	{
		if (spv.size() < kHeaderWords * sizeof(uint32_t) || spv.size() % sizeof(uint32_t) != 0)
			return {};

		const uint32_t* words     = reinterpret_cast<const uint32_t*>(spv.data());
		const std::size_t numWords = spv.size() / sizeof(uint32_t);

		if (words[0] != kMagic)
			return {};

		std::vector<uint32_t> out;
		out.reserve(numWords);

		// Copy the five-word SPIR-V header verbatim.
		for (uint32_t i = 0; i < kHeaderWords; ++i)
			out.push_back(words[i]);

		// Walk the instruction stream, copying non-debug instructions.
		std::size_t i = kHeaderWords;
		while (i < numWords)
		{
			const uint32_t word0    = words[i];
			const uint32_t instrLen = word0 >> 16;
			const uint32_t opcode   = word0 & 0xFFFFu;

			if (instrLen == 0 || i + instrLen > numWords)
				return {}; // malformed SPIR-V

			if (!IsStrippable(opcode))
			{
				for (uint32_t j = 0; j < instrLen; ++j)
					out.push_back(words[i + j]);
			}
			i += instrLen;
		}

		std::vector<std::byte> result(out.size() * sizeof(uint32_t));
		std::memcpy(result.data(), out.data(), result.size());
		return result;
	}
} // namespace SpirvProcessor

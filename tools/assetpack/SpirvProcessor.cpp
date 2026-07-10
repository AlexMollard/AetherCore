#include "SpirvProcessor.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace aether::assetpipeline
{
	namespace SpirvProcessor
	{
		namespace
		{
			constexpr uint32_t kMagic = 0x07230203u;
			constexpr uint32_t kHeaderWords = 5u;

			constexpr uint32_t kOpSourceContinued = 2u;
			constexpr uint32_t kOpSource = 3u;
			constexpr uint32_t kOpSourceExtension = 4u;
			constexpr uint32_t kOpName = 5u;
			constexpr uint32_t kOpMemberName = 6u;
			constexpr uint32_t kOpString = 7u;
			constexpr uint32_t kOpLine = 8u;
			constexpr uint32_t kOpExtInstImport = 11u;
			constexpr uint32_t kOpExtInst = 12u;
			constexpr uint32_t kOpNoLine = 317u;
			constexpr uint32_t kOpModuleProcessed = 330u;

			// Decodes a SPIR-V literal string (packed 4 chars per word, little-endian,
			// null-terminated) starting at `words[first]`.
			std::string DecodeLiteralString(const std::vector<uint32_t>& words, std::size_t first, std::size_t end)
			{
				std::string text;
				for (std::size_t w = first; w < end; ++w)
				{
					const uint32_t word = words[w];
					for (uint32_t shift = 0; shift < 32; shift += 8)
					{
						const char c = static_cast<char>((word >> shift) & 0xFFu);
						if (c == '\0')
						{
							return text;
						}
						text += c;
					}
				}
				return text;
			}

			// Returns true for instructions that carry only heavy debug/source metadata
			// and have no semantic effect on execution or interface layout.
			//
			// OpName / OpMemberName (variable and struct-member names) are deliberately
			// KEPT. The Khronos validation layer's shader-object SPIR-V analysis
			// (spirv::VariableBase::FindDebugName, reached via vkCreateShadersEXT)
			// null-dereferences when a push-constant variable has no OpName, which makes
			// a name-stripped engine impossible to run under the validation layer. The
			// names are tiny and also make RenderDoc / validation output readable.
			//
			// OpString handling is conditional: OpStrings are referenced by the
			// NonSemantic.Shader.DebugInfo.100 OpExtInst block (DebugSource et al), so
			// they may only be removed together with that block; and if any OTHER
			// non-semantic set (e.g. NonSemantic.DebugPrintf format strings) survives,
			// OpStrings must be kept or the module would contain dangling references --
			// exactly the spirv-val failure (VUID-VkShaderCreateInfoEXT-pCode-08737)
			// this processor previously shipped.
			bool IsAlwaysStrippable(uint32_t opcode)
			{
				switch (opcode)
				{
					case kOpSourceContinued:
					case kOpSource:
					case kOpSourceExtension:
					case kOpLine:
					case kOpNoLine:
					case kOpModuleProcessed:
						return true;
					default:
						return false;
				}
			}
		} // namespace

		ByteBuffer Strip(std::span<const std::byte> spv)
		{
			if (spv.size() < kHeaderWords * sizeof(uint32_t) || spv.size() % sizeof(uint32_t) != 0)
			{
				return {};
			}

			const std::size_t numWords = spv.size() / sizeof(uint32_t);

			std::vector<uint32_t> words(numWords);
			std::memcpy(words.data(), spv.data(), spv.size());

			if (words[0] != kMagic)
			{
				return {};
			}

			// -- Pass 1: find non-semantic instruction-set imports --------------------
			// Debug-info sets are stripped wholesale (their OpExtInst results are only
			// consumed by other instructions of the same set, so removal is coherent).
			// Any other surviving non-semantic set (DebugPrintf) forces OpStrings to be
			// kept because its instructions reference them.
			std::vector<uint32_t> strippedSetIds;
			bool keepStrings = false;
			{
				std::size_t i = kHeaderWords;
				while (i < numWords)
				{
					const uint32_t word0 = words[i];
					const uint32_t instrLen = word0 >> 16;
					const uint32_t opcode = word0 & 0xFFFFu;
					if (instrLen == 0 || i + instrLen > numWords)
					{
						return {}; // malformed SPIR-V
					}
					if (opcode == kOpExtInstImport && instrLen >= 3)
					{
						const std::string name = DecodeLiteralString(words, i + 2, i + instrLen);
						if (name.starts_with("NonSemantic.Shader.DebugInfo"))
						{
							strippedSetIds.push_back(words[i + 1]);
						}
						else if (name.starts_with("NonSemantic."))
						{
							keepStrings = true;
						}
					}
					i += instrLen;
				}
			}

			const auto isStrippedSet = [&strippedSetIds](uint32_t id)
			{
				for (const uint32_t setId: strippedSetIds)
				{
					if (setId == id)
					{
						return true;
					}
				}
				return false;
			};

			// -- Pass 2: collect the result IDs of every instruction being removed ----
			// slangc also emits OpName for the debug-info instructions themselves; once
			// those instructions are stripped, an OpName pointing at one would be a
			// forward reference to an undefined ID (spirv-val: "forward referenced IDs
			// have not been defined"). Track removed result IDs so pass 3 can drop the
			// names that refer to them.
			std::unordered_set<uint32_t> removedResultIds;
			{
				std::size_t i = kHeaderWords;
				while (i < numWords)
				{
					const uint32_t word0 = words[i];
					const uint32_t instrLen = word0 >> 16;
					const uint32_t opcode = word0 & 0xFFFFu;
					if (opcode == kOpString && !keepStrings && instrLen >= 2)
					{
						removedResultIds.insert(words[i + 1]); // OpString: word1 = result id
					}
					else if (opcode == kOpExtInstImport && instrLen >= 2 && isStrippedSet(words[i + 1]))
					{
						removedResultIds.insert(words[i + 1]);
					}
					else if (opcode == kOpExtInst && instrLen >= 4 && isStrippedSet(words[i + 3]))
					{
						removedResultIds.insert(words[i + 2]); // OpExtInst: word2 = result id
					}
					i += instrLen;
				}
			}

			// -- Pass 3: copy every instruction that survives the strip ---------------
			std::vector<uint32_t> out;
			out.reserve(numWords);

			for (uint32_t i = 0; i < kHeaderWords; ++i)
			{
				out.push_back(words[i]);
			}

			std::size_t i = kHeaderWords;
			while (i < numWords)
			{
				const uint32_t word0 = words[i];
				const uint32_t instrLen = word0 >> 16;
				const uint32_t opcode = word0 & 0xFFFFu;

				if (instrLen == 0 || i + instrLen > numWords)
				{
					return {}; // malformed SPIR-V
				}

				bool strip = IsAlwaysStrippable(opcode);
				if (!strip && opcode == kOpString)
				{
					strip = !keepStrings;
				}
				else if (!strip && opcode == kOpExtInstImport && instrLen >= 2)
				{
					strip = isStrippedSet(words[i + 1]);
				}
				else if (!strip && opcode == kOpExtInst && instrLen >= 4)
				{
					// OpExtInst: result-type, result-id, SET-ID, instruction, operands...
					strip = isStrippedSet(words[i + 3]);
				}
				else if (!strip && (opcode == kOpName || opcode == kOpMemberName) && instrLen >= 2)
				{
					// Drop names whose target instruction was removed above; a kept
					// OpName pointing at a stripped debug-info id is a dangling forward
					// reference and fails spirv-val.
					strip = removedResultIds.contains(words[i + 1]);
				}

				if (!strip)
				{
					for (uint32_t j = 0; j < instrLen; ++j)
					{
						out.push_back(words[i + j]);
					}
				}
				i += instrLen;
			}

			std::vector<std::byte> result(out.size() * sizeof(uint32_t));
			std::memcpy(result.data(), out.data(), result.size());
			return result;
		}
	} // namespace SpirvProcessor
} // namespace aether::assetpipeline

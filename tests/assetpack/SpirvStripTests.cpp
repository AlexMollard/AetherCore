#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "SpirvProcessor.hpp"

using namespace aether;

// Hand-assembled SPIR-V fixtures for SpirvProcessor::Strip. The strip must keep
namespace
{
	constexpr uint32_t kOpSource = 3;
	constexpr uint32_t kOpName = 5;
	constexpr uint32_t kOpString = 7;
	constexpr uint32_t kOpLine = 8;
	constexpr uint32_t kOpExtInstImport = 11;
	constexpr uint32_t kOpExtInst = 12;

	void EmitInstruction(std::vector<uint32_t>& words, uint32_t opcode, const std::vector<uint32_t>& operands)
	{
		words.push_back(((1u + static_cast<uint32_t>(operands.size())) << 16) | opcode);
		words.insert(words.end(), operands.begin(), operands.end());
	}

	std::vector<uint32_t> EncodeLiteralString(std::string_view text)
	{
		std::vector<uint32_t> encoded;
		uint32_t current = 0;
		uint32_t shift = 0;
		for (const char c: text)
		{
			current |= static_cast<uint32_t>(static_cast<unsigned char>(c)) << shift;
			shift += 8;
			if (shift == 32)
			{
				encoded.push_back(current);
				current = 0;
				shift = 0;
			}
		}
		encoded.push_back(current);
		return encoded;
	}

	void EmitInstructionWithString(std::vector<uint32_t>& words, uint32_t opcode, std::vector<uint32_t> leadingOperands, std::string_view text)
	{
		const std::vector<uint32_t> encoded = EncodeLiteralString(text);
		leadingOperands.insert(leadingOperands.end(), encoded.begin(), encoded.end());
		EmitInstruction(words, opcode, leadingOperands);
	}

	std::vector<uint32_t> BuildModule(bool withDebugPrintfImport)
	{
		std::vector<uint32_t> words{0x07230203u, 0x00010600u, 0u, 64u, 0u};
		EmitInstructionWithString(words, kOpExtInstImport, {1u}, "NonSemantic.Shader.DebugInfo.100");
		EmitInstructionWithString(words, kOpExtInstImport, {2u}, "GLSL.std.450");
		if (withDebugPrintfImport)
		{
			EmitInstructionWithString(words, kOpExtInstImport, {5u}, "NonSemantic.DebugPrintf");
		}
		EmitInstructionWithString(words, kOpString, {3u}, "shader.slang");
		EmitInstruction(words, kOpSource, {0u, 0u});
		EmitInstructionWithString(words, kOpName, {4u}, "g_pushConstants");
		EmitInstruction(words, kOpExtInst, {9u, 10u, 1u, 35u, 3u});
		EmitInstructionWithString(words, kOpName, {10u}, "dbgSource");
		EmitInstruction(words, kOpLine, {3u, 12u, 1u});
		return words;
	}

	struct Instruction
	{
		uint32_t opcode;
		std::vector<uint32_t> operands;
	};

	std::vector<Instruction> Disassemble(const std::vector<std::byte>& bytes)
	{
		std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
		std::memcpy(words.data(), bytes.data(), bytes.size());
		std::vector<Instruction> instructions;
		std::size_t i = 5;
		while (i < words.size())
		{
			const uint32_t len = words[i] >> 16;
			Instruction instr{words[i] & 0xFFFFu, {}};
			for (uint32_t j = 1; j < len; ++j)
			{
				instr.operands.push_back(words[i + j]);
			}
			instructions.push_back(std::move(instr));
			i += len;
		}
		return instructions;
	}

	bool ContainsOpcode(const std::vector<Instruction>& instructions, uint32_t opcode)
	{
		for (const Instruction& instr: instructions)
		{
			if (instr.opcode == opcode)
			{
				return true;
			}
		}
		return false;
	}

	std::span<const std::byte> AsBytes(const std::vector<uint32_t>& words)
	{
		return {reinterpret_cast<const std::byte*>(words.data()), words.size() * sizeof(uint32_t)};
	}
}

TEST_CASE("Strip removes the debug-info block coherently and keeps OpName")
{
	const std::vector<uint32_t> module = BuildModule(/*withDebugPrintfImport=*/false);
	const auto stripped = assetpipeline::SpirvProcessor::Strip(AsBytes(module));
	REQUIRE(!stripped.empty());

	const auto instructions = Disassemble(stripped);
	CHECK(ContainsOpcode(instructions, kOpName));
	CHECK_FALSE(ContainsOpcode(instructions, kOpString));
	CHECK_FALSE(ContainsOpcode(instructions, kOpSource));
	CHECK_FALSE(ContainsOpcode(instructions, kOpLine));
	CHECK_FALSE(ContainsOpcode(instructions, kOpExtInst));

	// stripped debug-info result (10) must go too or it dangles (spirv-val:
	bool sawNameOnRealId = false;
	bool sawNameOnStrippedId = false;
	for (const Instruction& instr: instructions)
	{
		if (instr.opcode == kOpName)
		{
			sawNameOnRealId |= instr.operands.at(0) == 4u;
			sawNameOnStrippedId |= instr.operands.at(0) == 10u;
		}
	}
	CHECK(sawNameOnRealId);
	CHECK_FALSE(sawNameOnStrippedId);

	bool sawDebugInfoImport = false;
	bool sawGlslImport = false;
	for (const Instruction& instr: instructions)
	{
		if (instr.opcode == kOpExtInstImport)
		{
			sawDebugInfoImport |= instr.operands.at(0) == 1u;
			sawGlslImport |= instr.operands.at(0) == 2u;
		}
	}
	CHECK_FALSE(sawDebugInfoImport);
	CHECK(sawGlslImport);
}

TEST_CASE("Strip keeps OpString when a surviving non-semantic set may reference it")
{
	const std::vector<uint32_t> module = BuildModule(/*withDebugPrintfImport=*/true);
	const auto stripped = assetpipeline::SpirvProcessor::Strip(AsBytes(module));
	REQUIRE(!stripped.empty());

	const auto instructions = Disassemble(stripped);
	CHECK(ContainsOpcode(instructions, kOpString));
	CHECK(ContainsOpcode(instructions, kOpName));

	bool sawPrintfImport = false;
	for (const Instruction& instr: instructions)
	{
		if (instr.opcode == kOpExtInstImport)
		{
			sawPrintfImport |= instr.operands.at(0) == 5u;
		}
	}
	CHECK(sawPrintfImport);
}

#pragma once

#include <cstddef>
#include <cstdint>

struct ProbeRecord
{
	std::uint32_t tag;
	std::uint64_t entity;
	const std::uint8_t* bytes;
	std::size_t length;
};

struct ProbeLayout
{
	std::uint32_t pointerSize;
	std::uint32_t recordSize;
	std::uint32_t recordAlign;
	std::uint32_t entityOffset;
	std::uint32_t bytesOffset;
	std::uint32_t lengthOffset;
};

extern "C"
{
	std::uint32_t ae_probe_add(std::uint32_t left, std::uint32_t right);
	std::uint32_t ae_probe_checksum(const std::uint8_t* bytes, std::size_t length);
	std::int32_t ae_probe_format(const std::uint8_t* bytes, std::size_t length, std::uint8_t* output, std::size_t capacity, std::size_t* required);
	std::uint32_t ae_probe_callback(std::uint32_t (*callback)(void*, std::uint32_t), void* context, std::uint32_t value);
	void ae_probe_layout(ProbeLayout* output);
	std::uint32_t ae_probe_record(const ProbeRecord* record);
}

#include "InteropProbe.hpp"

#include <array>
#include <cstdio>
#include <cstring>

static_assert(sizeof(ProbeLayout) == 24);
static_assert(offsetof(ProbeRecord, entity) == 8);
static_assert(offsetof(ProbeRecord, bytes) == 16);
static_assert(alignof(ProbeRecord) == 8);
static_assert(sizeof(ProbeRecord) == (sizeof(void*) == 4 ? 24 : 32));

namespace
{
	std::uint32_t Callback(void* context, std::uint32_t value)
	{
		auto* state = static_cast<std::uint32_t*>(context);
		*state += value;
		return *state;
	}
} // namespace

int main()
{
	unsigned failures = 0;
	unsigned checks = 0;
	auto check = [&](bool condition, const char* name)
	{
		++checks;
		if (!condition)
		{
			++failures;
			std::fprintf(stderr, "interop assertion failed: %s\n", name);
		}
	};
	check(ae_probe_add(20, 22) == 42, "integer");
	check(ae_probe_add(0xffffffffu, 1) == 0, "integer wraps without panic");
	// Explicit bytes make execution independent of compiler source-code encoding.
	constexpr std::uint8_t input[] = {'h', 0xc3, 0xa9, 0xf0, 0x9f, 0x8e, 0xae};
	std::uint32_t checksum = 2166136261u;
	for (auto byte: input)
	{
		checksum = (checksum ^ byte) * 16777619u;
	}
	check(ae_probe_checksum(input, sizeof(input)) == checksum, "exact UTF-8 bytes");
	check(ae_probe_checksum(nullptr, 0) == 2166136261u, "empty null input");
	std::size_t required = 0;
	check(ae_probe_format(input, sizeof(input), nullptr, 0, &required) == 1, "capacity query");
	std::array<std::uint8_t, 128> output;
	output.fill(0xab);
	const char expected[] = "Rust owns [h\xc3\xa9\xf0\x9f\x8e\xae] (7 bytes)";
	check(required == sizeof(expected), "required capacity includes terminator");
	check(ae_probe_format(input, sizeof(input), output.data(), required - 1, &required) == 1, "insufficient capacity");
	check(output[0] == 0xab && output.back() == 0xab, "short buffer untouched");
	check(ae_probe_format(input, sizeof(input), output.data(), required, &required) == 0, "owned string copy");
	check(std::memcmp(output.data(), expected, sizeof(expected)) == 0, "formatted UTF-8 bytes");
	check(output[sizeof(expected)] == 0xab, "no write after terminator");
	constexpr std::uint8_t invalid[] = {0xff};
	check(ae_probe_format(invalid, sizeof(invalid), output.data(), output.size(), &required) == 2 && required == 0, "invalid UTF-8 rejected");
	check(ae_probe_format(nullptr, 1, output.data(), output.size(), &required) == 3, "null nonempty input rejected");
	check(ae_probe_format(input, sizeof(input), output.data(), output.size(), nullptr) == 3, "null required rejected");
	check(ae_probe_format(input, sizeof(input), nullptr, 128, &required) == 3, "null destination rejected");
	check(ae_probe_format(nullptr, 0, output.data(), output.size(), &required) == 0, "empty formatted string");
	std::uint32_t state = 7;
	check(ae_probe_callback(Callback, &state, 35) == 42 && state == 42, "Rust calls C++ callback with context");
	check(ae_probe_callback(nullptr, nullptr, 0) == 0, "null callback");
	ProbeLayout layout{};
	ae_probe_layout(&layout);
	check(layout.pointerSize == sizeof(void*), "pointer width");
	check(layout.recordSize == sizeof(ProbeRecord), "record size");
	check(layout.recordAlign == alignof(ProbeRecord), "record alignment");
	check(layout.entityOffset == offsetof(ProbeRecord, entity), "entity offset");
	check(layout.bytesOffset == offsetof(ProbeRecord, bytes), "bytes offset");
	check(layout.lengthOffset == offsetof(ProbeRecord, length), "length offset");
	ProbeRecord record{0x12345678, 0x1122334455667788, input, sizeof(input)};
	check(ae_probe_record(&record) == checksum, "record field values and borrowed buffer");
	record.tag = 0;
	check(ae_probe_record(&record) == 0, "record value mismatch");
	check(ae_probe_record(nullptr) == 0, "null record");
	std::printf("{\"schema_version\":1,\"probe\":\"rust_cpp_interop\",\"status\":\"%s\",\"checks\":%u,\"failures\":%u,\"pointer_bytes\":%u,\"record_bytes\":%u,\"record_align\":%u,\"offsets\":{\"entity\":%u,\"bytes\":%u,\"length\":%u}}\n",
	        failures ? "failed" : "passed",
	        checks,
	        failures,
	        layout.pointerSize,
	        layout.recordSize,
	        layout.recordAlign,
	        layout.entityOffset,
	        layout.bytesOffset,
	        layout.lengthOffset);
	return failures ? 1 : 0;
}

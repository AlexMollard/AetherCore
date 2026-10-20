#include "probe.hpp"

#include <cstring>

int aether_probe_scalar(int value)
{
	return value * 2 + 1;
}

int aether_probe_utf8(const unsigned char* value, unsigned length)
{
	constexpr unsigned char kExpected[]{'c', 'a', 'f', 0xc3, 0xa9, 0, 'a', 'g', 'e', 'n', 't'};
	return value != nullptr && length == sizeof(kExpected) && std::memcmp(value, kExpected, sizeof(kExpected)) == 0;
}

void aether_probe_vec3(AetherGameplayVec3 value, AetherGameplayVec3* result)
{
	*result = {value.x + 1, value.y + 2, value.z + 3};
}

AetherGameplayVec3 aether_probe_return_vec3()
{
	return {3, 5, 8};
}

int aether_probe_ticks(int (*callback)(int), int ticks)
{
	int result = 0;
	for (int tick = 0; tick < ticks; ++tick)
	{
		result = callback(tick);
	}
	return result;
}

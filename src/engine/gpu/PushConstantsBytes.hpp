#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>

namespace aether::gpu
{
	// The smallest maxPushConstantsSize the Vulkan spec guarantees on any conformant device,
	// and the exact figure RDNA2-class AMD hardware reports. The descriptor-heap binding path
	// is bounded by maxPushDataSize instead (256 on current NVIDIA), so a push block that
	// overflows this floor works on the development machine and fails to create a shader on
	// the hardware that actually needed the fallback. Assert every push block against it.
	inline constexpr std::size_t kMaxGuaranteedPushConstantSize = 128;

	// caller's storage; the caller must keep the source object alive for
	template<typename T>
	[[nodiscard]] std::span<const std::byte> AsPushConstantBytes(const T& value) noexcept
	{
		static_assert(std::is_trivially_copyable_v<T>, "push-constant payload must be trivially copyable");
		return {reinterpret_cast<const std::byte*>(&value), sizeof(T)};
	}

	template<typename T>
	[[nodiscard]] std::span<std::byte> AsPushConstantBytes(T& value) noexcept
	{
		static_assert(std::is_trivially_copyable_v<T>, "push-constant payload must be trivially copyable");
		return {reinterpret_cast<std::byte*>(&value), sizeof(T)};
	}

	template<typename T>
	[[nodiscard]] std::span<const std::byte> ToPushConstantBytes(const T& value, std::byte (&buffer)[sizeof(T)]) noexcept
	{
		static_assert(std::is_trivially_copyable_v<T>, "push-constant payload must be trivially copyable");
		std::memcpy(buffer, &value, sizeof(T));
		return std::span<const std::byte>(buffer, sizeof(T));
	}
} // namespace aether::gpu

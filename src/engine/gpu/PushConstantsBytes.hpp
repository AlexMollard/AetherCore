#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>

namespace aether::gpu
{
	// Reinterpret a trivially-copyable value as a byte span suitable for
	// CommandList::PushConstantsRaw. The returned span references the
	// caller's storage; the caller must keep the source object alive for
	// the duration of the record-and-submit call (matches the existing
	// SkyboxPass / PostProcessStack / RenderQueue pattern).
	//
	// Const overload: read-only push constants.
	template<typename T>
	[[nodiscard]] std::span<const std::byte> AsPushConstantBytes(const T& value) noexcept
	{
		static_assert(std::is_trivially_copyable_v<T>, "push-constant payload must be trivially copyable");
		return {reinterpret_cast<const std::byte*>(&value), sizeof(T)};
	}

	// Mutable overload: for the rare case where the caller wants to fill
	// a buffer in place and then push it (provided for symmetry).
	template<typename T>
	[[nodiscard]] std::span<std::byte> AsPushConstantBytes(T& value) noexcept
	{
		static_assert(std::is_trivially_copyable_v<T>, "push-constant payload must be trivially copyable");
		return {reinterpret_cast<std::byte*>(&value), sizeof(T)};
	}

	// Build a byte span from a small stack buffer (for cases where the
	// caller wants to fill a fixed-size payload with a memcpy before
	// pushing - e.g. when the struct contains a Vulkan-only field that
	// the helper above cannot see through).
	//
	// Usage:
	//   DrawContracts::PushConstants pc{...};
	//   cmd.PushConstantsRaw(layout, stage, 0, gpu::AsPushConstantBytes(pc));
	template<typename T>
	[[nodiscard]] std::span<const std::byte> ToPushConstantBytes(const T& value, std::byte (&buffer)[sizeof(T)]) noexcept
	{
		static_assert(std::is_trivially_copyable_v<T>, "push-constant payload must be trivially copyable");
		std::memcpy(buffer, &value, sizeof(T));
		return std::span<const std::byte>(buffer, sizeof(T));
	}
} // namespace aether::gpu

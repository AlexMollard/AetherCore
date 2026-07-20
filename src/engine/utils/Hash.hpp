#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

// Canonical hashing primitives. Prior to this header the FNV-1a hash was
// re-implemented in five places (AssetTypes, MaterialRegistry, TextureRegistry,
// MaterialTemplate) and the boost-style combine in two more (Physics2DSystem,
// TileMapSystem), each with copy-pasted magic constants. Keep new hashing here
// so asset/material/texture identity stays byte-for-byte consistent.
namespace aether::utils
{
	inline constexpr std::uint64_t kFnv1aOffsetBasis = 1469598103934665603ull;
	inline constexpr std::uint64_t kFnv1aPrime = 1099511628211ull;

	// Incremental FNV-1a builder for hashing several fields into one value.
	struct Fnv1aHasher
	{
		std::uint64_t state = kFnv1aOffsetBasis;

		void Mix(const void* data, std::size_t n) noexcept
		{
			const auto* bytes = static_cast<const unsigned char*>(data);
			for (std::size_t i = 0; i < n; ++i)
			{
				state ^= bytes[i];
				state *= kFnv1aPrime;
			}
		}

		void Mix(std::string_view s) noexcept
		{
			Mix(s.data(), s.size());
		}

		template <typename T>
		void MixValue(const T& value) noexcept
		{
			static_assert(std::is_trivially_copyable_v<T>, "MixValue requires a trivially copyable type");
			Mix(&value, sizeof(T));
		}

		[[nodiscard]] std::uint64_t Value() const noexcept
		{
			return state;
		}
	};

	// One-shot FNV-1a over a byte range.
	[[nodiscard]] inline std::uint64_t Fnv1a(const void* data, std::size_t n, std::uint64_t seed = kFnv1aOffsetBasis) noexcept
	{
		Fnv1aHasher h{seed};
		h.Mix(data, n);
		return h.Value();
	}

	// One-shot FNV-1a over a string.
	[[nodiscard]] inline std::uint64_t Fnv1a(std::string_view s, std::uint64_t seed = kFnv1aOffsetBasis) noexcept
	{
		return Fnv1a(s.data(), s.size(), seed);
	}

	// Boost-style hash combine (64-bit golden ratio). Folds `value` into `seed`.
	[[nodiscard]] inline std::size_t HashCombine(std::size_t seed, std::size_t value) noexcept
	{
		return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u));
	}
} // namespace aether::utils

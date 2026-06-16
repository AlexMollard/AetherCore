#pragma once

#include <expected>
#include <format>
#include <string>
#include <string_view>

#include "utils/AetherExceptions.hpp"
#include "utils/LogCategory.hpp"

namespace aether
{
	struct AetherError
	{
		LogCategory category = LogCategory::Unknown;
		std::string message;
		int32_t code = 0;

		[[nodiscard]] std::string ToString() const
		{
			switch (category)
			{
				case LogCategory::Engine:
					return FormatError("Engine", code, message);
				case LogCategory::Window:
					return FormatError("Window", code, message);
				case LogCategory::Vulkan:
					return FormatError("Vulkan", code, message);
				case LogCategory::Animation:
					return FormatError("Animation", code, message);
				case LogCategory::Validation:
					return FormatError("Validation", code, message);
				case LogCategory::Asset:
					return FormatError("Asset", code, message);
				case LogCategory::FileSystem:
					return FormatError("FileSystem", code, message);
				case LogCategory::App:
					return FormatError("Application", code, message);
				case LogCategory::Std:
					return FormatError("Standard", code, message);
				case LogCategory::Render:
					return FormatError("Render", code, message);
				case LogCategory::Scene:
					return FormatError("Scene", code, message);
				case LogCategory::Camera:
					return FormatError("Camera", code, message);
				case LogCategory::UI:
					return FormatError("UI", code, message);
				case LogCategory::Input:
					return FormatError("Input", code, message);
				case LogCategory::Unknown:
					return FormatError("Unknown", code, message);
				default:
					return FormatError("Unknown", code, message);
			}
		}

		static AetherError Vulkan(int32_t result, std::string_view msg)
		{
			return {.category = LogCategory::Vulkan, .message = std::string(msg), .code = result};
		}

		static AetherError Asset(std::string_view msg)
		{
			return {.category = LogCategory::Asset, .message = std::string(msg)};
		}

		static AetherError FileSystem(std::string_view msg)
		{
			return {.category = LogCategory::FileSystem, .message = std::string(msg)};
		}

		static AetherError Window(std::string_view msg)
		{
			return {.category = LogCategory::Window, .message = std::string(msg)};
		}

		static AetherError Engine(std::string_view msg)
		{
			return {.category = LogCategory::Engine, .message = std::string(msg)};
		}

	private:
		static std::string FormatError(std::string_view categoryName, int32_t errCode, std::string_view msg)
		{
			if (errCode != 0)
			{
				return std::format("{} error (code={}): {}", categoryName, errCode, msg);
			}
			return std::format("{} error: {}", categoryName, msg);
		}
	};

	template<typename T>
	using Expected = std::expected<T, AetherError>;

	using Unexpected = std::unexpected<AetherError>;

	[[noreturn]] inline void Throw(const AetherError& err)
	{
		switch (err.category)
		{
			case LogCategory::Vulkan:
				throw VulkanError(err.message);
			case LogCategory::Asset:
				throw AssetError(err.message);
			case LogCategory::FileSystem:
				throw FileSystemError(err.message);
			case LogCategory::Window:
				throw WindowError(err.message);
			case LogCategory::Engine:
			case LogCategory::Animation:
			case LogCategory::Validation:
			case LogCategory::App:
			case LogCategory::Std:
			case LogCategory::Render:
			case LogCategory::Scene:
			case LogCategory::Camera:
			case LogCategory::UI:
			case LogCategory::Input:
			case LogCategory::Unknown:
			default:
				throw EngineError(err.message);
		}
	}
} // namespace aether

#define AE_TRY(var, expr) \
	auto (var) = (expr); \
	if (!(var).has_value()) \
		return std::unexpected(std::move((var).error()))

// Assigns expr to var. On failure, calls Throw() (fatal). On success, var holds the unwrapped value.
// Use at init-time call sites where failure is unrecoverable.
#define AE_EXPECT_OR_THROW(var, expr) \
	auto var##_expected = (expr); \
	if (!var##_expected.has_value()) \
		Throw(var##_expected.error()); \
	auto (var) = std::move(*var##_expected)

// Same as AE_EXPECT_OR_THROW but for functions returning Expected<void>.
#define AE_EXPECT_OR_THROW_VOID(expr) \
	{ \
		auto ae_result = (expr); \
		if (!ae_result.has_value()) \
			Throw(ae_result.error()); \
	}

#define AE_UNEXPECTED(err) return std::unexpected(err)

template<>
struct std::formatter<aether::AetherError> : std::formatter<std::string>
{
	auto format(const aether::AetherError& err, std::format_context& ctx) const
	{
		return std::formatter<std::string>::format(err.ToString(), ctx);
	}
};

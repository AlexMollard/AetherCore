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
				case LogCategory::Unknown:
					return FormatError("Unknown", code, message);
			}
			return message;
		}

		static AetherError Vulkan(int32_t vkResult, std::string_view msg)
		{
			return { .category = LogCategory::Vulkan, .message = std::string(msg), .code = vkResult };
		}

		static AetherError Asset(std::string_view msg)
		{
			return { .category = LogCategory::Asset, .message = std::string(msg) };
		}

		static AetherError FileSystem(std::string_view msg)
		{
			return { .category = LogCategory::FileSystem, .message = std::string(msg) };
		}

		static AetherError Window(std::string_view msg)
		{
			return { .category = LogCategory::Window, .message = std::string(msg) };
		}

		static AetherError Engine(std::string_view msg)
		{
			return { .category = LogCategory::Engine, .message = std::string(msg) };
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
			default:
				throw EngineError(err.message);
		}
	}
} // namespace aether

#define AE_TRY(var, expr) \
	auto var = (expr); \
	if (!var.has_value()) \
		return std::unexpected(std::move(var.error()))

// Assigns expr to var. On failure, calls Throw() (fatal). On success, var holds the value.
// Use at init-time call sites where failure is unrecoverable.
#define AE_EXPECT_OR_THROW(var, expr) \
	auto var = (expr); \
	if (!var.has_value()) \
		Throw(var.error())

template<>
struct std::formatter<aether::AetherError> : std::formatter<std::string>
{
	auto format(const aether::AetherError& err, std::format_context& ctx) const
	{
		return std::formatter<std::string>::format(err.ToString(), ctx);
	}
};

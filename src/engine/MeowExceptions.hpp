#pragma once

#include <stdexcept>
#include <string>

#include "LogCategory.hpp"

namespace meow
{
	class EngineError : public std::runtime_error
	{
	public:
		explicit EngineError(const std::string& message)
			: std::runtime_error(message)
		{
		}

		explicit EngineError(const char* message)
			: std::runtime_error(message)
		{
		}

		[[nodiscard]] virtual LogCategory Category() const noexcept
		{
			return LogCategory::Engine;
		}
	};

	class WindowError : public EngineError
	{
	public:
		explicit WindowError(const std::string& message)
			: EngineError(message)
		{
		}

		explicit WindowError(const char* message)
			: EngineError(message)
		{
		}

		[[nodiscard]] LogCategory Category() const noexcept override
		{
			return LogCategory::Window;
		}
	};

	class VulkanError : public EngineError
	{
	public:
		explicit VulkanError(const std::string& message)
			: EngineError(message)
		{
		}

		explicit VulkanError(const char* message)
			: EngineError(message)
		{
		}

		[[nodiscard]] LogCategory Category() const noexcept override
		{
			return LogCategory::Vulkan;
		}
	};

	class AssetError : public EngineError
	{
	public:
		explicit AssetError(const std::string& message)
			: EngineError(message)
		{
		}

		explicit AssetError(const char* message)
			: EngineError(message)
		{
		}

		[[nodiscard]] LogCategory Category() const noexcept override
		{
			return LogCategory::Asset;
		}
	};

	class FileSystemError : public EngineError
	{
	public:
		explicit FileSystemError(const std::string& message)
			: EngineError(message)
		{
		}

		explicit FileSystemError(const char* message)
			: EngineError(message)
		{
		}

		[[nodiscard]] LogCategory Category() const noexcept override
		{
			return LogCategory::FileSystem;
		}
	};
}

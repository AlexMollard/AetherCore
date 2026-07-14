#pragma once

#include <cstddef>
#include <utility>
#include <vector>
#include "utils/Expected.hpp"
#include "vulkan/volk.hpp"

namespace aether::vkutil
{
	class UniqueShaderModule
	{
	public:
		UniqueShaderModule() = default;

		UniqueShaderModule(VkDevice device, VkShaderModule module) noexcept
		      : m_device(device), m_module(module)
		{
		}

		~UniqueShaderModule()
		{
			Destroy();
		}

		UniqueShaderModule(const UniqueShaderModule&) = delete;
		UniqueShaderModule& operator=(const UniqueShaderModule&) = delete;

		UniqueShaderModule(UniqueShaderModule&& other) noexcept
		      : m_device(std::exchange(other.m_device, VK_NULL_HANDLE)), m_module(std::exchange(other.m_module, VK_NULL_HANDLE))
		{
		}

		UniqueShaderModule& operator=(UniqueShaderModule&& other) noexcept
		{
			if (this != &other)
			{
				Destroy();
				m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
				m_module = std::exchange(other.m_module, VK_NULL_HANDLE);
			}
			return *this;
		}

		[[nodiscard]] VkShaderModule Get() const noexcept
		{
			return m_module;
		}

		[[nodiscard]] operator VkShaderModule() const noexcept
		{
			return m_module;
		}

		[[nodiscard]] bool IsValid() const noexcept
		{
			return m_module != VK_NULL_HANDLE;
		}

		// Release ownership; caller becomes responsible for vkDestroyShaderModule.
		[[nodiscard]] VkShaderModule Release() noexcept
		{
			return std::exchange(m_module, VK_NULL_HANDLE);
		}

	private:
		VkDevice m_device = VK_NULL_HANDLE;
		VkShaderModule m_module = VK_NULL_HANDLE;

		void Destroy() noexcept
		{
			if (m_module != VK_NULL_HANDLE)
			{
				vkDestroyShaderModule(m_device, m_module, nullptr);
				m_module = VK_NULL_HANDLE;
			}
			m_device = VK_NULL_HANDLE;
		}
	};

	Expected<UniqueShaderModule> CreateShaderModule(VkDevice device, const std::vector<std::byte>& spirv, const char* owner);
} // namespace aether::vkutil

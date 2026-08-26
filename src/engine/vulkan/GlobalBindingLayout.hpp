#pragma once

#include <cstdint>

#include "vulkan/volk.hpp"

namespace aether::vulkan
{
	// The process-wide binding layout used when the device has no VK_EXT_descriptor_heap.
	//
	// On the heap path a shader is created layout-free and push data goes through
	// vkCmdPushDataEXT, so nothing here is needed and every handle stays null. Without the
	// extension the same SPIR-V has to be given a real VkDescriptorSetLayout and a real
	// VkPipelineLayout instead - and there is exactly one of each for the whole engine,
	// because every shader shares set 0 (g_textures[] at binding 0, g_linearSampler at
	// binding 1) and one push-constant range.
	//
	// BindlessManager owns these objects and publishes them here during Initialize; the
	// pipeline factories and CommandList::PushDataRaw read them. A singleton rather than
	// plumbing because a per-call parameter would have to thread through PipelineCache,
	// GraphicsPipeline and ResourceRegistry to reach values that are constant for the
	// process lifetime.
	struct GlobalBindingLayout
	{
		VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
		std::uint32_t pushConstantSize = 0;
	};

	// Published by BindlessManager::Initialize, cleared by its Shutdown. All handles are
	// VK_NULL_HANDLE while the descriptor-heap path is in use - callers branch on that.
	void SetGlobalBindingLayout(const GlobalBindingLayout& layout);
	[[nodiscard]] const GlobalBindingLayout& GetGlobalBindingLayout();
} // namespace aether::vulkan

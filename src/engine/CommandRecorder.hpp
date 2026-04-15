#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "DrawPushConstants.hpp"

namespace aether
{
	class GraphicsPipeline;

	class CommandRecorder
	{
	public:
		CommandRecorder() = default;
		explicit CommandRecorder(VkCommandBuffer cmd) : m_cmd(cmd) {}

		[[nodiscard]] bool IsValid() const { return m_cmd != VK_NULL_HANDLE; }
		[[nodiscard]] VkCommandBuffer GetCommandBuffer() const { return m_cmd; }

		void BindGraphicsPipeline(const GraphicsPipeline& pipeline);
		void Draw(
			std::uint32_t vertexCount,
			std::uint32_t instanceCount = 1,
			std::uint32_t firstVertex = 0,
			std::uint32_t firstInstance = 0);
		void DrawIndexed(
			std::uint32_t indexCount,
			std::uint32_t instanceCount = 1,
			std::uint32_t firstIndex = 0,
			std::int32_t  vertexOffset = 0,
			std::uint32_t firstInstance = 0);
		void BindVertexBuffer(VkBuffer buffer, VkDeviceSize offset = 0);
		void BindIndexBuffer(VkBuffer buffer, VkDeviceSize offset = 0, VkIndexType indexType = VK_INDEX_TYPE_UINT32);
		void BindDescriptorSet(VkPipelineLayout layout, std::uint32_t set, VkDescriptorSet descriptorSet);
		void PushConstants(VkPipelineLayout layout, const DrawPushConstants& pc);
		void BeginDebugLabel(const char* name, float r = 0.15f, float g = 0.55f, float b = 0.90f, float a = 1.0f);
		void EndDebugLabel();

		static void SetDebugLabelFunctions(
			PFN_vkCmdBeginDebugUtilsLabelEXT beginFn,
			PFN_vkCmdEndDebugUtilsLabelEXT endFn);

		// Name any Vulkan handle for RenderDoc / validation layers.
		// No-op if the extension was not loaded.
		static void SetObjectNameFunction(PFN_vkSetDebugUtilsObjectNameEXT fn);
		static void SetObjectName(VkDevice device, std::uint64_t handle, VkObjectType type, const char* name);
	private:
		static inline PFN_vkCmdBeginDebugUtilsLabelEXT s_beginDebugLabelFn = nullptr;
		static inline PFN_vkCmdEndDebugUtilsLabelEXT   s_endDebugLabelFn   = nullptr;
		static inline PFN_vkSetDebugUtilsObjectNameEXT s_setObjectNameFn   = nullptr;
		VkCommandBuffer m_cmd = VK_NULL_HANDLE;
	};
}

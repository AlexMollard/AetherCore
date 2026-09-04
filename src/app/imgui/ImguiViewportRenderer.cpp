#include "imgui/ImguiViewportRenderer.hpp"

#include "imgui/ImguiFrameData.hpp"
#include "utils/Logger.hpp"
#include "vulkan/VulkanContext.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include "vulkan/QueueSubmit.hpp"

// The stock ImGui_ImplVulkanH_CreateOrResizeWindow submits an "initial layout
void ImGui_ImplVulkanH_CreateWindowSwapChain(VkPhysicalDevice physical_device, VkDevice device, ImGui_ImplVulkanH_Window* wd, const VkAllocationCallbacks* allocator, int w, int h, uint32_t min_image_count, VkImageUsageFlags image_usage);
void ImGui_ImplVulkanH_CreateWindowCommandBuffers(VkPhysicalDevice physical_device, VkDevice device, ImGui_ImplVulkanH_Window* wd, uint32_t queue_family, const VkAllocationCallbacks* allocator);

namespace aether
{
	namespace
	{
		void CreateOrResizeViewportWindow(VulkanContext& vk, VkDevice device, ImGui_ImplVulkanH_Window& wd, int width, int height)
		{
			ImGui_ImplVulkanH_CreateWindowSwapChain(vk.GetPhysicalDevice(), device, &wd, nullptr, width, height, 2, 0);
			ImGui_ImplVulkanH_CreateWindowCommandBuffers(vk.GetPhysicalDevice(), device, &wd, vk.GetGraphicsQueueFamily(), nullptr);
		}

		/*
		#version 450 core
		layout(location = 0) in vec2 aPos;
		layout(location = 1) in vec2 aUV;
		layout(location = 2) in vec4 aColor;
		layout(push_constant) uniform uPushConstant { vec2 uScale; vec2 uTranslate; } pc;

		out gl_PerVertex { vec4 gl_Position; };
		layout(location = 0) out struct { vec4 Color; vec2 UV; } Out;

		void main()
		{
		    Out.Color = aColor;
		    Out.UV = aUV;
		    gl_Position = vec4(aPos * pc.uScale + pc.uTranslate, 0, 1);
		}
		*/
		static uint32_t _glsl_shader_vert_spv[] = {0x07230203,
		        0x00010000,
		        0x0008000b,
		        0x0000002e,
		        0x00000000,
		        0x00020011,
		        0x00000001,
		        0x0006000b,
		        0x00000001,
		        0x4c534c47,
		        0x6474732e,
		        0x3035342e,
		        0x00000000,
		        0x0003000e,
		        0x00000000,
		        0x00000001,
		        0x000a000f,
		        0x00000000,
		        0x00000004,
		        0x6e69616d,
		        0x00000000,
		        0x0000000b,
		        0x0000000f,
		        0x00000015,
		        0x0000001b,
		        0x0000001c,
		        0x00030003,
		        0x00000002,
		        0x000001c2,
		        0x00040005,
		        0x00000004,
		        0x6e69616d,
		        0x00000000,
		        0x00030005,
		        0x00000009,
		        0x00000000,
		        0x00050006,
		        0x00000009,
		        0x00000000,
		        0x6f6c6f43,
		        0x00000072,
		        0x00040006,
		        0x00000009,
		        0x00000001,
		        0x00005655,
		        0x00030005,
		        0x0000000b,
		        0x0074754f,
		        0x00040005,
		        0x0000000f,
		        0x6c6f4361,
		        0x0000726f,
		        0x00030005,
		        0x00000015,
		        0x00565561,
		        0x00060005,
		        0x00000019,
		        0x505f6c67,
		        0x65567265,
		        0x78657472,
		        0x00000000,
		        0x00060006,
		        0x00000019,
		        0x00000000,
		        0x505f6c67,
		        0x7469736f,
		        0x006e6f69,
		        0x00030005,
		        0x0000001b,
		        0x00000000,
		        0x00040005,
		        0x0000001c,
		        0x736f5061,
		        0x00000000,
		        0x00060005,
		        0x0000001e,
		        0x73755075,
		        0x6e6f4368,
		        0x6e617473,
		        0x00000074,
		        0x00050006,
		        0x0000001e,
		        0x00000000,
		        0x61635375,
		        0x0000656c,
		        0x00060006,
		        0x0000001e,
		        0x00000001,
		        0x61725475,
		        0x616c736e,
		        0x00006574,
		        0x00030005,
		        0x00000020,
		        0x00006370,
		        0x00040047,
		        0x0000000b,
		        0x0000001e,
		        0x00000000,
		        0x00040047,
		        0x0000000f,
		        0x0000001e,
		        0x00000002,
		        0x00040047,
		        0x00000015,
		        0x0000001e,
		        0x00000001,
		        0x00030047,
		        0x00000019,
		        0x00000002,
		        0x00050048,
		        0x00000019,
		        0x00000000,
		        0x0000000b,
		        0x00000000,
		        0x00040047,
		        0x0000001c,
		        0x0000001e,
		        0x00000000,
		        0x00030047,
		        0x0000001e,
		        0x00000002,
		        0x00050048,
		        0x0000001e,
		        0x00000000,
		        0x00000023,
		        0x00000000,
		        0x00050048,
		        0x0000001e,
		        0x00000001,
		        0x00000023,
		        0x00000008,
		        0x00020013,
		        0x00000002,
		        0x00030021,
		        0x00000003,
		        0x00000002,
		        0x00030016,
		        0x00000006,
		        0x00000020,
		        0x00040017,
		        0x00000007,
		        0x00000006,
		        0x00000004,
		        0x00040017,
		        0x00000008,
		        0x00000006,
		        0x00000002,
		        0x0004001e,
		        0x00000009,
		        0x00000007,
		        0x00000008,
		        0x00040020,
		        0x0000000a,
		        0x00000003,
		        0x00000009,
		        0x0004003b,
		        0x0000000a,
		        0x0000000b,
		        0x00000003,
		        0x00040015,
		        0x0000000c,
		        0x00000020,
		        0x00000001,
		        0x0004002b,
		        0x0000000c,
		        0x0000000d,
		        0x00000000,
		        0x00040020,
		        0x0000000e,
		        0x00000001,
		        0x00000007,
		        0x0004003b,
		        0x0000000e,
		        0x0000000f,
		        0x00000001,
		        0x00040020,
		        0x00000011,
		        0x00000003,
		        0x00000007,
		        0x0004002b,
		        0x0000000c,
		        0x00000013,
		        0x00000001,
		        0x00040020,
		        0x00000014,
		        0x00000001,
		        0x00000008,
		        0x0004003b,
		        0x00000014,
		        0x00000015,
		        0x00000001,
		        0x00040020,
		        0x00000017,
		        0x00000003,
		        0x00000008,
		        0x0003001e,
		        0x00000019,
		        0x00000007,
		        0x00040020,
		        0x0000001a,
		        0x00000003,
		        0x00000019,
		        0x0004003b,
		        0x0000001a,
		        0x0000001b,
		        0x00000003,
		        0x0004003b,
		        0x00000014,
		        0x0000001c,
		        0x00000001,
		        0x0004001e,
		        0x0000001e,
		        0x00000008,
		        0x00000008,
		        0x00040020,
		        0x0000001f,
		        0x00000009,
		        0x0000001e,
		        0x0004003b,
		        0x0000001f,
		        0x00000020,
		        0x00000009,
		        0x00040020,
		        0x00000021,
		        0x00000009,
		        0x00000008,
		        0x0004002b,
		        0x00000006,
		        0x00000028,
		        0x00000000,
		        0x0004002b,
		        0x00000006,
		        0x00000029,
		        0x3f800000,
		        0x00050036,
		        0x00000002,
		        0x00000004,
		        0x00000000,
		        0x00000003,
		        0x000200f8,
		        0x00000005,
		        0x0004003d,
		        0x00000007,
		        0x00000010,
		        0x0000000f,
		        0x00050041,
		        0x00000011,
		        0x00000012,
		        0x0000000b,
		        0x0000000d,
		        0x0003003e,
		        0x00000012,
		        0x00000010,
		        0x0004003d,
		        0x00000008,
		        0x00000016,
		        0x00000015,
		        0x00050041,
		        0x00000017,
		        0x00000018,
		        0x0000000b,
		        0x00000013,
		        0x0003003e,
		        0x00000018,
		        0x00000016,
		        0x0004003d,
		        0x00000008,
		        0x0000001d,
		        0x0000001c,
		        0x00050041,
		        0x00000021,
		        0x00000022,
		        0x00000020,
		        0x0000000d,
		        0x0004003d,
		        0x00000008,
		        0x00000023,
		        0x00000022,
		        0x00050085,
		        0x00000008,
		        0x00000024,
		        0x0000001d,
		        0x00000023,
		        0x00050041,
		        0x00000021,
		        0x00000025,
		        0x00000020,
		        0x00000013,
		        0x0004003d,
		        0x00000008,
		        0x00000026,
		        0x00000025,
		        0x00050081,
		        0x00000008,
		        0x00000027,
		        0x00000024,
		        0x00000026,
		        0x00050051,
		        0x00000006,
		        0x0000002a,
		        0x00000027,
		        0x00000000,
		        0x00050051,
		        0x00000006,
		        0x0000002b,
		        0x00000027,
		        0x00000001,
		        0x00070050,
		        0x00000007,
		        0x0000002c,
		        0x0000002a,
		        0x0000002b,
		        0x00000028,
		        0x00000029,
		        0x00050041,
		        0x00000011,
		        0x0000002d,
		        0x0000001b,
		        0x0000000d,
		        0x0003003e,
		        0x0000002d,
		        0x0000002c,
		        0x000100fd,
		        0x00010038};

		/*
		#version 450 core
		layout(location = 0) out vec4 fColor;
		layout(set=0, binding=0) uniform texture2D _Texture;
		layout(set=1, binding=0) uniform sampler _Sampler;
		layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
		void main()
		{
		    fColor = In.Color * texture(sampler2D(_Texture, _Sampler), In.UV.st);
		}
		*/
		static uint32_t _glsl_shader_frag_spv[] = {0x07230203,
		        0x00010000,
		        0x0008000b,
		        0x00000023,
		        0x00000000,
		        0x00020011,
		        0x00000001,
		        0x0006000b,
		        0x00000001,
		        0x4c534c47,
		        0x6474732e,
		        0x3035342e,
		        0x00000000,
		        0x0003000e,
		        0x00000000,
		        0x00000001,
		        0x0007000f,
		        0x00000004,
		        0x00000004,
		        0x6e69616d,
		        0x00000000,
		        0x00000009,
		        0x0000000d,
		        0x00030010,
		        0x00000004,
		        0x00000007,
		        0x00030003,
		        0x00000002,
		        0x000001c2,
		        0x00040005,
		        0x00000004,
		        0x6e69616d,
		        0x00000000,
		        0x00040005,
		        0x00000009,
		        0x6c6f4366,
		        0x0000726f,
		        0x00030005,
		        0x0000000b,
		        0x00000000,
		        0x00050006,
		        0x0000000b,
		        0x00000000,
		        0x6f6c6f43,
		        0x00000072,
		        0x00040006,
		        0x0000000b,
		        0x00000001,
		        0x00005655,
		        0x00030005,
		        0x0000000d,
		        0x00006e49,
		        0x00050005,
		        0x00000015,
		        0x7865545f,
		        0x65727574,
		        0x00000000,
		        0x00050005,
		        0x00000019,
		        0x6d61535f,
		        0x72656c70,
		        0x00000000,
		        0x00040047,
		        0x00000009,
		        0x0000001e,
		        0x00000000,
		        0x00040047,
		        0x0000000d,
		        0x0000001e,
		        0x00000000,
		        0x00040047,
		        0x00000015,
		        0x00000021,
		        0x00000000,
		        0x00040047,
		        0x00000015,
		        0x00000022,
		        0x00000000,
		        0x00040047,
		        0x00000019,
		        0x00000021,
		        0x00000000,
		        0x00040047,
		        0x00000019,
		        0x00000022,
		        0x00000001,
		        0x00020013,
		        0x00000002,
		        0x00030021,
		        0x00000003,
		        0x00000002,
		        0x00030016,
		        0x00000006,
		        0x00000020,
		        0x00040017,
		        0x00000007,
		        0x00000006,
		        0x00000004,
		        0x00040020,
		        0x00000008,
		        0x00000003,
		        0x00000007,
		        0x0004003b,
		        0x00000008,
		        0x00000009,
		        0x00000003,
		        0x00040017,
		        0x0000000a,
		        0x00000006,
		        0x00000002,
		        0x0004001e,
		        0x0000000b,
		        0x00000007,
		        0x0000000a,
		        0x00040020,
		        0x0000000c,
		        0x00000001,
		        0x0000000b,
		        0x0004003b,
		        0x0000000c,
		        0x0000000d,
		        0x00000001,
		        0x00040015,
		        0x0000000e,
		        0x00000020,
		        0x00000001,
		        0x0004002b,
		        0x0000000e,
		        0x0000000f,
		        0x00000000,
		        0x00040020,
		        0x00000010,
		        0x00000001,
		        0x00000007,
		        0x00090019,
		        0x00000013,
		        0x00000006,
		        0x00000001,
		        0x00000000,
		        0x00000000,
		        0x00000000,
		        0x00000001,
		        0x00000000,
		        0x00040020,
		        0x00000014,
		        0x00000000,
		        0x00000013,
		        0x0004003b,
		        0x00000014,
		        0x00000015,
		        0x00000000,
		        0x0002001a,
		        0x00000017,
		        0x00040020,
		        0x00000018,
		        0x00000000,
		        0x00000017,
		        0x0004003b,
		        0x00000018,
		        0x00000019,
		        0x00000000,
		        0x0003001b,
		        0x0000001b,
		        0x00000013,
		        0x0004002b,
		        0x0000000e,
		        0x0000001d,
		        0x00000001,
		        0x00040020,
		        0x0000001e,
		        0x00000001,
		        0x0000000a,
		        0x00050036,
		        0x00000002,
		        0x00000004,
		        0x00000000,
		        0x00000003,
		        0x000200f8,
		        0x00000005,
		        0x00050041,
		        0x00000010,
		        0x00000011,
		        0x0000000d,
		        0x0000000f,
		        0x0004003d,
		        0x00000007,
		        0x00000012,
		        0x00000011,
		        0x0004003d,
		        0x00000013,
		        0x00000016,
		        0x00000015,
		        0x0004003d,
		        0x00000017,
		        0x0000001a,
		        0x00000019,
		        0x00050056,
		        0x0000001b,
		        0x0000001c,
		        0x00000016,
		        0x0000001a,
		        0x00050041,
		        0x0000001e,
		        0x0000001f,
		        0x0000000d,
		        0x0000001d,
		        0x0004003d,
		        0x0000000a,
		        0x00000020,
		        0x0000001f,
		        0x00050057,
		        0x00000007,
		        0x00000021,
		        0x0000001c,
		        0x00000020,
		        0x00050085,
		        0x00000007,
		        0x00000022,
		        0x00000012,
		        0x00000021,
		        0x0003003e,
		        0x00000009,
		        0x00000022,
		        0x000100fd,
		        0x00010038};

		VkShaderModule MakeModule(VkDevice device, const uint32_t* code, std::size_t bytes)
		{
			VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			info.codeSize = bytes;
			info.pCode = code;
			VkShaderModule module = VK_NULL_HANDLE;
			vkCreateShaderModule(device, &info, nullptr, &module);
			return module;
		}

		VkDeviceSize AlignUp(VkDeviceSize size, VkDeviceSize alignment)
		{
			return (size + alignment - 1) & ~(alignment - 1);
		}

		std::uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, std::uint32_t typeBits, VkMemoryPropertyFlags properties)
		{
			VkPhysicalDeviceMemoryProperties memProperties;
			vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
			for (std::uint32_t i = 0; i < memProperties.memoryTypeCount; ++i)
			{
				if ((typeBits & (1u << i)) != 0 && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
				{
					return i;
				}
			}
			return UINT32_MAX;
		}
	} // namespace

	ImguiViewportRenderer::~ImguiViewportRenderer()
	{
		Shutdown();
	}

	void ImguiViewportRenderer::Init(VulkanContext& vk, VkFormat colorFormat)
	{
		m_vk = &vk;
		m_device = vk.GetDevice().device;
		m_colorFormat = colorFormat;
		CreatePipeline(colorFormat);
	}

	void ImguiViewportRenderer::CreatePipeline(VkFormat colorFormat)
	{
		VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.minLod = -1000.0f;
		samplerInfo.maxLod = 1000.0f;
		samplerInfo.maxAnisotropy = 1.0f;
		vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler);

		// descriptor sets are pipeline-layout-compatible with our pipeline.
		VkDescriptorSetLayoutBinding texBinding{};
		texBinding.binding = 0;
		texBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		texBinding.descriptorCount = 1;
		texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorSetLayoutCreateInfo texDsl{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		texDsl.bindingCount = 1;
		texDsl.pBindings = &texBinding;
		vkCreateDescriptorSetLayout(m_device, &texDsl, nullptr, &m_texSetLayout);

		VkDescriptorSetLayoutBinding sampBinding{};
		sampBinding.binding = 0;
		sampBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		sampBinding.descriptorCount = 1;
		sampBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorSetLayoutCreateInfo sampDsl{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		sampDsl.bindingCount = 1;
		sampDsl.pBindings = &sampBinding;
		vkCreateDescriptorSetLayout(m_device, &sampDsl, nullptr, &m_samplerSetLayout);

		const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1};
		VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		poolInfo.maxSets = 1;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes = &poolSize;
		vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_samplerPool);

		VkDescriptorSetAllocateInfo samplerAlloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		samplerAlloc.descriptorPool = m_samplerPool;
		samplerAlloc.descriptorSetCount = 1;
		samplerAlloc.pSetLayouts = &m_samplerSetLayout;
		vkAllocateDescriptorSets(m_device, &samplerAlloc, &m_samplerDS);

		VkDescriptorImageInfo samplerImage{};
		samplerImage.sampler = m_sampler;
		VkWriteDescriptorSet samplerWrite{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		samplerWrite.dstSet = m_samplerDS;
		samplerWrite.dstBinding = 0;
		samplerWrite.descriptorCount = 1;
		samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		samplerWrite.pImageInfo = &samplerImage;
		vkUpdateDescriptorSets(m_device, 1, &samplerWrite, 0, nullptr);

		const VkDescriptorSetLayout setLayouts[2] = {m_texSetLayout, m_samplerSetLayout};
		const VkPushConstantRange pcRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 4};
		VkPipelineLayoutCreateInfo plci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 2;
		plci.pSetLayouts = setLayouts;
		plci.pushConstantRangeCount = 1;
		plci.pPushConstantRanges = &pcRange;
		vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipelineLayout);

		VkShaderModule vert = MakeModule(m_device, _glsl_shader_vert_spv, sizeof(_glsl_shader_vert_spv));
		VkShaderModule frag = MakeModule(m_device, _glsl_shader_frag_spv, sizeof(_glsl_shader_frag_spv));

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vert;
		stages[0].pName = "main";
		stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = frag;
		stages[1].pName = "main";

		const VkVertexInputBindingDescription vbind{0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
		VkVertexInputAttributeDescription vattr[3]{};
		vattr[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos)};
		vattr[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv)};
		vattr[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col)};
		VkPipelineVertexInputStateCreateInfo vin{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vin.vertexBindingDescriptionCount = 1;
		vin.pVertexBindingDescriptions = &vbind;
		vin.vertexAttributeDescriptionCount = 3;
		vin.pVertexAttributeDescriptions = vattr;

		VkPipelineInputAssemblyStateCreateInfo ia{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState blend{};
		blend.blendEnable = VK_TRUE;
		blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.colorBlendOp = VK_BLEND_OP_ADD;
		blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.alphaBlendOp = VK_BLEND_OP_ADD;
		blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo cb{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &blend;
		VkDynamicState dyn[2]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dynInfo.dynamicStateCount = 2;
		dynInfo.pDynamicStates = dyn;

		VkPipelineRenderingCreateInfoKHR rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachmentFormats = &colorFormat;

		VkGraphicsPipelineCreateInfo pci{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		pci.pNext = &rendering;
		pci.stageCount = 2;
		pci.pStages = stages;
		pci.pVertexInputState = &vin;
		pci.pInputAssemblyState = &ia;
		pci.pViewportState = &vp;
		pci.pRasterizationState = &rs;
		pci.pMultisampleState = &ms;
		pci.pColorBlendState = &cb;
		pci.pDynamicState = &dynInfo;
		pci.layout = m_pipelineLayout;
		const VkResult pipelineResult = vkCreateGraphicsPipelines(m_device, m_vk->GetPipelineCache(), 1, &pci, nullptr, &m_pipeline);
		if (pipelineResult != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::UI, "ImGui viewport pipeline creation failed ({}).", static_cast<int>(pipelineResult));
		}

		vkDestroyShaderModule(m_device, vert, nullptr);
		vkDestroyShaderModule(m_device, frag, nullptr);
	}

	void ImguiViewportRenderer::Shutdown()
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}
		for (auto& [id, vp]: m_viewports)
		{
			DestroyOne(vp);
		}
		m_viewports.clear();
		if (m_pipeline)
		{
			vkDestroyPipeline(m_device, m_pipeline, nullptr);
		}
		if (m_pipelineLayout)
		{
			vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
		}
		if (m_samplerPool)
		{
			vkDestroyDescriptorPool(m_device, m_samplerPool, nullptr);
		}
		if (m_texSetLayout)
		{
			vkDestroyDescriptorSetLayout(m_device, m_texSetLayout, nullptr);
		}
		if (m_samplerSetLayout)
		{
			vkDestroyDescriptorSetLayout(m_device, m_samplerSetLayout, nullptr);
		}
		if (m_sampler)
		{
			vkDestroySampler(m_device, m_sampler, nullptr);
		}
		m_pipeline = VK_NULL_HANDLE;
		m_pipelineLayout = VK_NULL_HANDLE;
		m_samplerPool = VK_NULL_HANDLE;
		m_samplerDS = VK_NULL_HANDLE;
		m_texSetLayout = VK_NULL_HANDLE;
		m_samplerSetLayout = VK_NULL_HANDLE;
		m_sampler = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
	}

	void ImguiViewportRenderer::CreateOrResizeBuffer(VkBuffer& buffer, VkDeviceMemory& memory, VkDeviceSize& size, VkDeviceSize newSize, VkBufferUsageFlags usage) const
	{
		if (buffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(m_device, buffer, nullptr);
		}
		if (memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(m_device, memory, nullptr);
		}

		VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bufferInfo.size = newSize;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer);

		VkMemoryRequirements req{};
		vkGetBufferMemoryRequirements(m_device, buffer, &req);
		VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = FindMemoryType(m_vk->GetPhysicalDevice(), req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		vkAllocateMemory(m_device, &alloc, nullptr, &memory);
		vkBindBufferMemory(m_device, buffer, memory, 0);
		size = newSize;
	}

	void ImguiViewportRenderer::Render(const ImguiFrameData& frame)
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}
		for (const auto& captured: frame.SecondaryViewports())
		{
			if (captured.platformHandle == nullptr || captured.draw.CmdListsCount <= 0)
			{
				continue;
			}
			const int width = static_cast<int>(captured.size.x * captured.fbScale.x);
			const int height = static_cast<int>(captured.size.y * captured.fbScale.y);
			if (width <= 0 || height <= 0)
			{
				continue;
			}
			PerViewport& vp = m_viewports[captured.id];
			EnsureWindow(vp, captured.platformHandle, width, height);
			if (vp.created)
			{
				RenderOne(vp, captured.draw);
			}
		}
	}

	void ImguiViewportRenderer::RenderOne(PerViewport& vp, const ImDrawData& draw)
	{
		ImGui_ImplVulkanH_Window& wd = vp.window;
		const VkQueue queue = m_vk->GetGraphicsQueue();

		const int fbWidth = static_cast<int>(draw.DisplaySize.x * draw.FramebufferScale.x);
		const int fbHeight = static_cast<int>(draw.DisplaySize.y * draw.FramebufferScale.y);
		if (fbWidth <= 0 || fbHeight <= 0)
		{
			return;
		}

		const ImGui_ImplVulkanH_FrameSemaphores& fsd = wd.FrameSemaphores[static_cast<int>(wd.SemaphoreIndex)];
		std::uint32_t imageIndex = 0;
		const VkResult acquire = vkAcquireNextImageKHR(m_device, wd.Swapchain, UINT64_MAX, fsd.ImageAcquiredSemaphore, VK_NULL_HANDLE, &imageIndex);
		if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
		{
			// the semaphore, so bailing would leave an acquired-never-presented image
			// Idle before the helper tears the old swapchain and its frame objects
			// down: the previous frame may still be executing against them (never
			// free GPU resources the GPU may still be using).
			vkDeviceWaitIdle(m_device);
			CreateOrResizeViewportWindow(*m_vk, m_device, wd, wd.Width, wd.Height);
			return;
		}
		if (acquire == VK_ERROR_SURFACE_LOST_KHR)
		{
			// Recreating the swapchain cannot fix a dead surface - creation would
			// fail on it again. Tear the whole viewport window down (after an idle:
			// DestroyOne frees the vertex/index buffers and swapchain the last
			// submitted frame may still be walking) so the next frame's EnsureWindow
			// builds a fresh surface from the still-alive GLFW window.
			AE_WARN(LogCategory::UI, "ImGui viewport surface lost; rebuilding the viewport surface.");
			vkDeviceWaitIdle(m_device);
			DestroyOne(vp);
			return;
		}
		if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
		{
			// Any other failure (DEVICE_LOST, TIMEOUT, driver-specific): no image
			// was acquired, the acquire semaphore is unsignaled, and this frame's
			// fence was never submitted - falling through would wait on it forever
			// (vkWaitForFences below) or submit against an image that was never
			// acquired. Skip the frame without touching wd.FrameIndex or the fences.
			AE_WARN(LogCategory::UI, "ImGui viewport image acquire failed ({}); skipping frame.", static_cast<int>(acquire));
			return;
		}
		wd.FrameIndex = imageIndex;
		const ImGui_ImplVulkanH_Frame& fd = wd.Frames[static_cast<int>(imageIndex)];

		vkWaitForFences(m_device, 1, &fd.Fence, VK_TRUE, UINT64_MAX);
		vkResetFences(m_device, 1, &fd.Fence);
		vkResetCommandPool(m_device, fd.CommandPool, 0);
		VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(fd.CommandBuffer, &begin);

		// Transition the backbuffer to a color-attachment layout for drawing.
		VkImageMemoryBarrier toColor{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toColor.image = fd.Backbuffer;
		toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		// srcStage must include the stage the submit waits on the acquire semaphore
		vkCmdPipelineBarrier(fd.CommandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toColor);

		VkRenderingAttachmentInfo colorAttachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		colorAttachment.imageView = fd.BackbufferView;
		colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
		VkRenderingInfo renderingInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
		renderingInfo.renderArea.extent = {static_cast<std::uint32_t>(wd.Width), static_cast<std::uint32_t>(wd.Height)};
		renderingInfo.layerCount = 1;
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachments = &colorAttachment;
		vkCmdBeginRendering(fd.CommandBuffer, &renderingInfo);

		if (vp.ring.size() < wd.ImageCount)
		{
			vp.ring.resize(wd.ImageCount);
		}
		FrameBuffers& rb = vp.ring[imageIndex];
		if (draw.TotalVtxCount > 0)
		{
			const VkDeviceSize vtxNeeded = AlignUp(static_cast<VkDeviceSize>(draw.TotalVtxCount) * sizeof(ImDrawVert), 256);
			const VkDeviceSize idxNeeded = AlignUp(static_cast<VkDeviceSize>(draw.TotalIdxCount) * sizeof(ImDrawIdx), 256);
			if (rb.vtx == VK_NULL_HANDLE || rb.vtxSize < vtxNeeded)
			{
				CreateOrResizeBuffer(rb.vtx, rb.vtxMem, rb.vtxSize, vtxNeeded, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
			}
			if (rb.idx == VK_NULL_HANDLE || rb.idxSize < idxNeeded)
			{
				CreateOrResizeBuffer(rb.idx, rb.idxMem, rb.idxSize, idxNeeded, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
			}

			ImDrawVert* vtxDst = nullptr;
			ImDrawIdx* idxDst = nullptr;
			vkMapMemory(m_device, rb.vtxMem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&vtxDst));
			vkMapMemory(m_device, rb.idxMem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&idxDst));
			for (const ImDrawList* list: draw.CmdLists)
			{
				std::memcpy(vtxDst, list->VtxBuffer.Data, static_cast<std::size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
				std::memcpy(idxDst, list->IdxBuffer.Data, static_cast<std::size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
				vtxDst += list->VtxBuffer.Size;
				idxDst += list->IdxBuffer.Size;
			}
			VkMappedMemoryRange ranges[2]{};
			ranges[0] = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, rb.vtxMem, 0, VK_WHOLE_SIZE};
			ranges[1] = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, rb.idxMem, 0, VK_WHOLE_SIZE};
			vkFlushMappedMemoryRanges(m_device, 2, ranges);
			vkUnmapMemory(m_device, rb.vtxMem);
			vkUnmapMemory(m_device, rb.idxMem);
		}

		vkCmdBindPipeline(fd.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
		if (draw.TotalVtxCount > 0)
		{
			const VkBuffer vtxBuffers[1] = {rb.vtx};
			const VkDeviceSize vtxOffsets[1] = {0};
			vkCmdBindVertexBuffers(fd.CommandBuffer, 0, 1, vtxBuffers, vtxOffsets);
			vkCmdBindIndexBuffer(fd.CommandBuffer, rb.idx, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
		}
		const VkViewport viewport{0.0f, 0.0f, static_cast<float>(fbWidth), static_cast<float>(fbHeight), 0.0f, 1.0f};
		vkCmdSetViewport(fd.CommandBuffer, 0, 1, &viewport);

		float pushConstants[4];
		pushConstants[0] = 2.0f / draw.DisplaySize.x;
		pushConstants[1] = 2.0f / draw.DisplaySize.y;
		pushConstants[2] = -1.0f - draw.DisplayPos.x * pushConstants[0];
		pushConstants[3] = -1.0f - draw.DisplayPos.y * pushConstants[1];
		vkCmdPushConstants(fd.CommandBuffer, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 4, pushConstants);
		vkCmdBindDescriptorSets(fd.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1, 1, &m_samplerDS, 0, nullptr);

		const ImVec2 clipOffset = draw.DisplayPos;
		const ImVec2 clipScale = draw.FramebufferScale;
		int globalVtxOffset = 0;
		int globalIdxOffset = 0;
		VkDescriptorSet lastTexture = VK_NULL_HANDLE;
		for (const ImDrawList* list: draw.CmdLists)
		{
			for (int cmdIndex = 0; cmdIndex < list->CmdBuffer.Size; ++cmdIndex)
			{
				const ImDrawCmd& cmd = list->CmdBuffer[cmdIndex];
				if (cmd.UserCallback != nullptr)
				{
					continue;
				}
				if (cmd.ElemCount == 0)
				{
					continue;
				}

				ImVec2 clipMin((cmd.ClipRect.x - clipOffset.x) * clipScale.x, (cmd.ClipRect.y - clipOffset.y) * clipScale.y);
				ImVec2 clipMax((cmd.ClipRect.z - clipOffset.x) * clipScale.x, (cmd.ClipRect.w - clipOffset.y) * clipScale.y);
				clipMin.x = std::max(clipMin.x, 0.0f);
				clipMin.y = std::max(clipMin.y, 0.0f);
				clipMax.x = std::min(clipMax.x, static_cast<float>(fbWidth));
				clipMax.y = std::min(clipMax.y, static_cast<float>(fbHeight));
				if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
				{
					continue;
				}

				VkRect2D scissor;
				scissor.offset.x = static_cast<std::int32_t>(clipMin.x);
				scissor.offset.y = static_cast<std::int32_t>(clipMin.y);
				scissor.extent.width = static_cast<std::uint32_t>(clipMax.x - clipMin.x);
				scissor.extent.height = static_cast<std::uint32_t>(clipMax.y - clipMin.y);
				vkCmdSetScissor(fd.CommandBuffer, 0, 1, &scissor);

				const VkDescriptorSet texture = reinterpret_cast<VkDescriptorSet>(static_cast<std::uintptr_t>(cmd.GetTexID()));
				if (texture != lastTexture)
				{
					vkCmdBindDescriptorSets(fd.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &texture, 0, nullptr);
					lastTexture = texture;
				}
				vkCmdDrawIndexed(fd.CommandBuffer, cmd.ElemCount, 1, cmd.IdxOffset + globalIdxOffset, static_cast<std::int32_t>(cmd.VtxOffset + globalVtxOffset), 0);
			}
			globalIdxOffset += list->IdxBuffer.Size;
			globalVtxOffset += list->VtxBuffer.Size;
		}

		vkCmdEndRendering(fd.CommandBuffer);

		// Transition back to present layout.
		VkImageMemoryBarrier toPresent{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresent.image = fd.Backbuffer;
		toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(fd.CommandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);

		vkEndCommandBuffer(fd.CommandBuffer);

		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &fsd.ImageAcquiredSemaphore;
		submit.pWaitDstStageMask = &waitStage;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &fd.CommandBuffer;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &fsd.RenderCompleteSemaphore;
		{
			const std::lock_guard<std::mutex> queueLock(aether::vulkan::QueueSubmitMutex());
			vkQueueSubmit(queue, 1, &submit, fd.Fence);
		}

		VkPresentInfoKHR present{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores = &fsd.RenderCompleteSemaphore;
		present.swapchainCount = 1;
		present.pSwapchains = &wd.Swapchain;
		present.pImageIndices = &imageIndex;
		VkResult presented = VK_SUCCESS;
		{
			const std::lock_guard<std::mutex> queueLock(aether::vulkan::QueueSubmitMutex());
			presented = vkQueuePresentKHR(queue, &present);
		}
		wd.SemaphoreIndex = (wd.SemaphoreIndex + 1) % wd.SemaphoreCount;
		if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
		{
			CreateOrResizeViewportWindow(*m_vk, m_device, wd, wd.Width, wd.Height);
		}
	}

	void ImguiViewportRenderer::EnsureWindow(PerViewport& vp, void* glfwWindow, int width, int height)
	{
		ImGui_ImplVulkanH_Window& wd = vp.window;
		if (!vp.created)
		{
			VkSurfaceKHR surface = VK_NULL_HANDLE;
			const VkResult err = glfwCreateWindowSurface(m_vk->GetInstance().instance, static_cast<GLFWwindow*>(glfwWindow), nullptr, &surface);
			if (err != VK_SUCCESS || surface == VK_NULL_HANDLE)
			{
				AE_WARN(LogCategory::UI, "ImGui viewport surface creation failed ({}).", static_cast<int>(err));
				return;
			}
			wd.Surface = surface;
			wd.UseDynamicRendering = true;
			const VkFormat requested[] = {m_colorFormat};
			wd.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(m_vk->GetPhysicalDevice(), surface, requested, 1, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
			const VkPresentModeKHR presentModes[] = {VK_PRESENT_MODE_FIFO_KHR};
			wd.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(m_vk->GetPhysicalDevice(), surface, presentModes, 1);
			CreateOrResizeViewportWindow(*m_vk, m_device, wd, width, height);
			vp.created = true;
			AE_INFO(LogCategory::UI, "ImGui secondary viewport swapchain created ({}x{}, {} images).", width, height, wd.ImageCount);
			return;
		}
		if (wd.Width != width || wd.Height != height)
		{
			CreateOrResizeViewportWindow(*m_vk, m_device, wd, width, height);
		}
	}

	void ImguiViewportRenderer::DestroyOne(PerViewport& vp)
	{
		for (const FrameBuffers& rb: vp.ring)
		{
			if (rb.vtx)
			{
				vkDestroyBuffer(m_device, rb.vtx, nullptr);
			}
			if (rb.vtxMem)
			{
				vkFreeMemory(m_device, rb.vtxMem, nullptr);
			}
			if (rb.idx)
			{
				vkDestroyBuffer(m_device, rb.idx, nullptr);
			}
			if (rb.idxMem)
			{
				vkFreeMemory(m_device, rb.idxMem, nullptr);
			}
		}
		vp.ring.clear();

		if (!vp.created)
		{
			return;
		}
		const VkSurfaceKHR surface = vp.window.Surface;
		ImGui_ImplVulkanH_DestroyWindow(m_vk->GetInstance().instance, m_device, &vp.window, nullptr);
		if (surface != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(m_vk->GetInstance().instance, surface, nullptr);
		}
		vp.created = false;
		vp.window = ImGui_ImplVulkanH_Window{};
	}

	void ImguiViewportRenderer::RetireViewports(const std::vector<ImGuiID>& departedIds)
	{
		for (const ImGuiID id: departedIds)
		{
			auto it = m_viewports.find(id);
			if (it != m_viewports.end())
			{
				DestroyOne(it->second);
				m_viewports.erase(it);
			}
		}
	}
} // namespace aether

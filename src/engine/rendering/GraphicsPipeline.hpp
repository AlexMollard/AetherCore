#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include "gpu/DescriptorSetLayout.hpp"
#include "gpu/GpuTypes.hpp"
#include "utils/Assert.hpp"

namespace aether
{
	class GraphicsPipeline
	{
	public:
		struct Desc
		{
			std::string_view shaderVfsPath;
			std::string_view vertexEntry = "vertexMain";
			std::string_view fragmentEntry = "fragmentMain";
			gpu::Format colorFormat = gpu::Format::Undefined;
			gpu::Format depthFormat = gpu::Format::Undefined;
			bool depthTestEnable = false;
			bool depthWriteEnable = false;
			gpu::CompareOp depthCompareOp = gpu::CompareOp::Less;
			bool blendEnable = false;
			// Override the default DrawPushConstants block. If size is 0 the
			// default model-matrix + BDA range is used instead.
			uint32_t pushConstantSize = 0;
			gpu::ShaderStage pushConstantStages = gpu::ShaderStage::AllGraphics;
			// Descriptor set layouts bound into the pipeline layout in order.
			std::span<const gpu::DescriptorSetLayout> setLayouts;
		};

		GraphicsPipeline() = default;
		~GraphicsPipeline();

		GraphicsPipeline(const GraphicsPipeline&) = AE_DELETE_MSG("GraphicsPipeline owns a VkPipeline - use std::move");
		GraphicsPipeline& operator=(const GraphicsPipeline&) = AE_DELETE_MSG("GraphicsPipeline owns a VkPipeline - use std::move");

		GraphicsPipeline(GraphicsPipeline&&) noexcept;
		GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept;

		static Expected<GraphicsPipeline> Create(gpu::Device device, gpu::PipelineCache pipelineCache, const Desc& desc);
		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_pipeline != nullptr;
		}

		[[nodiscard]] gpu::Pipeline GetPipeline() const
		{
			return m_pipeline;
		}

		[[nodiscard]] gpu::PipelineLayout GetLayout() const
		{
			return m_layout;
		}

		[[nodiscard]] std::uint32_t GetSetLayoutCount() const
		{
			return m_setLayoutCount;
		}

	private:
		void* m_device = nullptr;
		void* m_layout = nullptr;
		void* m_pipeline = nullptr;
		void* m_vertInputLib = nullptr;
		void* m_preRasterLib = nullptr;
		void* m_fragShaderLib = nullptr;
		void* m_fragOutputLib = nullptr;
		std::uint32_t m_setLayoutCount = 0;
	};
} // namespace aether

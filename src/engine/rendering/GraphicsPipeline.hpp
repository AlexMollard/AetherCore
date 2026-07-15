#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include "gpu/GpuHandles.hpp"
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
			std::string_view fragmentVfsPath;
			std::string_view vertexEntry = "vertexMain";
			std::string_view fragmentEntry = "fragmentMain";
			gpu::Format colorFormat = gpu::Format::Undefined;
			gpu::Format depthFormat = gpu::Format::Undefined;
			bool depthTestEnable = false;
			bool depthWriteEnable = false;
			gpu::CompareOp depthCompareOp = gpu::CompareOp::Less;
			bool blendEnable = false;
			gpu::BlendMode blendMode = gpu::BlendMode::Alpha;
			gpu::PrimitiveTopology topology = gpu::PrimitiveTopology::TriangleList;
			gpu::PolygonMode polygonMode = gpu::PolygonMode::Fill;
			gpu::CullMode cullMode = gpu::CullMode::None;
			std::span<const gpu::VertexInputBinding> vertexBindings;
			std::span<const gpu::VertexInputAttribute> vertexAttributes;
			bool lineWidthDynamic = false;
			const char* debugName = nullptr;
			const void* descriptorHeapMappings = nullptr;
		};

		GraphicsPipeline() = default;
		~GraphicsPipeline();

		GraphicsPipeline(const GraphicsPipeline&) = AE_DELETE_MSG("GraphicsPipeline holds an opaque handle - use std::move");
		GraphicsPipeline& operator=(const GraphicsPipeline&) = AE_DELETE_MSG("GraphicsPipeline holds an opaque handle - use std::move");

		GraphicsPipeline(GraphicsPipeline&&) noexcept;
		GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept;

		static Expected<GraphicsPipeline> Create(gpu::Device device, const Desc& desc);

		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_handle.IsValid();
		}

		[[nodiscard]] gpu::PipelineView GetPipeline() const;

		[[nodiscard]] gpu::PipelineHandle GetHandle() const
		{
			return m_handle;
		}

	private:
		gpu::PipelineHandle m_handle{};
	};
} // namespace aether

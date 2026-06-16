#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include "gpu/DescriptorSetLayout.hpp"
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
			// Optional separate fragment path. When non-empty, the fragment
			// SPIR-V is loaded from this path instead of sharing the vertex
			// module. Default (empty) keeps the existing single-module behavior.
			std::string_view fragmentVfsPath;
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
			// Graphics-pipeline state overrides for non-default pipelines.
			// Defaults match the standard MeshDraw path.
			gpu::PrimitiveTopology topology = gpu::PrimitiveTopology::TriangleList;
			gpu::PolygonMode polygonMode = gpu::PolygonMode::Fill;
			std::span<const gpu::VertexInputBinding> vertexBindings;
			std::span<const gpu::VertexInputAttribute> vertexAttributes;
			bool lineWidthDynamic = false;
		};

		GraphicsPipeline() = default;
		~GraphicsPipeline();

		GraphicsPipeline(const GraphicsPipeline&) = AE_DELETE_MSG("GraphicsPipeline holds an opaque handle - use std::move");
		GraphicsPipeline& operator=(const GraphicsPipeline&) = AE_DELETE_MSG("GraphicsPipeline holds an opaque handle - use std::move");

		GraphicsPipeline(GraphicsPipeline&&) noexcept;
		GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept;

		// Creates the pipeline + GPL libraries and registers them with the
		// gpu::ResourceRegistry. The returned handle owns the deferred-
		// destruction path (3-frame ring). On Shutdown / destruction of
		// this object the handle is released and the registry tears down
		// the linked pipeline + libraries + layout kMaxFramesInFlight
		// frames later.
		static Expected<GraphicsPipeline> Create(gpu::Device device, gpu::PipelineCache pipelineCache, const Desc& desc);

		// Explicit teardown that schedules the underlying pipelines for
		// deferred destruction immediately, then resets the handle so the
		// destructor is a no-op. Callers that want to release GPU memory
		// before the owning object goes out of scope can use this; the
		// destructor will still fire on scope exit and is also safe.
		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_handle.IsValid();
		}

		[[nodiscard]] gpu::Pipeline GetPipeline() const;
		[[nodiscard]] gpu::PipelineLayout GetLayout() const;

		[[nodiscard]] std::uint32_t GetSetLayoutCount() const
		{
			return m_setLayoutCount;
		}

	private:
		gpu::PipelineHandle m_handle{};
		std::uint32_t m_setLayoutCount = 0;
	};
} // namespace aether

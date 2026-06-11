#pragma once

#include <functional>
#include <span>

#include "gpu/CommandList.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/FrameContext.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether
{
	class RenderQueue;

	// Registers the engine's main forward scene pass.
	//
	// Draws the commands produced by the preceding CullPass compute pass using
	// DrawIndexedIndirectCount. Scene and world flushing happens in CullPass - this
	// pass only records draw calls and clears the queue.
	class ForwardPass
	{
	public:
		void SetEnabled(bool enabled)
		{
			m_enabled = enabled;
		}

		[[nodiscard]] bool IsEnabled() const
		{
			return m_enabled;
		}

		void RegisterPass(const FrameContext& frame,
		        RenderQueue& renderQueue,
		        RGImage hdrColor,
		        RGImage depth,
		        std::function<void(gpu::CommandList&, gpu::PipelineLayout)> pushLightingFn,
		        std::span<const RGImage> shadowMaps = {},
		        RGImage localShadowAtlas = {});

	private:
		bool m_enabled = true;
	};
} // namespace aether

#pragma once

#include <cstdint>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether
{
	class GTAOPass
	{
	public:
		struct Desc
		{
			gpu::Device device = nullptr;
			gpu::Extent2D extent;
			class BindlessManager* bindlessManager = nullptr;
			RenderGraph* renderGraph = nullptr;
		};

		GTAOPass() = default;
		GTAOPass(const GTAOPass&) = delete;
		GTAOPass& operator=(const GTAOPass&) = delete;
		GTAOPass(GTAOPass&&) noexcept = default;
		GTAOPass& operator=(GTAOPass&&) noexcept = default;

		void Create(const Desc& desc);
		void Destroy();

		void RegisterPasses(RenderGraph& graph, RGImage depth, std::uint32_t depthBindlessSlot);

		[[nodiscard]] std::uint32_t GetAoBindlessSlot() const
		{
			return m_denoisedAoBindlessSlot;
		}

		[[nodiscard]] gpu::Extent2D GetAoExtent() const
		{
			return m_aoExtent;
		}

		[[nodiscard]] RGImage GetAoImage() const
		{
			return m_denoisedAoImage;
		}

	private:
		gpu::Device m_device = nullptr;
		gpu::Extent2D m_extent{};
		gpu::Extent2D m_aoExtent{};
		BindlessManager* m_bindlessManager = nullptr;
		RenderGraph* m_renderGraph = nullptr;

		GraphicsPipeline m_mainPipeline;
		GraphicsPipeline m_denoisePipeline;

		gpu::TextureHandle m_rawAoHandle;
		gpu::TextureHandle m_denoisedAoHandle;
		RGImage m_rawAoImage{};
		RGImage m_denoisedAoImage{};
		std::uint32_t m_rawAoBindlessSlot = 0xFFFFFFFFu;
		std::uint32_t m_denoisedAoBindlessSlot = 0xFFFFFFFFu;

		bool m_passesRegistered = false;
	};
} // namespace aether

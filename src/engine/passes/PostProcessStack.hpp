#pragma once

#include <cstdint>
#include <span>

#include "gpu/BindlessManager.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether
{
	// Tonemap operator applied in the $PostProcess pass.
	enum class TonemapMode : uint32_t
	{
		Reinhard = 0,   // x / (x + 1) - simple, cheap
		AcesFilmic = 1, // Narkowicz 2015 fit - filmic toe + shoulder
		Uncharted2 = 2, // John Hable curve - warm filmic look
	};

	// Owns the offscreen images and pipelines for the engine's post-processing
	// chain: forward HDR buffer -> tonemap -> FXAA -> swapchain.
	//
	// Lifetime contract
	// -----------------
	//  1. Create()         - allocates GPU resources, registers images with the
	//  graph
	//  2. RegisterPasses() - adds $PostProcess and $FXAA passes to the graph;
	//                        must be called every time the graph is rebuilt
	//  3. Destroy()        - releases all GPU resources; call before or during
	//                        swapchain recreation
	//
	// The stack is non-copyable and move-only.  After a move it is in a valid
	// but empty state; calling Destroy() on an empty stack is a no-op.
	class PostProcessStack
	{
	public:
		struct Desc
		{
			gpu::Device device = nullptr;
			gpu::PipelineCache pipelineCache = nullptr;
			gpu::Extent2D extent;
			gpu::Format swapchainFormat = gpu::Format::Undefined;
			BindlessManager* bindlessManager = nullptr; // non-owning
			RenderGraph* renderGraph = nullptr;         // non-owning
		};

		PostProcessStack() = default;

		PostProcessStack(const PostProcessStack&) = delete;
		PostProcessStack& operator=(const PostProcessStack&) = delete;

		PostProcessStack(PostProcessStack&&) noexcept = default;
		PostProcessStack& operator=(PostProcessStack&&) noexcept = default;

		static PostProcessStack Create(const Desc& desc);
		void Destroy();

		// Format the engine's forward pass must use when writing the HDR buffer.
		// Game-layer pipelines that render scene geometry must match this format.
		[[nodiscard]] static constexpr gpu::Format GetForwardColorFormat()
		{
			return gpu::Format::R16G16B16A16Sfloat;
		}

		// RenderGraph handle for the HDR buffer - pass to the forward pass's
		// WriteColor() so the graph tracks the write->read dependency.
		[[nodiscard]] RGImage GetHdrColor() const
		{
			return m_hdrColor;
		}

		// Select the tonemap curve applied in $PostProcess (default: Reinhard).
		void SetTonemapMode(TonemapMode mode)
		{
			m_tonemapMode = mode;
		}

		[[nodiscard]] TonemapMode GetTonemapMode() const
		{
			return m_tonemapMode;
		}

		// Pre-tonemap exposure multiplier (default: 1.0 = no change).
		void SetExposure(float exposure)
		{
			m_exposure = exposure;
		}

		[[nodiscard]] float GetExposure() const
		{
			return m_exposure;
		}

		// FXAA toggle.  When disabled the $FXAA pass becomes a passthrough
		// (no blurring) so graph topology stays stable across toggles.
		void SetFxaaEnabled(bool enabled)
		{
			m_fxaaEnabled = enabled;
		}

		[[nodiscard]] bool IsFxaaEnabled() const
		{
			return m_fxaaEnabled;
		}

		// Adds the $PostProcess (tonemap) and $FXAA passes to the render graph.
		// bindless must outlive the graph (it is captured by the pass lambdas).
		void RegisterPasses(RenderGraph& graph, BindlessManager& bindless);

	private:
		gpu::TextureHandle m_hdrColorHandle; // R16G16B16A16_SFLOAT - forward output
		RGImage m_hdrColor{};
		std::uint32_t m_hdrBindlessSlot = 0xFFFFFFFFu;
		GraphicsPipeline m_tonemapPipeline; // HDR -> LDR

		gpu::TextureHandle m_ldrColorHandle; // R8G8B8A8_UNORM - tonemap output
		RGImage m_ldrColor{};
		std::uint32_t m_ldrBindlessSlot = 0xFFFFFFFFu;
		GraphicsPipeline m_fxaaPipeline; // LDR -> swapchain (FXAA, can passthrough when disabled)
		gpu::Format m_swapchainFormat = gpu::Format::Undefined;

		TonemapMode m_tonemapMode = TonemapMode::Reinhard;
		float m_exposure = 1.0f;
		bool m_fxaaEnabled = false;
	};
} // namespace aether

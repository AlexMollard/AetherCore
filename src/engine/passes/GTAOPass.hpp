#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>

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
		~GTAOPass() = default;
		GTAOPass(const GTAOPass&) = delete;
		GTAOPass& operator=(const GTAOPass&) = delete;
		GTAOPass(GTAOPass&&) noexcept = default;
		GTAOPass& operator=(GTAOPass&&) noexcept = default;

		void Create(const Desc& desc);
		void Destroy();

		// isActive is polled each frame; when it returns false (e.g. a 2D scene with no
		// 3D geometry) both AO passes skip their compute so the full-screen AO work is
		// not wasted. Null => always active.
		void RegisterPasses(RenderGraph& graph, RGImage depth, std::function<bool()> isActive = {});

		// Live-tunable. Radius is world units, so it tracks the scale of the scene's
		// geometry rather than a fixed screen footprint; strength scales the result.
		// Disabling leaves the passes registered and writes fully-open visibility, so the
		// graph shape does not change with a settings toggle.
		void SetEnabled(bool enabled)
		{
			m_enabled = enabled;
		}

		[[nodiscard]] bool IsEnabled() const
		{
			return m_enabled;
		}

		void SetRadius(float radius)
		{
			m_radius = std::clamp(radius, 0.1f, 5.0f);
		}

		[[nodiscard]] float GetRadius() const
		{
			return m_radius;
		}

		void SetStrength(float strength)
		{
			m_strength = std::clamp(strength, 0.0f, 3.0f);
		}

		[[nodiscard]] float GetStrength() const
		{
			return m_strength;
		}

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
		bool m_enabled = true;
		float m_radius = 1.4f;
		float m_strength = 1.35f;
		gpu::Device m_device = nullptr;
		gpu::Extent2D m_extent;
		gpu::Extent2D m_aoExtent;
		BindlessManager* m_bindlessManager = nullptr;
		RenderGraph* m_renderGraph = nullptr;

		GraphicsPipeline m_mainPipeline;
		GraphicsPipeline m_denoisePipeline;

		// Graph-owned: the pass declares them, the graph decides where they live.
		RGImage m_rawAoImage{};
		RGImage m_denoisedAoImage{};
		std::uint32_t m_rawAoBindlessSlot = 0xFFFFFFFFu;
		std::uint32_t m_denoisedAoBindlessSlot = 0xFFFFFFFFu;

		bool m_passesRegistered = false;
	};
} // namespace aether

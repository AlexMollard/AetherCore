#pragma once

#include <array>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "GraphicsPipeline.hpp"
#include "Swapchain.hpp"
#include "UiLayout.hpp"

namespace aether
{
	class AetherCore;

	class QuadRenderer
	{
	public:
		QuadRenderer() = default;
		~QuadRenderer() = default;

		QuadRenderer(const QuadRenderer&) = delete;
		QuadRenderer& operator=(const QuadRenderer&) = delete;

		void Init(AetherCore& engine, std::string_view passName);
		void Shutdown(AetherCore& engine);

		// Must be called by the game thread before DrawQuad() each frame.
		void SetWriteSlot(std::uint32_t slot)
		{
			m_writeSlot = slot;
		}

		void DrawQuad(const UiRect& rect, glm::vec4 color = glm::vec4(1.f));

		[[nodiscard]] bool IsReady() const
		{
			return m_ready;
		}

	private:
		struct QuadPush
		{
			glm::vec4 screenSize; // .xy = viewport pixels
			glm::vec4 rect;       // x, y, w, h in screen pixels (top-left origin)
			glm::vec4 color;      // RGBA tint (linear)
		};

		static_assert(sizeof(QuadPush) == 48, "QuadPush must match ui_quad.slang push constant block.");

		struct PendingQuad
		{
			glm::vec4 rect;
			glm::vec4 color;
		};

		void EnsurePassRegistered();
		void RegisterPass();

		std::string m_passName;
		AetherCore* m_engine = nullptr;
		GraphicsPipeline m_pipeline;
		// Double-buffered pending draw list. Game thread writes to m_writeSlot;
		// render thread reads from ctx.frameIndex % 2 (guaranteed to be different).
		std::array<std::vector<PendingQuad>, Swapchain::kMaxFramesInFlight> m_pendingQuads;
		std::uint32_t m_writeSlot = 0;
		bool m_ready = false;
	};
} // namespace aether

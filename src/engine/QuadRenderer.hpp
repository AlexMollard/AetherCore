#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "GraphicsPipeline.hpp"
#include "UiLayout.hpp"

namespace meow
{
	class MeowCore;

	class QuadRenderer
	{
	public:
		QuadRenderer() = default;
		~QuadRenderer() = default;

		QuadRenderer(const QuadRenderer&) = delete;
		QuadRenderer& operator=(const QuadRenderer&) = delete;

		void Init(MeowCore& engine, std::string_view passName);
		void Shutdown(MeowCore& engine);

		void DrawQuad(const UiRect& rect, glm::vec4 color = glm::vec4(1.f));

		[[nodiscard]] bool IsReady() const { return m_ready; }

	private:
		struct QuadPush
		{
			glm::vec4 screenSize; // .xy = viewport pixels
			glm::vec4 rect;       // x, y, w, h in screen pixels (top-left origin)
			glm::vec4 color;      // RGBA tint (linear)
		};
		static_assert(sizeof(QuadPush) == 48,
			"QuadPush must match ui_quad.slang push constant block.");

		struct PendingQuad
		{
			glm::vec4 rect;
			glm::vec4 color;
		};

		void EnsurePassRegistered();
		void RegisterPass();

		std::string              m_passName;
		MeowCore* m_engine = nullptr;
		GraphicsPipeline         m_pipeline;
		std::vector<PendingQuad> m_pendingQuads;
		bool                     m_ready = false;
	};
}

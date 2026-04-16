#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

#include "GraphicsPipeline.hpp"
#include "Swapchain.hpp"
#include "UiLayout.hpp"
#include "UniqueBuffer.hpp"

namespace aether
{
	class AetherCore;

	class QuadRenderer
	{
	public:
		enum class ShapeType : std::uint32_t
		{
			Rect = 0,
			Circle = 1,
			Line = 2,
		};

		QuadRenderer() = default;
		~QuadRenderer() = default;

		QuadRenderer(const QuadRenderer&) = delete;
		QuadRenderer& operator=(const QuadRenderer&) = delete;

		void Init(AetherCore& engine, std::string_view passName);
		void Shutdown(AetherCore& engine);

		// Must be called by the game thread before DrawRect() each frame.
		void SetWriteSlot(std::uint32_t slot)
		{
			m_writeSlot = slot;
		}

		void DrawRect(const UiRect& rect, glm::vec4 color = glm::vec4(1.f), std::int32_t layer = 0, float cornerRadiusPx = 0.0f);
		void DrawLine(const UiPoint& start, const UiPoint& end, float thicknessPx, glm::vec4 color = glm::vec4(1.f), std::int32_t layer = 0);
		void DrawCircle(const UiPoint& center, float radiusPx, glm::vec4 color = glm::vec4(1.f), std::int32_t layer = 0);

		[[nodiscard]] bool IsReady() const
		{
			return m_ready;
		}

	private:
		struct QuadPush
		{
			glm::vec4 screenSize; // .xy = viewport pixels
			uint64_t commandDataAddr = 0;
			uint32_t _pad0 = 0;
			uint32_t _pad1 = 0;
		};

		static_assert(sizeof(QuadPush) == 32, "QuadPush must match ui_shapes.slang push constant block.");

		struct ComputePush
		{
			uint64_t commandDataAddr = 0;
			uint64_t indirectCmdAddr = 0;
			uint32_t commandCount = 0;
			uint32_t _pad0 = 0;
		};

		static_assert(sizeof(ComputePush) == 24, "ComputePush must match ui_build_draws.slang push constant block.");

		struct DrawCommandData
		{
			glm::vec4 data0{}; // Rect: x,y,w,h | Circle: cx,cy,r,0 | Line: x0,y0,x1,y1
			glm::vec4 data1{}; // x = cornerRadius (rect) or thickness (line), others reserved
			glm::vec4 color{}; // RGBA
			uint32_t type = 0; // ShapeType
			int32_t layer = 0; // painter's order key
			uint32_t _pad0 = 0;
			uint32_t _pad1 = 0;
		};

		static_assert(sizeof(DrawCommandData) == 64, "DrawCommandData must match ui_shapes.slang/ui_build_draws.slang.");

		struct PendingQuad
		{
			DrawCommandData cmd;
		};

		void EnsurePassRegistered();
		void EnsureComputePipeline();
		void RegisterPass();

		std::string m_passName;
		std::string m_buildPassName;
		AetherCore* m_engine = nullptr;
		GraphicsPipeline m_pipeline;
		VkPipeline m_computePipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_computePipelineLayout = VK_NULL_HANDLE;
		// Double-buffered pending draw list. Game thread writes to m_writeSlot;
		// render thread reads from ctx.frameIndex % 2 (guaranteed to be different).
		std::array<std::vector<PendingQuad>, Swapchain::kMaxFramesInFlight> m_pendingQuads;
		std::array<UniqueBuffer, Swapchain::kMaxFramesInFlight> m_commandBuffers;
		std::array<std::size_t, Swapchain::kMaxFramesInFlight> m_commandBufferCapacities{};
		std::array<UniqueBuffer, Swapchain::kMaxFramesInFlight> m_indirectBuffers;
		std::uint32_t m_writeSlot = 0;
		bool m_ready = false;
	};
} // namespace aether

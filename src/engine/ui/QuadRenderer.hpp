#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/GpuHandles.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "vulkan/Swapchain.hpp"
#include "ui/UiLayout.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether
{
	class RenderGraph;
	class VulkanContext;
	class BindlessManager;
	class Swapchain;

	class QuadRenderer
	{
	public:
		enum class ShapeType : std::uint32_t
		{
			Rect = 0,
			Circle = 1,
			Line = 2,
			TexturedRect = 3,
			SdfGlyph = 4,
		};

		QuadRenderer() = default;
		~QuadRenderer() = default;

		QuadRenderer(const QuadRenderer&) = delete;
		QuadRenderer& operator=(const QuadRenderer&) = delete;

		void Init(ServiceContainer& services, std::string_view passName);
		void Shutdown(ServiceContainer& services);

		// Must be called by the game thread before DrawRect() each frame.
		void SetWriteSlot(std::uint32_t slot)
		{
			m_writeSlot = slot;
		}

		void DrawRect(const UiRect& rect, glm::vec4 color = glm::vec4(1.f), std::int32_t layer = 0, float cornerRadiusPx = 0.0f);
		void DrawLine(const UiPoint& start, const UiPoint& end, float thicknessPx, glm::vec4 color = glm::vec4(1.f), std::int32_t layer = 0);
		void DrawCircle(const UiPoint& center, float radiusPx, glm::vec4 color = glm::vec4(1.f), std::int32_t layer = 0);
		// Draws a bindless-sampled texture on a quad. uvRect = (u0, v0, u1, v1).
		void DrawTexturedRect(const UiRect& rect, std::uint32_t textureSlot, glm::vec4 uvRect = glm::vec4(0.f, 0.f, 1.f, 1.f), glm::vec4 tint = glm::vec4(1.f), std::int32_t layer = 0);
		// Draws one SDF font glyph. glyphRectPx is pixel-space (x,y,w,h); uvRect is atlas UVs.
		// Routes through the quad sort pass so text and quads share the same layer ordering.
		void DrawGlyph(glm::vec4 glyphRectPx, glm::vec4 uvRect, glm::vec4 color, std::uint32_t atlasSlot, std::int32_t layer);

		// Clip rect: any draw call whose resolved pixel rect lies entirely outside
		// the active clip is discarded on the CPU before reaching the GPU.
		void SetClipRect(glm::vec4 pixelRect); // x, y, w, h
		void ClearClipRect();

		[[nodiscard]] bool IsReady() const
		{
			return m_ready;
		}

		// Re-registers UI render passes in the render graph (needed after a
		// swapchain recreation that clears the graph).  Safe to call multiple
		// times - the underlying RegisterPass will add duplicate passes if
		// called redundantly.
		void ReRegisterPass();

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
			uint32_t N = 0;           // padded element count (next power of two)
			uint32_t stage = 0;       // bitonic stage (2,4,8,...,N) or 0 for write-indirect
			uint32_t step = 0;        // compare distance within stage
			uint32_t actualCount = 0; // original count for DrawIndirectCommand
		};

		static_assert(sizeof(ComputePush) == 32, "ComputePush must match ui_build_draws.slang push constant block.");

		struct DrawCommandData
		{
			glm::vec4 data0{};        // Rect: x,y,w,h | Circle: cx,cy,r,0 | Line: x0,y0,x1,y1
			glm::vec4 data1{};        // Rect/Line: cornerRadius/thickness | TexturedRect: u0,v0,u1,v1
			glm::vec4 color{};        // RGBA tint
			uint32_t type = 0;        // ShapeType
			int32_t layer = 0;        // painter's order key
			uint32_t textureSlot = 0; // bindless slot (TexturedRect only)
			uint32_t _pad1 = 0;
		};

		static_assert(sizeof(DrawCommandData) == 64, "DrawCommandData must match ui_shapes.slang/ui_build_draws.slang.");

		struct PendingQuad
		{
			DrawCommandData cmd;
		};

		void RegisterPass();

		struct ClipState
		{
			bool active = false;
			glm::vec4 pixelRect{}; // x, y, w, h
		};

		// Returns true if the pixel rect is entirely outside the active clip.
		[[nodiscard]] bool IsClipped(glm::vec4 pxRect) const;

		std::string m_passName;
		VulkanContext* m_vkCtx = nullptr;
		RenderGraph* m_renderGraph = nullptr;
		BindlessManager* m_bindlessMgr = nullptr;
		Swapchain* m_swapchain = nullptr;
		GraphicsPipeline m_pipeline;

		// Double-buffered pending draw list. Game thread writes to m_writeSlot;
		// render thread reads from ctx.frameIndex % 2 (guaranteed to be different).
		struct PerFrameMapped
		{
			gpu::BufferHandle handle{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
			std::size_t capacity = 0;
		};

		struct PerFrameDevice
		{
			gpu::BufferHandle handle{};
			gpu::DeviceAddress address = 0;
		};

		std::array<std::vector<PendingQuad>, Swapchain::kMaxFramesInFlight> m_pendingQuads;
		std::array<PerFrameMapped, Swapchain::kMaxFramesInFlight> m_commandBuffers;
		std::array<PerFrameDevice, Swapchain::kMaxFramesInFlight> m_indirectBuffers;
		std::uint32_t m_writeSlot = 0;
		ClipState m_clipState{};
		bool m_ready = false;
	};
} // namespace aether

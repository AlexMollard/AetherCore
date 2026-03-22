#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace meow
{
	class CommandRecorder;
	class GraphicsPipeline;
	class Mesh;

	// A lightweight typed draw-call submission record.
	// Submitted by game/app code; consumed by the engine during EndFrame.
	struct DrawCommand
	{
		const GraphicsPipeline* pipeline = nullptr;
		const Mesh* mesh = nullptr;  // null = no vertex buffer (shader-hardcoded verts)
		std::uint32_t           vertexCount = 0;        // used when mesh == nullptr
		std::uint32_t           instanceCount = 1;
		glm::mat4               modelMatrix{ 1.0f };    // per-object world transform, pushed as push constant
	};

	// Per-frame bucket that collects DrawCommands from app/scene code and flushes
	// them into a CommandRecorder at the start of EndFrame.
	class RenderQueue
	{
	public:
		void Submit(const DrawCommand& cmd);

		// Engine-internal: record all queued commands into the recorder then clear.
		// frameConstantsAddr is the BDA of the per-frame FrameConstants buffer written by MeowCore.
		void Flush(CommandRecorder& recorder, VkDeviceAddress frameConstantsAddr);
		void Clear();

		[[nodiscard]] bool IsEmpty() const;

	private:
		std::vector<DrawCommand> m_commands;
	};
}

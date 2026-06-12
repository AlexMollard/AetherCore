#pragma once

#include <cstdint>

#include "gpu/CommandList.hpp"

namespace aether::gpu
{
	// RAII helper for one-shot GPU command buffer submission.
	//
	// Encapsulates the common pattern: allocate + begin a one-time command
	// buffer, record commands via gpu::CommandList, then end + submit + wait
	// for completion.  The implementation lives in vulkan/OneShotCmd.cpp.
	//
	// Usage:
	//   gpu::OneShotCmd cmd;
	//   if (!cmd.Begin(m_device, m_pool)) { /* error */ }
	//   cmd.CmdList().PipelineMemoryBarrier(...);
	//   cmd.CmdList().CopyBuffer(...);
	//   if (!cmd.EndAndSubmit(m_queue)) { /* error */ }
	//
	// Move-only.  After a move the source is in the empty state; calling
	// EndAndSubmit on an empty object is a no-op (returns false).
	class OneShotCmd
	{
	public:
		OneShotCmd() = default;
		~OneShotCmd();

		OneShotCmd(const OneShotCmd&) = delete;
		OneShotCmd& operator=(const OneShotCmd&) = delete;

		OneShotCmd(OneShotCmd&& other) noexcept;
		OneShotCmd& operator=(OneShotCmd&& other) noexcept;

		// Allocate + begin a one-time command buffer from the given pool.
		// device  - opaque pointer to VkDevice
		// pool    - opaque pointer to VkCommandPool
		[[nodiscard]] bool Begin(void* device, void* pool);

		// Access the gpu::CommandList for recording commands.
		// Only valid after a successful Begin() call.
		[[nodiscard]] gpu::CommandList& CmdList();

		// End the command buffer, submit to queue, wait for fence, and free.
		// queue - opaque pointer to VkQueue
		// Returns false if the object is empty or if any step fails.
		[[nodiscard]] bool EndAndSubmit(void* queue);

		// True when a command buffer has been allocated and is being recorded.
		[[nodiscard]] bool IsRecording() const
		{
			return m_cmd != nullptr;
		}

	private:
		void Release();

		void* m_device = nullptr;
		void* m_pool = nullptr;
		void* m_cmd = nullptr;
		gpu::CommandList m_cmdList;
	};
} // namespace aether::gpu

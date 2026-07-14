#pragma once

#include <cstdint>

#include "gpu/CommandList.hpp"

namespace aether::gpu
{
	class OneShotCmd
	{
	public:
		OneShotCmd() = default;
		~OneShotCmd();

		OneShotCmd(const OneShotCmd&) = delete;
		OneShotCmd& operator=(const OneShotCmd&) = delete;

		OneShotCmd(OneShotCmd&& other) noexcept;
		OneShotCmd& operator=(OneShotCmd&& other) noexcept;

		[[nodiscard]] bool Begin(void* device, void* pool);

		[[nodiscard]] gpu::CommandList& CmdList();

		[[nodiscard]] bool EndAndSubmit(void* queue);

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

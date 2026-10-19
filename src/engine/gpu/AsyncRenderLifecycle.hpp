#pragma once

#include <cstdint>

namespace aether::gpu
{
	enum class RenderState
	{
		Initializing,
		Ready,
		Failed,
		DeviceLost,
		Stopping,
		Stopped
	};

	// Owner-thread state. Async completions retire submissions even after stop/loss.
	class AsyncRenderLifecycle
	{
	public:
		[[nodiscard]] RenderState State() const
		{
			return m_state;
		}

		bool Ready()
		{
			if (m_state != RenderState::Initializing)
			{
				return false;
			}
			m_state = RenderState::Ready;
			return true;
		}

		bool Submit()
		{
			if (m_state != RenderState::Ready)
			{
				return false;
			}
			++m_pending;
			return true;
		}

		void Complete()
		{
			if (m_pending > 0)
			{
				--m_pending;
			}
			if (m_state == RenderState::Stopping && m_pending == 0)
			{
				m_state = RenderState::Stopped;
			}
		}

		void Stop()
		{
			m_state = m_pending == 0 ? RenderState::Stopped : RenderState::Stopping;
		}

		void Fail(bool deviceLost = false)
		{
			if (m_state == RenderState::Initializing || m_state == RenderState::Ready)
			{
				m_state = deviceLost ? RenderState::DeviceLost : RenderState::Failed;
			}
		}

	private:
		RenderState m_state = RenderState::Initializing;
		std::uint32_t m_pending = 0;
	};
} // namespace aether::gpu

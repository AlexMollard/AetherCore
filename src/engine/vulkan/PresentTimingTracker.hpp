#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "vulkan/volk.hpp"

namespace aether
{
	// Learns when presents actually reach the screen, using VK_KHR_present_wait.
	//
	// This is the piece that makes frame pacing honest. Everything else the engine can
	// observe - the in-flight wait, the present call, the loop period - happens on the CPU
	// side of the presentation engine and only correlates with the display flip. Pacing
	// against a phase INFERRED from those signals was measured to be strictly worse than not
	// pacing at all: it missed every other vsync. vkWaitForPresentKHR is the one call that
	// answers "has frame N been shown yet", so a thread that waits on each present in turn
	// and timestamps the return recovers both the flip period and its phase.
	//
	// The wait blocks until the frame is displayed, so it cannot run on the game or render
	// thread; it gets its own. That thread touches no engine state - it publishes two atomics
	// and nothing else reads its internals.
	class PresentTimingTracker
	{
	public:
		using Clock = std::chrono::steady_clock;

		~PresentTimingTracker()
		{
			Stop();
		}

		PresentTimingTracker() = default;
		PresentTimingTracker(const PresentTimingTracker&) = delete;
		PresentTimingTracker& operator=(const PresentTimingTracker&) = delete;

		// device must outlive the tracker. Safe to call again after Stop().
		void Start(VkDevice device);
		void Stop();

		// Called from the present path with the id just handed to vkQueuePresentKHR. A
		// swapchain handle is taken per frame because it changes on every recreate, and
		// waiting on a retired swapchain is undefined.
		void OnPresented(VkSwapchainKHR swapchain, std::uint64_t presentId);

		// Retire the current swapchain. The waiter drops any pending id for it, so a recreate
		// cannot leave the thread blocked on a handle that is about to be destroyed.
		void OnSwapchainRetired();

		// False while the display period is unstable. On a variable-refresh display (G-Sync,
		// FreeSync) the flip deadline is not periodic at all, so extrapolating one is exactly
		// the guess this class exists to replace - callers must stand down rather than pace
		// against a cadence the display is not keeping.
		[[nodiscard]] bool HasEstimate() const
		{
			return m_periodNs.load(std::memory_order_acquire) > 0 && !m_variableRate.load(std::memory_order_acquire);
		}

		[[nodiscard]] bool IsVariableRate() const
		{
			return m_variableRate.load(std::memory_order_acquire);
		}

		// Measured display period. Far more trustworthy than the mode's advertised refresh
		// rate, which says nothing about what the compositor is actually doing to us.
		[[nodiscard]] double PeriodMs() const
		{
			return static_cast<double>(m_periodNs.load(std::memory_order_acquire)) / 1'000'000.0;
		}

		// When the most recent observed present was displayed.
		[[nodiscard]] Clock::time_point LastFlip() const
		{
			return Clock::time_point{Clock::duration{m_lastFlipNs.load(std::memory_order_acquire)}};
		}

		[[nodiscard]] std::uint64_t ObservedFlips() const
		{
			return m_observedFlips.load(std::memory_order_relaxed);
		}

		// Next flip at or after `after`, extrapolated from the last observed one. Returns
		// `after` unchanged while there is no estimate, so callers degrade to not pacing
		// rather than to pacing against a guess.
		[[nodiscard]] Clock::time_point PredictNextFlip(Clock::time_point after) const;

	private:
		void ThreadMain();

		VkDevice m_device = VK_NULL_HANDLE;
		std::thread m_thread;

		mutable std::mutex m_mutex;
		std::condition_variable m_cv;
		VkSwapchainKHR m_currentSwapchain = VK_NULL_HANDLE;
		// Highest id handed to vkQueuePresentKHR. The waiter walks ids CONSECUTIVELY up to
		// this, rather than jumping to the newest: skipping ids yields multi-frame gaps that
		// are useless as a refresh-period estimate. A wait on an already-displayed id returns
		// immediately, so walking is self-correcting after a stall.
		std::uint64_t m_lastPresentedId = 0;
		bool m_reset = false;
		bool m_stop = false;

		std::atomic<std::int64_t> m_periodNs{0};
		std::atomic<std::int64_t> m_lastFlipNs{0};
		std::atomic<std::uint64_t> m_observedFlips{0};
		std::atomic<bool> m_variableRate{false};
	};
} // namespace aether

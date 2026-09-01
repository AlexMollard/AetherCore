#pragma once

#include <array>
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
		// latchTimeNs is when the frame being presented sampled its input, as a
		// steady_clock epoch count. Passing it here is what lets the waiter - which is the
		// only thing that knows when a frame actually reached the screen - close the loop and
		// report true input-to-photon latency. 0 means unknown and is simply not measured.
		void OnPresented(VkSwapchainKHR swapchain, std::uint64_t presentId, std::int64_t latchTimeNs = 0);

		// Retire the current swapchain. BLOCKS until the waiter is out of any wait on it, so
		// the handle can be destroyed the moment this returns. Callers must retire BEFORE
		// vkDestroySwapchainKHR: vkWaitForPresentKHR cannot be cancelled, and a driver left
		// waiting on a freed handle faults rather than returning an error.
		void OnSwapchainRetired();

		// False while the display period is unstable. On a variable-refresh display (G-Sync,
		// FreeSync) the flip deadline is not periodic at all, so extrapolating one is exactly
		// the guess this class exists to replace - callers must stand down rather than pace
		// against a cadence the display is not keeping.
		[[nodiscard]] bool HasEstimate() const
		{
			return m_periodNs.load(std::memory_order_acquire) > 0 && !m_variableRate.load(std::memory_order_acquire);
		}

		// Measured latch-to-flip: from the moment a frame sampled input to the moment it was
		// actually on screen. This is the number every other latency signal in the engine only
		// approximates - the in-flight wait, the present call and the reserve are all guesses
		// at it - and it is the one worth tuning graphics.syncSlackMs against.
		[[nodiscard]] float LatchToFlipMs() const
		{
			return static_cast<float>(static_cast<double>(m_latchToFlipNs.load(std::memory_order_acquire)) / 1'000'000.0);
		}

		// Worst case seen since the last call, then reset. A mean hides exactly the frames a
		// user notices.
		[[nodiscard]] float TakeWorstLatchToFlipMs() const
		{
			return static_cast<float>(static_cast<double>(m_worstLatchToFlipNs.exchange(0, std::memory_order_acq_rel)) / 1'000'000.0);
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
		// Signals the waiter leaving vkWaitForPresentKHR. Separate from m_cv because the two
		// have opposite directions - m_cv parks the waiter, this one parks whoever is retiring
		// a swapchain out from under it.
		std::condition_variable m_idleCv;
		VkSwapchainKHR m_currentSwapchain = VK_NULL_HANDLE;
		// The handle the waiter is inside vkWaitForPresentKHR on right now, published under
		// the mutex in the same critical section that reads m_currentSwapchain. Without that
		// pairing there is a window where a retire sees "not waiting", frees the handle, and
		// the waiter then enters the call with the stale copy it read a moment earlier.
		VkSwapchainKHR m_waitingOn = VK_NULL_HANDLE;
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
		// Smoothed latch-to-flip, and the worst since it was last read.
		std::atomic<std::int64_t> m_latchToFlipNs{0};
		// Mutable: reading the worst-since-last-read resets it, which does not change what
		// the tracker IS, and every accessor here is reached through a const reference.
		mutable std::atomic<std::int64_t> m_worstLatchToFlipNs{0};
		// Latch times keyed by present id. Ids are consecutive and the waiter trails the
		// presenter by only a frame or two, so a small power-of-two ring cannot be lapped.
		static constexpr std::size_t kLatchRing = 64;
		std::array<std::atomic<std::int64_t>, kLatchRing> m_latchNs{};
	};
} // namespace aether

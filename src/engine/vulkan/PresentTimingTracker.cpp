#include "vulkan/PresentTimingTracker.hpp"

#include "vulkan/volk.hpp"

#include <algorithm>
#include <array>

#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		// A flip interval outside this range is not a refresh rate; it is a stall, an occluded
		// window, or a monitor change. Folding those into the estimate would poison it for
		// hundreds of frames, and the pacer would reserve against a period that never existed.
		constexpr double kMinPlausiblePeriodMs = 1.0;  // 1000 Hz
		constexpr double kMaxPlausiblePeriodMs = 60.0; // ~16 Hz

		// Slow enough to ignore jitter, fast enough to follow a genuine refresh-rate change
		// within a second or so.
		constexpr double kPeriodSmoothing = 0.05;

		// Separates a variable-refresh display from ordinary jitter. A fixed 60 Hz panel was
		// MEASURED at 15.5-18.0 ms flip to flip - a 15% spread - so a tight threshold flags
		// every fixed display as variable and switches pacing off (paced fell 1404 -> 108).
		// Real VRR swings far wider than this: 8 ms to 33 ms is over 150%.
		constexpr double kVariableRateSpread = 0.5;
		constexpr std::size_t kSpreadWindow = 32;
	} // namespace

	void PresentTimingTracker::Start(VkDevice device)
	{
		Stop();
		{
			const std::lock_guard lock(m_mutex);
			m_device = device;
			m_stop = false;
			m_reset = true;
			m_lastPresentedId = 0;
			m_currentSwapchain = VK_NULL_HANDLE;
			m_waitingOn = VK_NULL_HANDLE;
		}
		m_periodNs.store(0, std::memory_order_release);
		m_lastFlipNs.store(0, std::memory_order_release);
		m_observedFlips.store(0, std::memory_order_relaxed);
		m_thread = std::thread(&PresentTimingTracker::ThreadMain, this);
	}

	void PresentTimingTracker::Stop()
	{
		{
			const std::lock_guard lock(m_mutex);
			if (m_stop && !m_thread.joinable())
			{
				return;
			}
			m_stop = true;
		}
		m_cv.notify_all();
		if (m_thread.joinable())
		{
			m_thread.join();
		}
	}

	void PresentTimingTracker::OnPresented(VkSwapchainKHR swapchain, const std::uint64_t presentId)
	{
		{
			const std::lock_guard lock(m_mutex);
			if (m_stop)
			{
				return;
			}
			m_currentSwapchain = swapchain;
			m_lastPresentedId = presentId;
		}
		m_cv.notify_one();
	}

	void PresentTimingTracker::OnSwapchainRetired()
	{
		{
			std::unique_lock lock(m_mutex);
			m_reset = true;
			m_lastPresentedId = 0;
			m_currentSwapchain = VK_NULL_HANDLE;
			// Clearing m_currentSwapchain stops the waiter picking the handle up again, but a
			// wait already in flight is inside the driver and cannot be cancelled. Block until
			// it returns of its own accord - one frame in steady state, at worst the waiter's
			// own timeout. Costing a frame here is what buys the caller the right to destroy
			// the handle on the next line.
			m_cv.notify_all();
			m_idleCv.wait(lock, [this] { return m_waitingOn == VK_NULL_HANDLE; });
		}
		// The period survives a recreate (the display did not change), but the phase does not.
		m_lastFlipNs.store(0, std::memory_order_release);
	}

	PresentTimingTracker::Clock::time_point PresentTimingTracker::PredictNextFlip(const Clock::time_point after) const
	{
		const std::int64_t periodNs = m_periodNs.load(std::memory_order_acquire);
		const std::int64_t lastNs = m_lastFlipNs.load(std::memory_order_acquire);
		if (periodNs <= 0 || lastNs <= 0)
		{
			return after;
		}

		const auto last = Clock::time_point{Clock::duration{lastNs}};
		const auto period = std::chrono::nanoseconds{periodNs};
		if (last >= after)
		{
			return last;
		}

		// Integer step rather than a loop: after a long stall this could otherwise be
		// thousands of iterations on the game thread.
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(after - last).count();
		const std::int64_t steps = (elapsed + periodNs - 1) / periodNs;
		return last + std::chrono::nanoseconds{steps * periodNs};
	}

	void PresentTimingTracker::ThreadMain()
	{
		AE_PROFILE_THREAD("PresentTiming");

		auto waitForPresent = reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(m_device, "vkWaitForPresentKHR"));
		if (waitForPresent == nullptr)
		{
			AE_WARN(LogCategory::Vulkan, "vkWaitForPresentKHR unavailable despite the extension being enabled; present timing disabled.");
			return;
		}

		std::int64_t previousFlipNs = 0;
		std::array<double, kSpreadWindow> recentMs{};
		std::size_t recentCount = 0;
		std::size_t recentHead = 0;

		std::uint64_t nextId = 1;

		while (true)
		{
			VkSwapchainKHR swapchain = VK_NULL_HANDLE;
			{
				std::unique_lock lock(m_mutex);
				// Only wait on an id that has actually been submitted; waiting past the newest
				// present would block for a frame that does not exist yet.
				m_cv.wait(lock, [this, &nextId] { return m_stop || m_reset || m_lastPresentedId >= nextId; });
				if (m_stop)
				{
					return;
				}
				if (m_reset)
				{
					m_reset = false;
					nextId = 1;
					previousFlipNs = 0;
					continue;
				}
				swapchain = m_currentSwapchain;
				// Claim the handle while still holding the lock the retire path takes, so a
				// retire either lands before this claim (and we read VK_NULL_HANDLE) or waits
				// for the release below. Publishing it after unlocking would reopen the exact
				// window this is here to close.
				m_waitingOn = swapchain;
			}

			if (swapchain == VK_NULL_HANDLE)
			{
				continue;
			}
			const std::uint64_t presentId = nextId;

			// A bounded timeout matters: if the window is minimised the presentation engine
			// may never display this id, and a retire blocks on the release below until this
			// call returns.
			constexpr std::uint64_t kTimeoutNs = 200'000'000; // 200 ms
			const VkResult result = waitForPresent(m_device, swapchain, presentId, kTimeoutNs);
			const auto now = Clock::now();
			{
				const std::lock_guard lock(m_mutex);
				m_waitingOn = VK_NULL_HANDLE;
			}
			m_idleCv.notify_all();

			if (result != VK_SUCCESS)
			{
				// Timeout, out-of-date, or suboptimal. The phase is now unknown - keeping the
				// old one would have the pacer aiming at a flip that already passed.
				previousFlipNs = 0;
				m_lastFlipNs.store(0, std::memory_order_release);
				// Do not retry the same id forever; it may belong to a swapchain that is gone.
				++nextId;
				continue;
			}
			++nextId;

			const std::int64_t nowNs = now.time_since_epoch().count();
			if (previousFlipNs > 0)
			{
				const double deltaMs = static_cast<double>(nowNs - previousFlipNs) / 1'000'000.0;
				if (deltaMs >= kMinPlausiblePeriodMs && deltaMs <= kMaxPlausiblePeriodMs)
				{
					const std::int64_t currentNs = m_periodNs.load(std::memory_order_acquire);
					const auto sampleNs = static_cast<std::int64_t>(deltaMs * 1'000'000.0);
					const std::int64_t updatedNs = (currentNs <= 0)
					        ? sampleNs
					        : static_cast<std::int64_t>(static_cast<double>(currentNs) * (1.0 - kPeriodSmoothing) + static_cast<double>(sampleNs) * kPeriodSmoothing);
					m_periodNs.store(updatedNs, std::memory_order_release);
					m_observedFlips.fetch_add(1, std::memory_order_relaxed);

					recentMs[recentHead] = deltaMs;
					recentHead = (recentHead + 1) % kSpreadWindow;
					recentCount = std::min(recentCount + 1, kSpreadWindow);
					if (recentCount == kSpreadWindow)
					{
						const auto [lo, hi] = std::minmax_element(recentMs.begin(), recentMs.end());
						const double mean = static_cast<double>(updatedNs) / 1'000'000.0;
						m_variableRate.store(mean > 0.0 && (*hi - *lo) / mean > kVariableRateSpread, std::memory_order_release);
					}
				}
			}
			previousFlipNs = nowNs;
			m_lastFlipNs.store(nowNs, std::memory_order_release);
		}
	}
} // namespace aether

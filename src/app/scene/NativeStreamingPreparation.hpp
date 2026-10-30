#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "scene/SceneSerializer.hpp"
#include "streaming/StreamingBudget.hpp"

namespace aether::app::scene
{
	[[nodiscard]] std::string ResolveNativeSceneStreamingDependency(std::string_view reference);

	// The ledger must outlive every job and result. Destroy the scene before the
	// reservation: Result's member order deliberately enforces that ordering.
	struct NativePreparedRegion
	{
		struct Reservation
		{
			streaming::BudgetLedger* ledger = nullptr;
			std::uint64_t bytes = 0;
			~Reservation();
		};

		std::shared_ptr<Reservation> reservation;
		std::shared_ptr<const SceneDescription> scene;
		std::vector<std::string> dependencies;
		std::string error;
		streaming::RequestId request = 0;
	};

	class NativeStreamingPreparation
	{
	public:
		struct Job
		{
			std::optional<NativePreparedRegion> Take();

		private:
			friend class NativeStreamingPreparation;
			std::mutex mutex;
			std::optional<NativePreparedRegion> result;
		};

		// At most maxPendingRequests jobs can be outstanding, including completed
		// jobs not yet consumed. Returns null when admission is full.
		std::shared_ptr<Job> Submit(std::string reference, std::string regionId, streaming::RequestId request, io::IOPriority priority, std::shared_ptr<std::atomic_bool> cancelled, streaming::BudgetLedger& ledger);

	private:
		std::mutex m_mutex;
		std::vector<std::weak_ptr<Job>> m_jobs;
	};
} // namespace aether::app::scene

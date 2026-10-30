#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "streaming/StreamingTypes.hpp"
#include "utils/Expected.hpp"

namespace aether::app::scripting
{
	struct SceneContext;
}

namespace aether::app::scene
{
	struct ApplySceneDeps;

	// Runtime-only ownership; never serialized as an authored component.
	struct StreamingRegionOwner
	{
		std::string region;
	};

	// Backends own prepared documents, dependency leases and persisted region snapshots.
	// All calls are game-thread calls; workers may publish immutable completion data only.
	class IWorldStreamingBackend
	{
	public:
		virtual ~IWorldStreamingBackend() = default;
		virtual Expected<void> SetBudgets(const streaming::Budgets& budgets) = 0;
		virtual Expected<void> SetDemand(const streaming::Region& region, streaming::Demand demand, io::IOPriority priority) = 0;
		virtual void Update() = 0;
		[[nodiscard]] virtual streaming::State GetState(std::string_view regionId) const = 0;
		[[nodiscard]] virtual std::string GetDiagnosticsJson() const = 0;
		// Whole-world teardown; invalidate jobs and retire ownership before ECS replacement.
		virtual void ResetWorld() = 0;
		virtual void Shutdown() = 0;
	};

	class WorldStreaming
	{
	public:
		explicit WorldStreaming(std::unique_ptr<IWorldStreamingBackend> backend);
		~WorldStreaming();
		WorldStreaming(const WorldStreaming&) = delete;
		WorldStreaming& operator=(const WorldStreaming&) = delete;

		Expected<void> Configure(std::span<const streaming::Region> regions);
		Expected<void> SetBudgets(const streaming::Budgets& budgets);
		Expected<void> SetSource(streaming::Source source);
		bool RemoveSource(std::string_view sourceId);
		[[nodiscard]] streaming::RequestId Request(std::string_view regionId, bool activate, io::IOPriority priority = io::IOPriority::Normal);
		bool Cancel(streaming::RequestId request);
		bool Release(streaming::RequestId request);
		[[nodiscard]] streaming::State GetRequestState(streaming::RequestId request) const;
		[[nodiscard]] streaming::State GetRegionState(std::string_view regionId) const;
		[[nodiscard]] std::string GetDiagnosticsJson() const;
		void Update();
		void ResetWorld();
		void Shutdown();

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};

	[[nodiscard]] std::unique_ptr<IWorldStreamingBackend> CreateNativeStreamingBackend(scripting::SceneContext& context, const ApplySceneDeps& deps);
} // namespace aether::app::scene

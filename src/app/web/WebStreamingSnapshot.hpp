#pragma once

#include <cstdint>
#include <string>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include "scene/World.hpp"
#include "scripting/GameplayHost.hpp"

namespace aether::app::web
{
	struct RegionSnapshotStorage;
	// Maximum encoded script section; the caller must reserve this plus the host's
	// one-MiB capture buffer before invoking any script capture callback.
	inline constexpr std::size_t kStreamingScriptSnapshotLimit = 16 * 1024 * 1024;
	inline constexpr std::size_t kStreamingScriptCaptureScratch = 1024 * 1024;
	// Maximum persisted payload; this is not a bound on capture workspace.
	inline constexpr std::size_t kStreamingSnapshotInputLimit = 64 * 1024 * 1024;

	struct StreamingScriptInstance
	{
		Entity entity;
		std::uint64_t handle;
		std::string type;
		std::uint32_t index;
	};

	struct StreamingSavedProperty
	{
		std::string name;
		ScriptPropertyValue value;
		std::uint64_t node = 0;
		int propertyIndex = -1;
	};

	struct StreamingSavedScript
	{
		std::uint64_t node = 0;
		Entity entity{};
		std::uint32_t index = 0;
		std::uint64_t handle = 0;
		std::string type;
		std::vector<StreamingSavedProperty> properties;
		std::string state;
	};

	struct StreamingScriptRestoreCursor
	{
		std::vector<StreamingSavedScript> scripts;
		std::size_t next = 0;
		bool failed = false;
	};

	Expected<StreamingScriptRestoreCursor> PrepareStreamingScriptRestore(World& world, scripting::GameplayHost& host, const std::vector<StreamingScriptInstance>& instances, std::string_view data);
	Expected<bool> StepStreamingScriptRestore(StreamingScriptRestoreCursor& cursor, scripting::GameplayHost& host, std::size_t maxCount);

	struct StreamingScriptCaptureBudget
	{
		std::uint64_t workspace = 0;
		std::uint32_t maxPropertyBytes = 0;
	};

	// Logical script-capture workspace only; scene and final frame need separate admission.
	Expected<StreamingScriptCaptureBudget> MeasureStreamingScriptCapture(scripting::GameplayHost& host, std::span<const StreamingScriptInstance> instances);
	// Logical scene workspace after hooks; previously admitted hook/script blobs stay charged separately.
	Expected<std::uint64_t> ComputeRegionSnapshotWorkspace(const RegionSnapshotStorage& storage, std::uint64_t scriptBytes, std::uint64_t runtimeEntryBytes);
	// Returned views borrow the supplied frame.
	Expected<std::pair<std::string_view, std::uint64_t>> StreamingSnapshotDocument(std::string_view frame);

	Expected<std::string> CaptureStreamingScripts(World& world, scripting::GameplayHost& host, const std::vector<StreamingScriptInstance>& instances, std::span<const std::string> states, std::uint32_t maxPropertyBytes);
} // namespace aether::app::web

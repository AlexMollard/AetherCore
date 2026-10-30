#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "scene/SceneSerializer.hpp"
#include "io/IOPriority.hpp"
#include "scene/reflection/Reflection.hpp"
#include "streaming/StreamingBudget.hpp"
#include "systems/ScriptComponentSystem.hpp"

namespace aether::app::scene
{
	class NativeStreamingSnapshot
	{
	public:
		struct Velocity
		{
			std::uint64_t node = 0;
			std::uint8_t dimensions = 0;
			glm::vec3 linear{};
			glm::vec3 angular{};
		};

		struct Animation
		{
			std::uint64_t node = 0;
			float frameTime = 0;
			float fixedAccumulator = 0;
			std::uint32_t frame = 0;
			std::int32_t direction = 1;
			bool playing = false;
			bool initialized = false;
		};

		struct Reference
		{
			std::uint64_t node = 0;
			std::string component;
			std::string field;
			reflect::FieldValue value;
		};

		class SaveTicket
		{
		public:
			std::optional<Expected<void>> Poll() const;
			bool Cancel();

		private:
			friend class NativeStreamingSnapshot;
			mutable std::mutex mutex;
			std::optional<Expected<void>> result;
			bool cancelled = false;
		};

		struct Loaded
		{
			SceneDescription scene;
			std::vector<Velocity> velocities;
			std::vector<Animation> animations;
			std::vector<Reference> references;
			std::vector<ScriptComponentSystem::StreamingScriptState> scripts;
			streaming::BudgetLedger* ledger = nullptr;
			std::uint64_t reserved = 0;
			Loaded() = default;
			~Loaded();
			Loaded(const Loaded&) = delete;
			Loaded& operator=(const Loaded&) = delete;
			Loaded(Loaded&& other) noexcept;
			Loaded& operator=(Loaded&& other) noexcept;
		};

		struct SaveOperation
		{
			std::shared_ptr<SaveTicket> ticket;
			std::shared_ptr<Loaded> rollback;
		};

		class LoadTicket
		{
		public:
			std::optional<Expected<std::optional<Loaded>>> Take();

		private:
			friend class NativeStreamingSnapshot;
			std::mutex mutex;
			std::optional<Expected<std::optional<Loaded>>> result;
		};

		explicit NativeStreamingSnapshot(streaming::BudgetLedger& ledger);
		~NativeStreamingSnapshot();
		NativeStreamingSnapshot(const NativeStreamingSnapshot&) = delete;
		NativeStreamingSnapshot& operator=(const NativeStreamingSnapshot&) = delete;
		void Reset();
		Expected<std::optional<SaveOperation>> CaptureAndSave(scripting::SceneContext& context,
		        ApplySceneDeps& deps,
		        std::string_view region,
		        const std::vector<Entity>& roots,
		        const std::vector<Entity>& entities,
		        const std::vector<Entity>& temporarilyDisabled,
		        const std::vector<Velocity>& velocityOverrides,
		        io::IOPriority priority);
		std::shared_ptr<LoadTicket> LoadAsync(std::string_view region, io::IOPriority priority);
		Expected<void> Restore(scripting::SceneContext& context, const std::vector<Entity>& entities, const Loaded& snapshot);
		Expected<void> PrepareForDetach(World& world, const std::vector<Entity>& entities);
		Expected<void> ResolveDeferredReferences(scripting::SceneContext& context);
		void ForgetDeferredReferences(World& world, const std::vector<Entity>& entities);

	private:
		struct Session;

		static Expected<std::optional<Loaded>> LoadFromSession(const std::shared_ptr<Session>& session, streaming::BudgetLedger& ledger, std::string_view region);
		streaming::BudgetLedger& m_ledger;
		std::shared_ptr<Session> m_session;
	};
} // namespace aether::app::scene

#include "WebGameplay.hpp"
#include "WebStreamingSnapshot.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Hierarchy.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#if defined(AE_WEB_MANAGED)
#	include "BrowserManaged.hpp"
#	include "scripting/SceneContext.hpp"
#endif

namespace aether::app::web
{
	namespace
	{
		constexpr std::uint32_t kSnapshotMagic = 0x33525357;

		template<typename T>
		void Append(std::string& out, const T& value)
		{
			out.append(reinterpret_cast<const char*>(&value), sizeof(value));
		}

		template<typename T>
		bool Take(std::string_view& input, T& value)
		{
			if (input.size() < sizeof(value))
			{
				return false;
			}
			std::memcpy(&value, input.data(), sizeof(value));
			input.remove_prefix(sizeof(value));
			return true;
		}

		struct Velocity
		{
			std::uint64_t node = 0;
			std::uint32_t flags = 0;
			float linear3[3]{}, angular3[3]{}, linear2[2]{}, angular2 = 0;
			float frameTime = 0, fixedAccumulator = 0;
			std::uint32_t currentFrame = 0;
			std::int32_t direction = 1;
			std::uint8_t playing = 0, initialized = 0;
		};

		bool ValidGateHierarchy(const scene::SceneDescription& description, std::uint64_t node)
		{
			const auto gate = std::find_if(description.entities.begin(), description.entities.end(), [node](const auto& record) { return record.nodeId == node; });
			if (gate == description.entities.end() || gate->parentIndex >= 0 || std::count_if(description.entities.begin(), description.entities.end(), [node](const auto& record) { return record.nodeId == node; }) != 1)
			{
				return false;
			}
			const auto gateIndex = static_cast<std::size_t>(std::distance(description.entities.begin(), gate));
			std::vector<std::uint8_t> ancestry(description.entities.size(), 0);
			ancestry[gateIndex] = 2;
			for (std::size_t i = 0; i < ancestry.size(); ++i)
			{
				std::size_t current = i;
				while (ancestry[current] == 0)
				{
					ancestry[current] = 1;
					const auto parent = description.entities[current].parentIndex;
					if (parent < 0 || static_cast<std::size_t>(parent) >= ancestry.size())
					{
						return false;
					}
					current = static_cast<std::size_t>(parent);
				}
				if (ancestry[current] != 2)
				{
					return false;
				}
				current = i;
				while (ancestry[current] == 1)
				{
					ancestry[current] = 2;
					current = static_cast<std::size_t>(description.entities[current].parentIndex);
				}
			}
			return true;
		}

		bool Sections(std::string_view data, std::string_view& document, std::string_view& scripts, std::string_view& velocities, std::uint64_t& gateNode)
		{
			if (data.size() > kStreamingSnapshotInputLimit)
			{
				return false;
			}
			std::uint32_t magic, docSize, scriptSize, velocityCount;
			if (!Take(data, magic) || !Take(data, docSize) || !Take(data, scriptSize) || !Take(data, velocityCount) || !Take(data, gateNode) || magic != kSnapshotMagic || !gateNode || velocityCount > 65536)
			{
				return false;
			}
			const auto velocityBytes = static_cast<std::size_t>(velocityCount) * sizeof(Velocity);
			if (docSize > data.size() || scriptSize > data.size() - docSize || velocityBytes != data.size() - docSize - scriptSize)
			{
				return false;
			}
			document = data.substr(0, docSize);
			scripts = data.substr(docSize, scriptSize);
			velocities = data.substr(docSize + scriptSize);
			return true;
		}
	} // namespace

	Expected<std::pair<std::string_view, std::uint64_t>> StreamingSnapshotDocument(std::string_view frame)
	{
		std::string_view document, scripts, velocities;
		std::uint64_t gateNode = 0;
		if (!Sections(frame, document, scripts, velocities, gateNode))
		{
			return std::unexpected(AetherError::Asset("Invalid browser streaming snapshot"));
		}
		return std::pair{document, gateNode};
	}

	Expected<std::string> WebGameplay::CaptureRegion(const WebRegion& region)
	{
		struct Reservation
		{
			std::shared_ptr<streaming::BudgetLedger> budget;
			std::uint64_t bytes = 0;

			bool Add(std::uint64_t amount)
			{
				if (!budget || amount > std::numeric_limits<std::uint64_t>::max() - bytes || !budget->TryReserve(streaming::MemoryClass::CpuPending, amount))
				{
					return false;
				}
				bytes += amount;
				return true;
			}

			~Reservation()
			{
				if (budget && bytes)
				{
					budget->Release(streaming::MemoryClass::CpuPending, bytes);
				}
			}
		} reservation{m_streaming_budget};

		std::uint64_t instanceWorkspace = 0;
		for (const auto& instance: m_instances)
		{
			const auto cost = 4ull * (sizeof(StreamingScriptInstance) + sizeof(std::string) + instance.type.size() + 1);
			if (cost > std::numeric_limits<std::uint64_t>::max() - instanceWorkspace)
			{
				AE_UNEXPECTED(AetherError::Asset("Streaming snapshot workspace is too large"));
			}
			instanceWorkspace += cost;
		}
		if (!reservation.Add(instanceWorkspace))
		{
			AE_UNEXPECTED(AetherError::Asset("CPU budget blocks snapshot workspace"));
		}
		std::vector<StreamingScriptInstance> instances;
		for (const auto& instance: m_instances)
		{
			const auto* owner = m_world.TryGet<scene::StreamingRegionOwner>(instance.entity);
			if (owner && owner->region == region.id && !ecs::HasDontDestroyOnLoadAncestor(m_world, instance.entity))
			{
				instances.push_back({instance.entity, instance.handle, instance.type, instance.scriptIndex});
			}
		}
		if (instances.size() > (std::numeric_limits<std::uint64_t>::max() - kStreamingScriptCaptureScratch) / kStreamingScriptCaptureScratch || !reservation.Add((static_cast<std::uint64_t>(instances.size()) + 1) * kStreamingScriptCaptureScratch))
		{
			AE_UNEXPECTED(AetherError::Asset("CPU budget blocks snapshot workspace"));
		}
		std::vector<std::string> capturedStates;
		capturedStates.reserve(instances.size());
		std::uint64_t capturedBytes = 0;
		for (const auto& instance: instances)
		{
			struct ActorScope
			{
				Entity& active;
				Entity previous;

				ActorScope(Entity& slot, Entity entity)
				      : active(slot), previous(slot)
				{
					active = entity;
				}

				~ActorScope()
				{
					active = previous;
				}
			} actorScope(m_callback_entity, instance.entity);
#if defined(AE_WEB_MANAGED)
			const scripting::ActiveScriptEntityScope managedActor(ManagedBrowserSceneContext(), instance.entity);
#endif
			AE_TRY(state, m_host.CaptureStreamingState(instance.handle));
			if (state->size() > 16ull * 1024 * 1024 - capturedBytes)
			{
				AE_UNEXPECTED(AetherError::Asset("Streaming script hook state exceeds the capture limit"));
			}
			capturedBytes += state->size();
			capturedStates.emplace_back(state->data(), state->size());
		}
		AE_TRY(scriptBudget, MeasureStreamingScriptCapture(m_host, instances));
		if (!reservation.Add(scriptBudget->workspace))
		{
			AE_UNEXPECTED(AetherError::Asset("CPU budget blocks snapshot workspace"));
		}
		AE_TRY(scripts, CaptureStreamingScripts(m_world, m_host, instances, capturedStates, scriptBudget->maxPropertyBytes));
		AE_TRY(sceneBudget, m_scene->MeasureRegionSnapshotStorage(region));
		AE_TRY(sceneWorkspace, ComputeRegionSnapshotWorkspace(*sceneBudget, scripts->size(), sizeof(Velocity)));
		if (!reservation.Add(*sceneWorkspace))
		{
			AE_UNEXPECTED(AetherError::Asset("CPU budget blocks snapshot workspace"));
		}
		AE_TRY(description, m_scene->CaptureRegionDocument(region));
		if (!region.activationGate.IsValid() || !m_world.GetRegistry().valid(World::ToEntt(region.activationGate)))
		{
			AE_UNEXPECTED(AetherError::Asset("Browser streaming activation gate is missing"));
		}
		const auto* gate = m_world.TryGet<SceneNodeComponent>(region.activationGate);
		if (!gate || !gate->id || std::count_if(description->entities.begin(), description->entities.end(), [gate](const auto& record) { return record.nodeId == gate->id; }) != 1)
		{
			AE_UNEXPECTED(AetherError::Asset("Browser streaming activation gate has no unique snapshot node"));
		}
		const std::uint64_t gateNode = gate->id;
		if (!ValidGateHierarchy(*description, gateNode))
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid browser streaming activation hierarchy"));
		}
		const auto document = scene::WriteToml(*description);
		std::vector<Velocity> velocities;
		auto* physics2d = static_cast<Physics2DSystem*>(m_world.FindSystem("Physics2DSystem"));
		for (auto raw: m_world.GetRegistry().view<scene::StreamingRegionOwner>())
		{
			const Entity entity = World::FromEntt(raw);
			const auto& owner = m_world.Get<scene::StreamingRegionOwner>(entity);
			if (owner.region != region.id || ecs::HasDontDestroyOnLoadAncestor(m_world, entity))
			{
				continue;
			}
			const auto* node = m_world.TryGet<SceneNodeComponent>(entity);
			if (!node || !node->id)
			{
				AE_UNEXPECTED(AetherError::Asset("Streaming entity has no stable node"));
			}
			Velocity value;
			value.node = node->id;
			if (const auto* body = m_world.TryGet<RigidBodyComponent>(entity))
			{
				value.flags |= 1;
				const auto linear = body->body.IsValid() ? m_scene->Physics().GetLinearVelocity(body->body) : body->initialVelocity;
				const auto angular = body->body.IsValid() ? m_scene->Physics().GetAngularVelocity(body->body) : body->initialAngularVelocity;
				for (int i = 0; i < 3; ++i)
				{
					value.linear3[i] = linear[i];
					value.angular3[i] = angular[i];
				}
			}
			if (const auto* body = m_world.TryGet<RigidBody2DComponent>(entity))
			{
				if (!physics2d)
				{
					AE_UNEXPECTED(AetherError::Asset("Streaming 2D physics unavailable"));
				}
				value.flags |= 2;
				const auto linear = body->body.IsValid() ? physics2d->GetLinearVelocity(body->body) : body->initialVelocity;
				value.linear2[0] = linear.x;
				value.linear2[1] = linear.y;
				value.angular2 = body->body.IsValid() ? physics2d->GetAngularVelocity(body->body) : body->initialAngularVelocity;
			}
			if (const auto* animator = m_world.TryGet<SpriteAnimatorComponent>(entity))
			{
				value.flags |= 4;
				value.frameTime = animator->frameTime;
				value.fixedAccumulator = animator->fixedAccumulator;
				value.currentFrame = animator->currentFrame;
				value.direction = animator->direction;
				value.playing = animator->playing ? 1 : 0;
				value.initialized = animator->initialized ? 1 : 0;
				if (!std::isfinite(value.frameTime) || !std::isfinite(value.fixedAccumulator) || (value.direction != -1 && value.direction != 1))
				{
					AE_UNEXPECTED(AetherError::Asset("Invalid streaming sprite animation state"));
				}
			}
			if (value.flags)
			{
				velocities.push_back(value);
			}
		}
		if (document.size() > kStreamingSnapshotInputLimit || scripts->size() > kStreamingScriptSnapshotLimit || velocities.size() > 65536
		        || document.size() + scripts->size() + velocities.size() * sizeof(Velocity) + 4 * sizeof(std::uint32_t) + sizeof(gateNode) > kStreamingSnapshotInputLimit)
		{
			AE_UNEXPECTED(AetherError::Asset("Streaming snapshot too large"));
		}
		std::string out;
		const auto docSize = static_cast<std::uint32_t>(document.size()), scriptSize = static_cast<std::uint32_t>(scripts->size()), count = static_cast<std::uint32_t>(velocities.size());
		Append(out, kSnapshotMagic);
		Append(out, docSize);
		Append(out, scriptSize);
		Append(out, count);
		Append(out, gateNode);
		out.append(document);
		out.append(*scripts);
		for (const auto& velocity: velocities)
		{
			Append(out, velocity);
		}
		if (!reservation.Add(out.size()))
		{
			AE_UNEXPECTED(AetherError::Asset("CPU budget blocks snapshot workspace"));
		}
		const auto retained = static_cast<std::uint64_t>(out.capacity()) + 1 + out.size();
		if (retained > reservation.bytes)
		{
			AE_UNEXPECTED(AetherError::Asset("Streaming snapshot workspace exceeded reservation"));
		}
		reservation.bytes -= retained;
		return out;
	}

	Expected<void> WebGameplay::RestoreRegion(WebRegion& region, std::string_view data)
	{
		std::string_view document, scripts, velocities;
		std::uint64_t gateNode = 0;
		if (!Sections(data, document, scripts, velocities, gateNode))
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid browser streaming snapshot"));
		}
		std::unordered_map<std::uint64_t, Entity> nodes;
		for (const auto entity: region.entities)
		{
			if (const auto* node = m_world.TryGet<SceneNodeComponent>(entity); node && node->id)
			{
				nodes.emplace(node->id, entity);
			}
		}
		const auto gate = nodes.find(gateNode);
		if (gate == nodes.end() || !m_world.Has<DisabledComponent>(gate->second))
		{
			AE_UNEXPECTED(AetherError::Asset("Restored browser streaming activation gate is missing or enabled"));
		}
		region.activationGate = gate->second;
		auto* physics2d = static_cast<Physics2DSystem*>(m_world.FindSystem("Physics2DSystem"));
		while (!velocities.empty())
		{
			Velocity value;
			if (!Take(velocities, value) || !nodes.contains(value.node))
			{
				AE_UNEXPECTED(AetherError::Asset("Invalid streaming physics snapshot"));
			}
			if (!value.flags || (value.flags & ~7u) || value.playing > 1 || value.initialized > 1
			        || ((value.flags & 4) && (!std::isfinite(value.frameTime) || !std::isfinite(value.fixedAccumulator) || (value.direction != -1 && value.direction != 1))))
			{
				AE_UNEXPECTED(AetherError::Asset("Invalid streaming runtime snapshot"));
			}
			const auto entity = nodes.at(value.node);
			if (value.flags & 1)
			{
				auto* body = m_world.TryGet<RigidBodyComponent>(entity);
				if (!body)
				{
					AE_UNEXPECTED(AetherError::Asset("Missing streaming 3D body"));
				}
				const glm::vec3 linear{value.linear3[0], value.linear3[1], value.linear3[2]};
				const glm::vec3 angular{value.angular3[0], value.angular3[1], value.angular3[2]};
				if (body->body.IsValid())
				{
					m_scene->Physics().SetLinearVelocity(body->body, linear);
					m_scene->Physics().SetAngularVelocity(body->body, angular);
				}
				else
				{
					body->initialVelocity = linear;
					body->initialAngularVelocity = angular;
				}
			}
			if (value.flags & 2)
			{
				auto* body = m_world.TryGet<RigidBody2DComponent>(entity);
				if (!physics2d || !body)
				{
					AE_UNEXPECTED(AetherError::Asset("Missing streaming 2D body"));
				}
				const glm::vec2 linear{value.linear2[0], value.linear2[1]};
				if (body->body.IsValid())
				{
					physics2d->SetLinearVelocity(body->body, linear);
					physics2d->SetAngularVelocity(body->body, value.angular2);
				}
				else
				{
					body->initialVelocity = linear;
					body->initialAngularVelocity = value.angular2;
				}
			}
			if (value.flags & 4)
			{
				auto* animator = m_world.TryGet<SpriteAnimatorComponent>(entity);
				if (!animator)
				{
					AE_UNEXPECTED(AetherError::Asset("Missing streaming sprite animator"));
				}
				animator->frameTime = value.frameTime;
				animator->fixedAccumulator = value.fixedAccumulator;
				animator->currentFrame = value.currentFrame;
				animator->direction = value.direction;
				animator->playing = value.playing != 0;
				animator->initialized = value.initialized != 0;
			}
		}
		return {};
	}

	Expected<bool> WebGameplay::StepRegionScriptRestore(WebRegion& region, std::string_view data, std::size_t& remaining)
	{
		auto found = m_script_restores.find(region.id);
		if (found == m_script_restores.end())
		{
			std::string_view document, scripts, velocities;
			std::uint64_t gateNode = 0;
			if (!Sections(data, document, scripts, velocities, gateNode))
			{
				AE_UNEXPECTED(AetherError::Asset("Invalid browser streaming snapshot"));
			}
			const auto* gate = m_world.TryGet<SceneNodeComponent>(region.activationGate);
			if (!gate || gate->id != gateNode || !m_world.Has<DisabledComponent>(region.activationGate))
			{
				AE_UNEXPECTED(AetherError::Asset("Browser streaming activation gate is not ready for script restore"));
			}
			std::vector<StreamingScriptInstance> instances;
			for (const auto& instance: m_instances)
			{
				const auto* owner = m_world.TryGet<scene::StreamingRegionOwner>(instance.entity);
				if (owner && owner->region == region.id)
				{
					instances.push_back({instance.entity, instance.handle, instance.type, instance.scriptIndex});
				}
			}
			auto prepared = PrepareStreamingScriptRestore(m_world, m_host, instances, scripts);
			if (!prepared)
			{
				return std::unexpected(prepared.error());
			}
			found = m_script_restores.emplace(region.id, std::make_unique<StreamingScriptRestoreCursor>(std::move(*prepared))).first;
		}
		auto& cursor = *found->second;
		while (cursor.next < cursor.scripts.size() && remaining)
		{
			const auto entity = cursor.scripts[cursor.next].entity;
			if (!m_world.GetRegistry().valid(World::ToEntt(entity)))
			{
				AE_UNEXPECTED(AetherError::Asset("Streaming script entity disappeared during restore"));
			}

			struct ActorScope
			{
				Entity& active;
				Entity previous;

				~ActorScope()
				{
					active = previous;
				}
			} actor{m_callback_entity, m_callback_entity};

			m_callback_entity = entity;
#if defined(AE_WEB_MANAGED)
			const scripting::ActiveScriptEntityScope managedActor(ManagedBrowserSceneContext(), entity);
#endif
			auto result = StepStreamingScriptRestore(cursor, m_host, 1);
			if (!result)
			{
				return std::unexpected(result.error());
			}
			--remaining;
		}
		if (cursor.next != cursor.scripts.size())
		{
			return false;
		}
		m_script_restores.erase(found);
		return true;
	}
} // namespace aether::app::web

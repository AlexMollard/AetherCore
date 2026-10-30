#include "scene/WorldStreaming.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "assets/AssetManager.hpp"
#include "assets/AssetStreaming.hpp"
#include "io/FileSystem.hpp"
#include "scene/Hierarchy.hpp"
#include "physics/PhysicsSystem.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "scene/NativeStreamingPreparation.hpp"
#include "scene/NativeStreamingSnapshot.hpp"
#include "scene/SceneStreamingDependencies.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "systems/ScriptComponentSystem.hpp"

namespace aether::app::scene
{
	namespace
	{
		class NativeWorldStreaming final : public IWorldStreamingBackend
		{
			struct Region
			{
				streaming::Region definition;
				streaming::Demand demand = streaming::Demand::None;
				io::IOPriority priority = io::IOPriority::Normal;
				streaming::State state = streaming::State::Unknown;
				std::string error;
				std::shared_ptr<std::atomic_bool> cancelled;
				std::shared_ptr<NativeStreamingPreparation::Job> job;
				std::shared_ptr<NativeStreamingSnapshot::LoadTicket> loadTicket;
				bool rollbackPending = false;
				std::optional<NativePreparedRegion> prepared;
				std::shared_ptr<NativeStreamingSnapshot::Loaded> snapshot;
				std::vector<streaming::RequestId> claims;
				std::unordered_set<std::string> claimedReferences;
				std::size_t nextDependency = 0;
				std::vector<Entity> entities;
				std::shared_ptr<NativeStreamingSnapshot::SaveTicket> saveTicket;
				std::vector<Entity> saveEntities;
				std::vector<Entity> saveRoots;
				std::vector<Entity> temporarilyDisabled;
				std::vector<Entity> awake3D;
				std::vector<Entity> awake2D;
				std::vector<NativeStreamingSnapshot::Velocity> frozenVelocities;
				std::size_t applied = 0;
				bool activationStarted = false;
				bool unloadBlocked = false;
			};

			struct Promoted
			{
				std::vector<Entity> entities;
				std::vector<streaming::RequestId> claims;
				io::IOPriority priority = io::IOPriority::Normal;
				bool needsRefinement = false;
				std::string error;
			};

		public:
			NativeWorldStreaming(scripting::SceneContext& context, const ApplySceneDeps& deps)
			      : m_context(context), m_deps(deps)
			{
				if (m_context.assets != nullptr)
				{
					m_snapshot = std::make_unique<NativeStreamingSnapshot>(m_context.assets->GetStreaming().GetBudget());
				}
			}

			~NativeWorldStreaming() override
			{
				Shutdown();
			}

			Expected<void> SetBudgets(const streaming::Budgets& budgets) override
			{
				if (m_context.assets == nullptr || budgets.activationEntitiesPerFrame == 0)
				{
					return std::unexpected(AetherError::Engine("Native streaming requires assets and a nonzero activation budget"));
				}
				AE_TRY_VOID(m_context.assets->GetStreaming().SetBudgets(budgets));
				m_budgets = budgets;
				return {};
			}

			Expected<void> SetDemand(const streaming::Region& definition, streaming::Demand demand, io::IOPriority priority) override
			{
				if (m_stopped || m_context.assets == nullptr || m_context.world == nullptr)
				{
					return std::unexpected(AetherError::Engine("Native streaming backend is unavailable"));
				}
				auto& ptr = m_regions[definition.id];
				if (!ptr)
				{
					ptr = std::make_unique<Region>();
				}
				Region& region = *ptr;
				if (!region.definition.scene.empty() && region.definition.scene != definition.scene && (region.activationStarted || region.saveTicket || region.rollbackPending))
				{
					return std::unexpected(AetherError::Engine("Cannot replace an active streaming region"));
				}
				const bool changed = region.demand != demand || region.definition.scene != definition.scene;
				if (region.job && region.priority != priority)
				{
					region.cancelled->store(true);
					region.job.reset();
					region.state = streaming::State::Unknown;
				}
				if (region.definition.scene != definition.scene && region.state != streaming::State::Unknown)
				{
					Discard(region);
				}
				region.definition = definition;
				region.demand = demand;
				region.priority = priority;
				auto& assets = m_context.assets->GetStreaming();
				for (auto claim: region.claims)
				{
					assets.SetPriority(claim, priority);
					assets.SetRequired(claim, demand == streaming::Demand::Active || region.activationStarted || region.saveTicket || region.rollbackPending);
				}
				if (changed && (region.state == streaming::State::Failed || region.state == streaming::State::Blocked))
				{
					region.error.clear();
					region.unloadBlocked = false;
					if (region.rollbackPending)
					{
						region.state = streaming::State::Loading;
					}
					else if (region.activationStarted)
					{
						region.state = streaming::State::Active;
					}
					else
					{
						Discard(region);
					}
				}
				return {};
			}

			void Update() override
			{
				if (m_stopped || m_context.assets == nullptr || m_context.world == nullptr)
				{
					return;
				}
				auto& assets = m_context.assets->GetStreaming();
				assets.Update();
				std::erase_if(m_promoted,
				        [&](Promoted& promoted)
				        {
					        auto& registry = m_context.world->GetRegistry();
					        std::erase_if(promoted.entities, [&](Entity entity) { return !registry.valid(World::ToEntt(entity)); });
					        if (!promoted.entities.empty())
					        {
						        if (promoted.needsRefinement)
						        {
							        auto claims = AcquirePromoted(promoted.entities, promoted.priority);
							        if (claims)
							        {
								        for (const auto old: promoted.claims)
								        {
									        assets.Release(old);
								        }
								        promoted.claims = std::move(*claims);
								        promoted.needsRefinement = false;
								        promoted.error.clear();
							        }
							        else
							        {
								        promoted.error = claims.error().message;
							        }
						        }
						        return false;
					        }
					        for (const auto claim: promoted.claims)
					        {
						        assets.Release(claim);
					        }
					        return true;
				        });
				std::uint32_t remaining = m_budgets.activationEntitiesPerFrame;
				m_order.clear();
				for (auto& [id, ptr]: m_regions)
				{
					m_order.push_back(ptr.get());
				}
				std::stable_sort(m_order.begin(), m_order.end(), [](const Region* a, const Region* b) { return static_cast<unsigned>(a->priority) < static_cast<unsigned>(b->priority); });
				for (Region* ptr: m_order)
				{
					Region& region = *ptr;
					if (region.saveTicket)
					{
						if (region.demand == streaming::Demand::Active)
						{
							region.saveTicket->Cancel();
							region.saveTicket.reset();
							region.rollbackPending = true;
							region.state = streaming::State::Loading;
						}
						else
						{
							auto saved = region.saveTicket->Poll();
							if (!saved)
							{
								continue;
							}
							region.saveTicket.reset();
							if (*saved)
							{
								Discard(region);
								continue;
							}
							region.error = "Snapshot write failed: " + saved->error().message;
							region.rollbackPending = true;
							region.state = streaming::State::Loading;
						}
					}
					if (region.rollbackPending)
					{
						if (region.state == streaming::State::Failed)
						{
							continue;
						}
						if (remaining > 0)
						{
							Activate(region, remaining);
						}
						if (region.state == streaming::State::Active)
						{
							region.rollbackPending = false;
							if (!region.error.empty() && region.demand != streaming::Demand::Active)
							{
								region.state = streaming::State::Failed;
							}
						}
						continue;
					}
					if (region.demand == streaming::Demand::None)
					{
						if (region.state == streaming::State::Active || region.state == streaming::State::Saving || region.unloadBlocked)
						{
							Unload(region);
						}
						else if (region.state != streaming::State::Failed || !region.activationStarted)
						{
							Discard(region);
						}
						continue;
					}
					if (region.state == streaming::State::Unknown || region.state == streaming::State::Cancelled || (region.state == streaming::State::Blocked && !region.prepared && !region.job && !region.loadTicket))
					{
						Start(region);
					}
					if (region.job)
					{
						auto result = region.job->Take();
						if (!result)
						{
							continue;
						}
						region.job.reset();
						if (!result->error.empty() || !result->scene)
						{
							Fail(region, result->error.empty() ? "Region preparation failed" : result->error);
							continue;
						}
						region.prepared.emplace(std::move(*result));
						region.loadTicket = m_snapshot->LoadAsync(region.definition.id, region.priority);
						if (!region.loadTicket)
						{
							Fail(region, "Snapshot load admission failed");
							continue;
						}
						region.state = streaming::State::Loading;
						continue;
					}
					if (region.loadTicket)
					{
						auto loaded = region.loadTicket->Take();
						if (!loaded)
						{
							continue;
						}
						region.loadTicket.reset();
						if (!*loaded)
						{
							Fail(region, loaded->error().message);
							continue;
						}
						if (**loaded)
						{
							region.snapshot = std::make_shared<NativeStreamingSnapshot::Loaded>(std::move(***loaded));
						}
						auto& dependencies = region.prepared->dependencies;
						if (region.snapshot)
						{
							auto extracted = CollectSceneStreamingDependencies(region.snapshot->scene, dependencies, ResolveNativeSceneStreamingDependency);
							if (!extracted)
							{
								Fail(region, extracted.error());
								continue;
							}
						}
						std::sort(dependencies.begin(), dependencies.end());
						dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
						region.state = streaming::State::Loading;
					}
					if (!region.prepared || region.state == streaming::State::Failed)
					{
						continue;
					}
					auto& dependencies = region.prepared->dependencies;
					while (region.nextDependency < dependencies.size())
					{
						const auto& dependency = dependencies[region.nextDependency];
						if (!region.claimedReferences.contains(dependency))
						{
							const auto claim = assets.Request(dependency, region.priority, region.demand == streaming::Demand::Active);
							if (!claim)
							{
								break;
							}
							region.claims.push_back(claim);
							region.claimedReferences.insert(dependency);
						}
						++region.nextDependency;
					}
					bool ready = region.nextDependency == dependencies.size();
					auto waitingState = ready ? streaming::State::Loading : streaming::State::Blocked;
					for (auto claim: region.claims)
					{
						const auto state = assets.GetRequestState(claim);
						if (state == streaming::State::Failed || state == streaming::State::Cancelled)
						{
							Fail(region, "A region dependency failed or was cancelled");
							ready = false;
							break;
						}
						if (state == streaming::State::Blocked)
						{
							waitingState = streaming::State::Blocked;
						}
						else if (state == streaming::State::Uploading && waitingState != streaming::State::Blocked)
						{
							waitingState = streaming::State::Uploading;
						}
						if (state != streaming::State::Loaded && state != streaming::State::Active)
						{
							ready = false;
						}
					}
					if (!ready)
					{
						if (region.state != streaming::State::Failed)
						{
							if (waitingState == streaming::State::Blocked)
							{
								region.error = "A region dependency is blocked by the streaming budget";
							}
							else if (region.state == streaming::State::Blocked)
							{
								region.error.clear();
							}
							region.state = waitingState;
						}
						continue;
					}
					if (region.state == streaming::State::Blocked)
					{
						region.error.clear();
					}
					if (region.demand == streaming::Demand::Prefetch && !region.activationStarted)
					{
						region.state = streaming::State::Loaded;
						continue;
					}
					if (region.demand == streaming::Demand::Prefetch && region.activationStarted)
					{
						if (region.state == streaming::State::Active || region.unloadBlocked)
						{
							Unload(region);
						}
						else if (auto aborted = AbortActivation(region); !aborted)
						{
							Fail(region, aborted.error().message);
						}
						continue;
					}
					if (region.state != streaming::State::Active && remaining > 0)
					{
						Activate(region, remaining);
					}
				}
				if (m_snapshot)
				{
					auto resolved = m_snapshot->ResolveDeferredReferences(m_context);
					m_referenceError = resolved ? std::string{} : resolved.error().message;
				}
			}

			streaming::State GetState(std::string_view id) const override
			{
				const auto it = m_regions.find(std::string(id));
				return it == m_regions.end() ? streaming::State::Unknown : it->second->state;
			}

			std::string GetDiagnosticsJson() const override
			{
				nlohmann::json rows = nlohmann::json::array();
				for (const auto& [id, ptr]: m_regions)
				{
					rows.push_back({{"id", id},
					        {"scene", ptr->definition.scene},
					        {"state", static_cast<unsigned>(ptr->state)},
					        {"demand", static_cast<unsigned>(ptr->demand)},
					        {"priority", static_cast<unsigned>(ptr->priority)},
					        {"claims", ptr->claims.size()},
					        {"entities", ptr->entities.size()},
					        {"error", ptr->error}});
				}
				nlohmann::json promoted = nlohmann::json::array();
				for (const auto& item: m_promoted)
				{
					promoted.push_back({{"entities", item.entities.size()}, {"claims", item.claims.size()}, {"error", item.error}});
				}
				nlohmann::json result = {{"regions", std::move(rows)}, {"promoted", std::move(promoted)}, {"referenceError", m_referenceError}};
				if (m_context.assets != nullptr)
				{
					result["assets"] = nlohmann::json::parse(m_context.assets->GetStreaming().GetDiagnosticsJson(), nullptr, false);
				}
				return result.dump();
			}

			void ResetWorld() override
			{
				for (auto& [id, region]: m_regions)
				{
					if (m_context.world != nullptr)
					{
						std::vector<Entity> promoted;
						for (const auto entity: Owned(*region))
						{
							if (ecs::HasDontDestroyOnLoadAncestor(*m_context.world, entity))
							{
								promoted.push_back(entity);
							}
						}
						if (!promoted.empty())
						{
							auto claims = AcquirePromoted(promoted, region->priority);
							if (claims)
							{
								m_promoted.push_back({promoted, std::move(*claims), region->priority});
							}
							else
							{
								m_promoted.push_back({promoted, std::move(region->claims), region->priority, true, claims.error().message});
								region->claims.clear();
							}
							for (const auto entity: promoted)
							{
								m_context.world->Remove<StreamingRegionOwner>(entity);
							}
						}
					}
					if (region->cancelled)
					{
						region->cancelled->store(true);
					}
					if (region->saveTicket)
					{
						region->saveTicket->Cancel();
					}
					if (region->activationStarted && region->prepared)
					{
						CancelStreamingRegionBatch(Document(*region));
					}
					if (m_context.assets)
					{
						for (const auto claim: region->claims)
						{
							m_context.assets->GetStreaming().Release(claim);
						}
					}
				}
				m_regions.clear();
				if (m_snapshot)
				{
					m_snapshot->Reset();
				}
			}

			void Shutdown() override
			{
				if (m_stopped)
				{
					return;
				}
				ResetWorld();
				// Jobs hold the asset service's ledger. Drain before the service can be destroyed.
				if (io::FileSystem::IsInitialized())
				{
					io::FileSystem::Flush();
				}
				if (m_context.assets != nullptr)
				{
					for (const auto& promoted: m_promoted)
					{
						for (const auto claim: promoted.claims)
						{
							m_context.assets->GetStreaming().Release(claim);
						}
					}
				}
				m_promoted.clear();
				m_stopped = true;
			}

		private:
			const SceneDescription& Document(const Region& region) const
			{
				return region.snapshot ? region.snapshot->scene : *region.prepared->scene;
			}

			Expected<std::vector<streaming::RequestId>> AcquirePromoted(const std::vector<Entity>& entities, io::IOPriority priority)
			{
				auto& world = *m_context.world;
				std::unordered_set<std::uint32_t> ids;
				for (const auto entity: entities)
				{
					ids.insert(entity.id);
				}
				std::vector<Entity> roots;
				for (const auto entity: entities)
				{
					const auto* hierarchy = world.TryGet<HierarchyComponent>(entity);
					if (!hierarchy || !hierarchy->parent.IsValid() || !ids.contains(hierarchy->parent.id))
					{
						roots.push_back(entity);
					}
				}
				const auto captured = CaptureSubtrees(world, roots, m_context.assets->GetMaterialRegistry(), m_context.assets->GetTextureRegistry());
				std::vector<std::string> dependencies;
				const auto extracted = CollectSceneStreamingDependencies(captured, dependencies, ResolveNativeSceneStreamingDependency);
				if (!extracted)
				{
					return std::unexpected(AetherError::Engine(extracted.error()));
				}
				std::sort(dependencies.begin(), dependencies.end());
				dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
				std::vector<streaming::RequestId> claims;
				auto& assets = m_context.assets->GetStreaming();
				for (const auto& reference: dependencies)
				{
					const auto claim = assets.Request(reference, priority, true);
					if (!claim)
					{
						for (const auto prior: claims)
						{
							assets.Release(prior);
						}
						return std::unexpected(AetherError::Engine("Promoted asset request admission failed"));
					}
					claims.push_back(claim);
				}
				return claims;
			}

			void Start(Region& region)
			{
				region.cancelled = std::make_shared<std::atomic_bool>(false);
				region.job = m_preparation.Submit(region.definition.scene, region.definition.id, ++m_nextRequest, region.priority, region.cancelled, m_context.assets->GetStreaming().GetBudget());
				if (!region.job)
				{
					region.state = streaming::State::Blocked;
					region.error = "Region preparation admission is full";
					return;
				}
				region.error.clear();
				region.state = streaming::State::Queued;
			}

			void Fail(Region& region, std::string error)
			{
				region.error = std::move(error);
				region.state = streaming::State::Failed;
			}

			std::vector<Entity> Owned(const Region& region) const
			{
				std::vector<Entity> result;
				m_context.world->View<StreamingRegionOwner>().each(
				        [&](entt::entity e, const StreamingRegionOwner& owner)
				        {
					        if (owner.region == region.definition.id)
					        {
						        result.push_back(World::FromEntt(e));
					        }
				        });
				return result;
			}

			void DetachForeignChildren(const std::vector<Entity>& entities, const std::unordered_set<std::uint32_t>& ids)
			{
				auto& world = *m_context.world;
				for (const Entity entity: entities)
				{
					const auto* hierarchy = world.TryGet<HierarchyComponent>(entity);
					if (!hierarchy)
					{
						continue;
					}
					const auto children = hierarchy->children;
					for (const Entity child: children)
					{
						if (!ids.contains(child.id))
						{
							ecs::DetachFromParent(world, child);
						}
					}
				}
			}

			void PruneSceneEntities()
			{
				auto& registry = m_context.world->GetRegistry();
				std::erase_if(m_context.sceneEntities, [&](Entity entity) { return !registry.valid(World::ToEntt(entity)); });
			}

			void Activate(Region& region, std::uint32_t& remaining)
			{
				auto& world = *m_context.world;
				const auto& document = Document(region);
				if (!region.activationStarted)
				{
					std::unordered_set<std::uint64_t> live;
					world.View<SceneNodeComponent>().each(
					        [&](entt::entity, const SceneNodeComponent& node)
					        {
						        if (node.id)
						        {
							        live.insert(node.id);
						        }
					        });
					for (const auto& record: document.entities)
					{
						if (record.nodeId && !live.insert(record.nodeId).second)
						{
							Fail(region, "Streaming scene node identity collides with a live node");
							return;
						}
					}
				}
				region.activationStarted = true;
				region.state = streaming::State::Loading;
				while (region.entities.size() < document.entities.size() && remaining > 0)
				{
					const Entity entity = world.Create();
					world.Emplace<StreamingRegionOwner>(entity, StreamingRegionOwner{region.definition.id});
					world.Emplace<SceneTransientComponent>(entity);
					world.Emplace<IncludedFromComponent>(entity, IncludedFromComponent{region.definition.id});
					world.Emplace<DisabledComponent>(entity);
					region.entities.push_back(entity);
					--remaining;
				}
				if (region.entities.size() != document.entities.size())
				{
					return;
				}
				const std::size_t end = std::min(document.entities.size(), region.applied + remaining);
				if (end == region.applied && !document.entities.empty())
				{
					return;
				}
				const bool finalBatch = end == document.entities.size();
				auto status = ApplyStreamingRegionBatch(document, world, m_deps, region.entities, region.applied, end, finalBatch);
				if (!status)
				{
					const auto error = status.error().message;
					auto aborted = AbortActivation(region);
					Fail(region, aborted ? error : error + "; detach failed: " + aborted.error().message);
					return;
				}
				remaining -= static_cast<std::uint32_t>(end - region.applied);
				region.applied = end;
				if (!finalBatch)
				{
					return;
				}
				if (region.snapshot)
				{
					auto restored = m_snapshot->Restore(m_context, region.entities, *region.snapshot);
					if (!restored)
					{
						const auto error = restored.error().message;
						auto aborted = AbortActivation(region);
						Fail(region, aborted ? error : error + "; detach failed: " + aborted.error().message);
						return;
					}
				}
				for (std::size_t i = 0; i < region.entities.size(); ++i)
				{
					if (!document.entities[i].disabled)
					{
						world.Remove<DisabledComponent>(region.entities[i]);
					}
				}
				region.snapshot.reset();
				region.state = streaming::State::Active;
			}

			Expected<void> AbortActivation(Region& region)
			{
				if (region.prepared && region.activationStarted)
				{
					CancelStreamingRegionBatch(Document(region));
				}
				if (m_context.world != nullptr)
				{
					auto& world = *m_context.world;
					auto entities = Owned(region);
					std::unordered_set<std::uint32_t> ids;
					for (const auto entity: entities)
					{
						ids.insert(entity.id);
					}
					DetachForeignChildren(entities, ids);
					if (auto* scripts = static_cast<ScriptComponentSystem*>(world.FindSystem("ScriptComponentSystem")))
					{
						auto detached = scripts->DetachStreamingEntities(entities);
						if (!detached)
						{
							return std::unexpected(std::move(detached.error()));
						}
					}
					if (m_snapshot)
					{
						m_snapshot->ForgetDeferredReferences(world, entities);
					}
					for (const auto entity: entities)
					{
						if (!world.GetRegistry().valid(World::ToEntt(entity)))
						{
							continue;
						}
						if (m_context.physics && world.Has<RigidBodyComponent>(entity))
						{
							m_context.physics->RemoveBody(world, entity);
						}
						if (m_context.physics2D && world.Has<RigidBody2DComponent>(entity))
						{
							m_context.physics2D->RemoveBody(world, entity);
						}
					}
					for (const auto entity: entities)
					{
						if (world.GetRegistry().valid(World::ToEntt(entity)))
						{
							world.Destroy(entity);
						}
					}
					PruneSceneEntities();
				}
				region.entities.clear();
				region.applied = 0;
				region.activationStarted = false;
				region.state = streaming::State::Loaded;
				return {};
			}

			void Thaw(Region& region)
			{
				if (!m_context.world)
				{
					return;
				}
				auto& world = *m_context.world;
				for (const auto entity: region.saveEntities)
				{
					if (!world.GetRegistry().valid(World::ToEntt(entity)))
					{
						continue;
					}
					const auto* node = world.TryGet<SceneNodeComponent>(entity);
					if (!node)
					{
						continue;
					}
					for (const auto& velocity: region.frozenVelocities)
					{
						if (velocity.node != node->id)
						{
							continue;
						}
						if (velocity.dimensions == 3 && m_context.physics)
						{
							if (const auto* body = world.TryGet<RigidBodyComponent>(entity); body && body->body.IsValid())
							{
								m_context.physics->SetLinearVelocity(body->body, velocity.linear);
								m_context.physics->SetAngularVelocity(body->body, velocity.angular);
							}
						}
						else if (velocity.dimensions == 2 && m_context.physics2D)
						{
							if (const auto* body = world.TryGet<RigidBody2DComponent>(entity); body && body->body.IsValid())
							{
								m_context.physics2D->SetLinearVelocity(body->body, {velocity.linear.x, velocity.linear.y});
								m_context.physics2D->SetAngularVelocity(body->body, velocity.angular.x);
							}
						}
					}
				}
				for (const auto entity: region.temporarilyDisabled)
				{
					if (world.GetRegistry().valid(World::ToEntt(entity)))
					{
						world.Remove<DisabledComponent>(entity);
					}
				}
				for (const auto entity: region.awake3D)
				{
					if (world.GetRegistry().valid(World::ToEntt(entity)))
					{
						if (const auto* body = world.TryGet<RigidBodyComponent>(entity); body && body->body.IsValid() && m_context.physics)
						{
							m_context.physics->SetBodyActive(body->body, true);
						}
					}
				}
				for (const auto entity: region.awake2D)
				{
					if (world.GetRegistry().valid(World::ToEntt(entity)))
					{
						if (const auto* body = world.TryGet<RigidBody2DComponent>(entity); body && body->body.IsValid() && m_context.physics2D)
						{
							m_context.physics2D->SetBodyAwake(body->body, true);
						}
					}
				}
				region.temporarilyDisabled.clear();
				region.awake3D.clear();
				region.awake2D.clear();
				region.frozenVelocities.clear();
			}

			void Freeze(Region& region)
			{
				auto& world = *m_context.world;
				for (const auto entity: region.saveEntities)
				{
					auto* node = world.TryGet<SceneNodeComponent>(entity);
					if (!node)
					{
						node = &world.Emplace<SceneNodeComponent>(entity, SceneNodeComponent{GenerateSceneNodeId()});
					}
					if (!node->id)
					{
						node->id = GenerateSceneNodeId();
					}
					if (!world.Has<DisabledComponent>(entity))
					{
						world.Emplace<DisabledComponent>(entity);
						region.temporarilyDisabled.push_back(entity);
					}
					if (const auto* body = world.TryGet<RigidBodyComponent>(entity); body && body->body.IsValid() && m_context.physics)
					{
						region.frozenVelocities.push_back({node->id, 3, m_context.physics->GetLinearVelocity(body->body), m_context.physics->GetAngularVelocity(body->body)});
						if (m_context.physics->IsBodyActive(body->body))
						{
							region.awake3D.push_back(entity);
						}
						m_context.physics->SetBodyActive(body->body, false);
					}
					if (const auto* body = world.TryGet<RigidBody2DComponent>(entity); body && body->body.IsValid() && m_context.physics2D)
					{
						const auto linear = m_context.physics2D->GetLinearVelocity(body->body);
						region.frozenVelocities.push_back({node->id, 2, {linear.x, linear.y, 0.0f}, {m_context.physics2D->GetAngularVelocity(body->body), 0.0f, 0.0f}});
						if (m_context.physics2D->IsBodyAwake(body->body))
						{
							region.awake2D.push_back(entity);
						}
						m_context.physics2D->SetBodyAwake(body->body, false);
					}
				}
			}

			void Unload(Region& region)
			{
				region.unloadBlocked = false;
				auto& world = *m_context.world;
				auto entities = Owned(region);
				std::unordered_set<std::uint32_t> ids;
				std::vector<Entity> promoted;
				for (const auto entity: entities)
				{
					if (ecs::HasDontDestroyOnLoadAncestor(world, entity))
					{
						const auto* hierarchy = world.TryGet<HierarchyComponent>(entity);
						if (hierarchy && hierarchy->parent.IsValid() && !ecs::HasDontDestroyOnLoadAncestor(world, hierarchy->parent))
						{
							ecs::DetachFromParent(world, entity);
						}
						world.Remove<StreamingRegionOwner>(entity);
						promoted.push_back(entity);
					}
					else
					{
						ids.insert(entity.id);
					}
				}
				if (!promoted.empty())
				{
					auto claims = AcquirePromoted(promoted, region.priority);
					if (claims)
					{
						m_promoted.push_back({std::move(promoted), std::move(*claims), region.priority});
					}
					else
					{
						m_promoted.push_back({std::move(promoted), std::move(region.claims), region.priority, true, claims.error().message});
						region.claims.clear();
						region.claimedReferences.clear();
						region.error = "Promoted dependency acquisition failed: " + claims.error().message;
					}
				}
				entities.erase(std::remove_if(entities.begin(), entities.end(), [&](Entity e) { return !ids.contains(e.id); }), entities.end());
				DetachForeignChildren(entities, ids);
				std::vector<Entity> roots;
				for (const auto entity: entities)
				{
					const auto* hierarchy = world.TryGet<HierarchyComponent>(entity);
					if (!hierarchy || !hierarchy->parent.IsValid() || !ids.contains(hierarchy->parent.id))
					{
						roots.push_back(entity);
					}
				}
				region.saveEntities = entities;
				region.saveRoots = roots;
				Freeze(region);
				auto attempt = m_snapshot->CaptureAndSave(m_context, m_deps, region.definition.id, roots, entities, region.temporarilyDisabled, region.frozenVelocities, region.priority);
				if (!attempt)
				{
					Thaw(region);
					Fail(region, attempt.error().message);
					return;
				}
				if (!*attempt)
				{
					Thaw(region);
					region.unloadBlocked = true;
					region.state = streaming::State::Blocked;
					region.error = "Snapshot waiting for CPU/staging capacity";
					return;
				}
				auto& saved = *attempt;
				auto ownershipChanged = [&]()
				{
					const auto current = Owned(region);
					if (current.size() != ids.size())
					{
						return true;
					}
					for (const auto entity: current)
					{
						if (!ids.contains(entity.id))
						{
							return true;
						}
					}
					return false;
				};
				if (ownershipChanged())
				{
					saved->ticket->Cancel();
					Thaw(region);
					Fail(region, "Region ownership changed during snapshot");
					return;
				}
				std::vector<std::string> dependencies;
				auto extracted = CollectSceneStreamingDependencies(saved->rollback->scene, dependencies, ResolveNativeSceneStreamingDependency);
				if (!extracted)
				{
					saved->ticket->Cancel();
					Thaw(region);
					Fail(region, extracted.error());
					return;
				}
				for (const auto& dependency: dependencies)
				{
					auto& assets = m_context.assets->GetStreaming();
					if (!region.claimedReferences.contains(dependency))
					{
						const auto claim = assets.Request(dependency, region.priority, true);
						if (!claim)
						{
							saved->ticket->Cancel();
							Thaw(region);
							Fail(region, "Rollback dependency admission failed");
							return;
						}
						region.claims.push_back(claim);
						region.claimedReferences.insert(dependency);
					}
					if (assets.GetAssetState(dependency) != streaming::State::Loaded)
					{
						saved->ticket->Cancel();
						Thaw(region);
						Fail(region, "Rollback dependency is not resident");
						return;
					}
				}
				Expected<void> detached;
				if (auto* scripts = static_cast<ScriptComponentSystem*>(world.FindSystem("ScriptComponentSystem")))
				{
					detached = scripts->DetachStreamingEntities(entities);
				}
				if (!detached || ownershipChanged())
				{
					saved->ticket->Cancel();
					region.snapshot = saved->rollback;
					auto restored = m_snapshot->Restore(m_context, entities, *saved->rollback);
					Thaw(region);
					if (!restored)
					{
						Fail(region, "Region detach recovery failed: " + restored.error().message);
						return;
					}
					Fail(region, detached ? "Region ownership changed during detach" : "Region detach failed: " + detached.error().message);
					return;
				}
				auto references = m_snapshot->PrepareForDetach(world, entities);
				if (!references)
				{
					saved->ticket->Cancel();
					region.snapshot = saved->rollback;
					auto restored = m_snapshot->Restore(m_context, entities, *saved->rollback);
					Thaw(region);
					if (!restored)
					{
						Fail(region, "Region reference recovery failed: " + restored.error().message);
						return;
					}
					Fail(region, "Region reference detachment failed: " + references.error().message);
					return;
				}
				m_snapshot->ForgetDeferredReferences(world, entities);
				for (const auto entity: entities)
				{
					if (m_context.physics != nullptr && world.Has<RigidBodyComponent>(entity))
					{
						m_context.physics->RemoveBody(world, entity);
					}
					if (m_context.physics2D != nullptr && world.Has<RigidBody2DComponent>(entity))
					{
						m_context.physics2D->RemoveBody(world, entity);
					}
				}
				for (const auto root: roots)
				{
					if (world.GetRegistry().valid(World::ToEntt(root)))
					{
						world.Destroy(root);
					}
				}
				region.temporarilyDisabled.clear();
				region.awake3D.clear();
				region.awake2D.clear();
				region.frozenVelocities.clear();
				PruneSceneEntities();
				region.entities.clear();
				region.activationStarted = false;
				region.applied = 0;
				region.snapshot = std::move(saved->rollback);
				region.saveTicket = std::move(saved->ticket);
				region.state = streaming::State::Saving;
			}

			void Discard(Region& region)
			{
				if (region.cancelled)
				{
					region.cancelled->store(true);
				}
				region.job.reset();
				region.loadTicket.reset();
				if (region.saveTicket)
				{
					region.saveTicket->Cancel();
				}
				region.saveTicket.reset();
				Thaw(region);
				if (region.activationStarted)
				{
					auto aborted = AbortActivation(region);
					if (!aborted)
					{
						Fail(region, "Region detach failed: " + aborted.error().message);
						return;
					}
				}
				if (m_context.assets != nullptr)
				{
					for (auto claim: region.claims)
					{
						m_context.assets->GetStreaming().Release(claim);
					}
				}
				region.claims.clear();
				region.claimedReferences.clear();
				region.snapshot.reset();
				region.prepared.reset();
				region.nextDependency = 0;
				region.cancelled.reset();
				region.saveEntities.clear();
				region.saveRoots.clear();
				region.rollbackPending = false;
				region.unloadBlocked = false;
				region.state = streaming::State::Unknown;
			}

			scripting::SceneContext& m_context;
			ApplySceneDeps m_deps;
			streaming::Budgets m_budgets;
			NativeStreamingPreparation m_preparation;
			std::unique_ptr<NativeStreamingSnapshot> m_snapshot;
			std::unordered_map<std::string, std::unique_ptr<Region>> m_regions;
			std::vector<Region*> m_order;
			std::vector<Promoted> m_promoted;
			std::string m_referenceError;
			streaming::RequestId m_nextRequest = 0;
			bool m_stopped = false;
		};
	} // namespace

	std::unique_ptr<IWorldStreamingBackend> CreateNativeStreamingBackend(scripting::SceneContext& context, const ApplySceneDeps& deps)
	{
		return std::make_unique<NativeWorldStreaming>(context, deps);
	}
} // namespace aether::app::scene

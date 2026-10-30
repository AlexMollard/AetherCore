#include "scene/WorldStreaming.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

namespace aether::app::scene
{
	namespace
	{
		bool ValidPriority(io::IOPriority priority)
		{
			return static_cast<unsigned>(priority) <= static_cast<unsigned>(io::IOPriority::Background);
		}

		bool Finite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool Within(const streaming::Region& region, const glm::vec3& position, float radius)
		{
			double distance = 0;
			for (int i = 0; i < 3; ++i)
			{
				const double delta = std::max({static_cast<double>(region.boundsMin[i]) - position[i], 0.0, static_cast<double>(position[i]) - region.boundsMax[i]});
				distance += delta * delta;
			}
			return distance <= static_cast<double>(radius) * radius;
		}
	} // namespace

	struct WorldStreaming::Impl
	{
		struct Claim
		{
			std::string region;
			bool activate;
			io::IOPriority priority;
			bool cancelled = false;
		};

		std::unique_ptr<IWorldStreamingBackend> backend;
		std::vector<streaming::Region> regions;
		std::unordered_map<std::string, streaming::Source> sources;
		std::unordered_map<streaming::RequestId, Claim> claims;
		std::unordered_map<std::string, std::unordered_map<std::string, streaming::Demand>> spatial;
		std::unordered_map<std::string, std::string> errors;
		streaming::Budgets budgets;
		streaming::RequestId nextId = 1;
		bool stopped = false;
	};

	WorldStreaming::WorldStreaming(std::unique_ptr<IWorldStreamingBackend> backend)
	      : m_impl(std::make_unique<Impl>())
	{
		m_impl->backend = std::move(backend);
	}

	WorldStreaming::~WorldStreaming()
	{
		Shutdown();
	}

	Expected<void> WorldStreaming::Configure(std::span<const streaming::Region> regions)
	{
		if (regions.size() > 4096)
		{
			return std::unexpected(AetherError::Engine("Too many streaming regions"));
		}
		std::unordered_set<std::string> ids;
		for (const auto& region: regions)
		{
			if (region.id.empty() || region.id.size() > 255 || region.scene.empty() || !ids.insert(region.id).second || !Finite(region.boundsMin) || !Finite(region.boundsMax) || region.boundsMin.x > region.boundsMax.x
			        || region.boundsMin.y > region.boundsMax.y || region.boundsMin.z > region.boundsMax.z || !std::isfinite(region.unloadMargin) || region.unloadMargin < 0 || !ValidPriority(region.priority))
			{
				return std::unexpected(AetherError::Engine("Invalid streaming region"));
			}
		}
		if (!m_impl->backend || m_impl->stopped)
		{
			return std::unexpected(AetherError::Engine("Streaming backend unavailable"));
		}
		for (const auto& old: m_impl->regions)
		{
			if (!ids.contains(old.id))
			{
				AE_TRY_VOID(m_impl->backend->SetDemand(old, streaming::Demand::None, old.priority));
			}
		}
		m_impl->regions.assign(regions.begin(), regions.end());
		m_impl->spatial.clear();
		m_impl->errors.clear();
		for (auto& [id, claim]: m_impl->claims)
		{
			if (!ids.contains(claim.region))
			{
				claim.cancelled = true;
			}
		}
		return {};
	}

	Expected<void> WorldStreaming::SetBudgets(const streaming::Budgets& budgets)
	{
		if (!m_impl->backend || m_impl->stopped)
		{
			return std::unexpected(AetherError::Engine("Streaming backend unavailable"));
		}
		AE_TRY_VOID(m_impl->backend->SetBudgets(budgets));
		m_impl->budgets = budgets;
		return {};
	}

	Expected<void> WorldStreaming::SetSource(streaming::Source source)
	{
		if (m_impl->stopped || source.id.empty() || source.id.size() > 255 || !Finite(source.position) || (!m_impl->sources.contains(source.id) && m_impl->sources.size() >= m_impl->budgets.maxPendingRequests) || !std::isfinite(source.loadRadius)
		        || !std::isfinite(source.prefetchRadius) || source.loadRadius < 0 || source.prefetchRadius < source.loadRadius || !ValidPriority(source.priority))
		{
			return std::unexpected(AetherError::Engine("Invalid streaming source"));
		}
		auto id = source.id;
		m_impl->sources.insert_or_assign(std::move(id), std::move(source));
		return {};
	}

	bool WorldStreaming::RemoveSource(std::string_view id)
	{
		m_impl->spatial.erase(std::string(id));
		return m_impl->sources.erase(std::string(id)) != 0;
	}

	streaming::RequestId WorldStreaming::Request(std::string_view region, bool activate, io::IOPriority priority)
	{
		if (m_impl->stopped || !ValidPriority(priority) || m_impl->claims.size() >= m_impl->budgets.maxPendingRequests || std::none_of(m_impl->regions.begin(), m_impl->regions.end(), [&](const auto& r) { return r.id == region; }))
		{
			return 0;
		}
		auto id = m_impl->nextId++;
		while (id == 0 || m_impl->claims.contains(id))
		{
			id = m_impl->nextId++;
		}
		m_impl->claims.emplace(id, Impl::Claim{std::string(region), activate, priority});
		return id;
	}

	bool WorldStreaming::Cancel(streaming::RequestId id)
	{
		auto it = m_impl->claims.find(id);
		if (it == m_impl->claims.end() || it->second.cancelled)
		{
			return false;
		}
		it->second.cancelled = true;
		return true;
	}

	bool WorldStreaming::Release(streaming::RequestId id)
	{
		return m_impl->claims.erase(id) != 0;
	}

	streaming::State WorldStreaming::GetRequestState(streaming::RequestId id) const
	{
		auto it = m_impl->claims.find(id);
		if (it == m_impl->claims.end())
		{
			return streaming::State::Unknown;
		}
		if (it->second.cancelled)
		{
			return streaming::State::Cancelled;
		}
		const auto state = GetRegionState(it->second.region);
		return state == streaming::State::Unknown ? streaming::State::Queued : state;
	}

	streaming::State WorldStreaming::GetRegionState(std::string_view region) const
	{
		if (!m_impl->backend || m_impl->stopped)
		{
			return streaming::State::Unknown;
		}
		if (m_impl->errors.contains(std::string(region)))
		{
			return streaming::State::Failed;
		}
		return m_impl->backend->GetState(region);
	}

	void WorldStreaming::Update()
	{
		if (!m_impl->backend || m_impl->stopped)
		{
			return;
		}
		for (const auto& region: m_impl->regions)
		{
			auto demand = region.alwaysLoaded ? streaming::Demand::Active : streaming::Demand::None;
			auto priority = region.priority;
			for (const auto& [id, source]: m_impl->sources)
			{
				auto& previous = m_impl->spatial[id][region.id];
				const float activeMargin = previous == streaming::Demand::Active ? region.unloadMargin : 0;
				const float prefetchMargin = previous != streaming::Demand::None ? region.unloadMargin : 0;
				auto selected = Within(region, source.position, source.loadRadius + activeMargin)         ? streaming::Demand::Active
				                : Within(region, source.position, source.prefetchRadius + prefetchMargin) ? streaming::Demand::Prefetch
				                                                                                          : streaming::Demand::None;
				previous = selected;
				if (selected != streaming::Demand::None)
				{
					demand = std::max(demand, selected);
					priority = std::min(priority, source.priority);
				}
			}
			for (const auto& [id, claim]: m_impl->claims)
			{
				if (!claim.cancelled && claim.region == region.id)
				{
					demand = std::max(demand, claim.activate ? streaming::Demand::Active : streaming::Demand::Prefetch);
					priority = std::min(priority, claim.priority);
				}
			}
			auto result = m_impl->backend->SetDemand(region, demand, priority);
			if (result)
			{
				m_impl->errors.erase(region.id);
			}
			else
			{
				m_impl->errors[region.id] = result.error().message;
			}
		}
		m_impl->backend->Update();
	}

	std::string WorldStreaming::GetDiagnosticsJson() const
	{
		nlohmann::json result = {{"regions", nlohmann::json::array()}, {"sources", nlohmann::json::array()}, {"requests", nlohmann::json::array()}};
		for (const auto& region: m_impl->regions)
		{
			nlohmann::json row = {{"id", region.id}, {"scene", region.scene}, {"state", static_cast<unsigned>(GetRegionState(region.id))}};
			if (auto it = m_impl->errors.find(region.id); it != m_impl->errors.end())
			{
				row["error"] = it->second;
			}
			result["regions"].push_back(std::move(row));
		}
		for (const auto& [id, source]: m_impl->sources)
		{
			result["sources"].push_back(
			        {{"id", id}, {"position", {source.position.x, source.position.y, source.position.z}}, {"loadRadius", source.loadRadius}, {"prefetchRadius", source.prefetchRadius}, {"priority", static_cast<unsigned>(source.priority)}});
		}
		for (const auto& [id, claim]: m_impl->claims)
		{
			result["requests"].push_back({{"id", id}, {"region", claim.region}, {"activate", claim.activate}, {"priority", static_cast<unsigned>(claim.priority)}, {"state", static_cast<unsigned>(GetRequestState(id))}});
		}
		if (m_impl->backend)
		{
			auto backend = nlohmann::json::parse(m_impl->backend->GetDiagnosticsJson(), nullptr, false);
			result["backend"] = backend.is_discarded() ? nlohmann::json::object() : std::move(backend);
		}
		return result.dump();
	}

	void WorldStreaming::ResetWorld()
	{
		if (!m_impl || m_impl->stopped)
		{
			return;
		}
		if (m_impl->backend)
		{
			m_impl->backend->ResetWorld();
		}
		m_impl->regions.clear();
		m_impl->claims.clear();
		m_impl->sources.clear();
		m_impl->spatial.clear();
		m_impl->errors.clear();
	}

	void WorldStreaming::Shutdown()
	{
		if (m_impl && !m_impl->stopped)
		{
			m_impl->stopped = true;
			if (m_impl->backend)
			{
				m_impl->backend->Shutdown();
			}
			m_impl->claims.clear();
			m_impl->sources.clear();
			m_impl->spatial.clear();
		}
	}
} // namespace aether::app::scene

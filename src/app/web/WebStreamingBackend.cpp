#include "WebStreamingBackend.hpp"
#include "WebStreamingSnapshot.hpp"

#include <map>
#include <utility>
#include <nlohmann/json.hpp>
#include <emscripten.h>
#include "platform/web/WebStreamingTransport.hpp"
#include "streaming/StreamingBudget.hpp"

namespace
{
	struct SnapshotOrphan
	{
		std::shared_ptr<aether::streaming::BudgetLedger> budget;
		std::uint64_t bytes;
	};

	std::map<int, SnapshotOrphan> s_snapshotOrphans;
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void aether_web_snapshot_orphan_terminal(int id)
{
	const auto found = s_snapshotOrphans.find(id);
	if (found == s_snapshotOrphans.end())
	{
		return;
	}
	if (found->second.bytes)
	{
		found->second.budget->Release(aether::streaming::MemoryClass::CpuPending, found->second.bytes);
	}
	s_snapshotOrphans.erase(found);
}

namespace
{
	// clang-format off
	EM_JS(int, SnapshotBegin, (const char* key, const char* data, int length, int mode), {
		const s = Module.aetherStreamingSnapshots ||= {next : 1, jobs : new Map(), session : globalThis.crypto.randomUUID()};
		const id = s.next++;
		const job = {state : 0, value : new Uint8Array(0), size : 0, cancelled : false, orphan : false, tx : null};
		s.jobs.set(id, job);
		const region = UTF8ToString(key);
		const value = mode === 1 ? HEAPU8.slice(data, data + length) : null;
		const open = indexedDB.open('aethercore-streaming-' + s.session, 1);
		open.onupgradeneeded = () =>
		{
			open.result.createObjectStore('regions');
			open.result.createObjectStore('sizes');
		};
		const finish = state =>
		{
			if (job.state)
			{
				return;
			}
			job.state = state;
			if (job.orphan)
			{
				_aether_web_snapshot_orphan_terminal(id);
				s.jobs.delete(id);
			}
		};
		open.onerror = () => finish(-1);
		open.onsuccess = () =>
		{
			const db = open.result;
			if (job.cancelled)
			{
				db.close();
				finish(-1);
				return;
			}
			try
			{
				const tx = db.transaction([ 'regions', 'sizes' ], mode === 1 ? 'readwrite' : 'readonly');
				job.tx = tx;
				const request = mode === 1 ? tx.objectStore('regions').put(value, region) : mode === 0 ? tx.objectStore('sizes').get(region) : tx.objectStore('regions').get(region);
				if (mode === 1)
				{
					tx.objectStore('sizes').put(length, region);
				}
				request.onsuccess = () =>
				{
					if (mode === 0)
					{
						job.size = request.result || 0;
					}
					else if (mode === 2)
					{
						job.value = request.result || new Uint8Array(0);
					}
				};
				tx.oncomplete = () =>
				{
					db.close();
					finish(1);
				};
				tx.onerror = tx.onabort = () =>
				{
					db.close();
					finish(-1);
				};
			}
			catch (_)
			{
				db.close();
				finish(-1);
			}
		};
		return id;
	});
	EM_JS(int, SnapshotState, (int id), { return Module.aetherStreamingSnapshots?.jobs.get(id)?.state ?? -1; });
	EM_JS(int, SnapshotLength, (int id), {
		const job = Module.aetherStreamingSnapshots ?.jobs.get(id);
		return job ? (job.size || job.value.byteLength) : 0;
	});
	EM_JS(void, SnapshotCopy, (int id, char* out, int capacity), {
		const job = Module.aetherStreamingSnapshots ?.jobs.get(id);
		if (job && job.value.byteLength <= capacity)
		{
			HEAPU8.set(job.value, out);
		}
	});
	EM_JS(void, SnapshotForget, (int id), { Module.aetherStreamingSnapshots?.jobs.delete(id); });
	EM_JS(void, SnapshotAbort, (int id, int orphan), {
		const job = Module.aetherStreamingSnapshots ?.jobs.get(id);
		if (!job)
		{
			return;
		}
		job.cancelled = true;
		job.orphan = !!orphan;
		if (job.tx)
		{
			try
			{
				job.tx.abort();
			}
			catch (_)
			{
			}
		}
	});
	EM_JS(void, SnapshotReset, (), {
		const s = Module.aetherStreamingSnapshots;
		if (s)
		{
			indexedDB.deleteDatabase('aethercore-streaming-' + s.session);
			s.session = globalThis.crypto.randomUUID();
		}
	});
	// clang-format on
} // namespace

namespace aether::app::web
{
	class WebStreamingBackend final : public scene::IWorldStreamingBackend
	{
	public:
		WebStreamingBackend(WebStreamingCallbacks callbacks)
		      : m_callbacks(std::move(callbacks))
		{
		}

		Expected<void> Initialize(std::string_view url)
		{
			m_callbacks.bindBudget(m_budget);
			return m_transport.Initialize(url, *m_budget);
		}

		Expected<void> SetBudgets(const streaming::Budgets& budgets) override
		{
			return m_budget->Configure(budgets);
		}

		Expected<void> SetDemand(const streaming::Region& region, streaming::Demand demand, io::IOPriority priority) override
		{
			auto& item = m_regions[region.id];
			if (item.state == streaming::State::Failed && item.demand != demand && !item.rollback && !item.job)
			{
				if (item.hasActive && !item.committed)
				{
					AE_TRY_VOID(m_callbacks.detach(item.active));
					item.hasActive = false;
				}
				if (!item.hasActive)
				{
					m_callbacks.cancelAttach(region.id);
					item.attaching = false;
					item.waitingReady = false;
					item.scriptsStarted = false;
					if (item.snapshotRequest)
					{
						item.retiredSnapshots.push_back(item.snapshotRequest);
						item.snapshotRequest = 0;
					}
					if (item.request && (m_transport.GetState(item.request) == streaming::State::Failed || m_transport.GetState(item.request) == streaming::State::Cancelled))
					{
						m_transport.Release(item.request);
						item.request = 0;
					}
				}
				std::string{}.swap(item.snapshot);
				m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
				item.staging = 0;
				item.state = item.hasActive ? streaming::State::Active : streaming::State::Unknown;
				item.error.clear();
			}
			item.region = region;
			item.demand = demand;
			item.priority = priority;
			if (demand != streaming::Demand::None && !item.request)
			{
				item.request = m_transport.Acquire(region.scene, priority);
				if (!item.request)
				{
					item.state = streaming::State::Blocked;
					item.error = "Streaming request queue is full";
				}
			}
			else if (item.request && demand != streaming::Demand::None)
			{
				m_transport.SetPriority(item.request, priority);
			}
			if (item.snapshotRequest && demand != streaming::Demand::None)
			{
				m_transport.SetPriority(item.snapshotRequest, priority);
			}
			return {};
		}

		void Update() override
		{
			m_transport.Update();
			for (auto it = m_promoted.begin(); it != m_promoted.end();)
			{
				bool ready = true;
				for (auto actor = it->actors.begin(); actor != it->actors.end();)
				{
					if (!m_callbacks.promotedAlive({actor->root}))
					{
						if (m_callbacks.retirementPending())
						{
							ready = false;
							++actor;
							continue;
						}
						if (actor->pending)
						{
							m_transport.Cancel(actor->pending);
							m_transport.Release(actor->pending);
						}
						if (actor->current)
						{
							m_transport.Release(actor->current);
						}
						for (const auto request: actor->retired)
						{
							m_transport.Release(request);
						}
						m_budget->Release(streaming::MemoryClass::CpuPending, actor->currentCharge + actor->pendingCharge);
						actor = it->actors.erase(actor);
						continue;
					}
					if (!m_callbacks.retirementPending())
					{
						for (const auto request: actor->retired)
						{
							m_transport.Release(request);
						}
						actor->retired.clear();
					}
					auto workspace = m_callbacks.measurePersistentAssets(actor->root);
					if (!workspace || !m_budget->TryReserve(streaming::MemoryClass::CpuPending, *workspace))
					{
						ready = false;
						++actor;
						continue;
					}
					auto document = m_callbacks.persistentAssets(actor->root);
					if (!document)
					{
						m_budget->Release(streaming::MemoryClass::CpuPending, *workspace);
						ready = false;
						++actor;
						continue;
					}
					if (actor->pending && m_transport.GetState(actor->pending) == streaming::State::Loaded)
					{
						if (*document == actor->pendingDocument)
						{
							if (actor->current)
							{
								actor->retired.push_back(actor->current);
							}
							actor->current = actor->pending;
							m_budget->Release(streaming::MemoryClass::CpuPending, actor->currentCharge);
							actor->currentCharge = actor->pendingCharge;
							actor->pendingCharge = 0;
							actor->document = std::move(actor->pendingDocument);
						}
						else
						{
							actor->retired.push_back(actor->pending);
							std::string{}.swap(actor->pendingDocument);
							m_budget->Release(streaming::MemoryClass::CpuPending, actor->pendingCharge);
							actor->pendingCharge = 0;
						}
						actor->pending = 0;
					}
					else if (actor->pending && (m_transport.GetState(actor->pending) == streaming::State::Failed || m_transport.GetState(actor->pending) == streaming::State::Cancelled))
					{
						m_transport.Release(actor->pending);
						std::string{}.swap(actor->pendingDocument);
						m_budget->Release(streaming::MemoryClass::CpuPending, actor->pendingCharge);
						actor->pendingCharge = 0;
						actor->pending = 0;
					}
					const bool matches = *document == actor->document;
					if (!actor->pending && !matches)
					{
						actor->pending = m_transport.PrepareSnapshot(*document, io::IOPriority::Normal);
						if (actor->pending)
						{
							actor->pendingDocument = std::move(*document);
							actor->pendingCharge = actor->pendingDocument.capacity() + 1;
							*workspace -= actor->pendingCharge;
						}
					}
					if (!actor->current || actor->pending || !matches)
					{
						ready = false;
					}
					m_budget->Release(streaming::MemoryClass::CpuPending, *workspace);
					++actor;
				}
				if (ready && !m_callbacks.retirementPending())
				{
					if (it->request)
					{
						m_transport.Release(it->request);
						it->request = 0;
					}
					for (const auto request: it->snapshotRequests)
					{
						m_transport.Release(request);
					}
					it->snapshotRequests.clear();
				}
				if (it->actors.empty() && !it->request && it->snapshotRequests.empty())
				{
					it = m_promoted.erase(it);
				}
				else
				{
					++it;
				}
			}
			for (auto& [id, item]: m_regions)
			{
				if (item.job)
				{
					const int status = SnapshotState(item.job);
					if (status == 0)
					{
						if (!item.writing && item.demand != streaming::Demand::Active && !item.abortingRead)
						{
							item.abortingRead = true;
							SnapshotAbort(item.job, 0);
						}
						continue;
					}
					if (item.abortingRead)
					{
						SnapshotForget(item.job);
						item.job = 0;
						item.abortingRead = false;
						item.readingSize = false;
						if (item.staging)
						{
							m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
							item.staging = 0;
						}
						item.state = streaming::State::Retiring;
						continue;
					}
					if (status < 0)
					{
						SnapshotForget(item.job);
						item.job = 0;
						if (item.writing)
						{
							item.error = "IndexedDB snapshot write failed; restoring detached region";
							item.writing = false;
							item.rollback = true;
							item.attaching = true;
							item.state = streaming::State::Loading;
							continue;
						}
						item.error = "IndexedDB snapshot read failed";
						if (item.staging)
						{
							m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
							item.staging = 0;
						}
						item.state = streaming::State::Failed;
						continue;
					}
					if (item.writing)
					{
						SnapshotForget(item.job);
						item.job = 0;
						item.writing = false;
						if (item.demand == streaming::Demand::Active)
						{
							item.attaching = true;
							item.state = streaming::State::Loading;
						}
						else
						{
							item.state = streaming::State::Retiring;
							std::string{}.swap(item.snapshot);
							if (item.staging)
							{
								m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
								item.staging = 0;
							}
						}
					}
					else
					{
						const int size = SnapshotLength(item.job);
						if (size < 0 || static_cast<std::size_t>(size) > kStreamingSnapshotInputLimit)
						{
							SnapshotForget(item.job);
							item.job = 0;
							if (item.staging)
							{
								m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
								item.staging = 0;
							}
							item.state = streaming::State::Failed;
							continue;
						}
						if (item.readingSize)
						{
							if (item.demand != streaming::Demand::Active)
							{
								SnapshotForget(item.job);
								item.job = 0;
								item.readingSize = false;
								continue;
							}
							const auto reservation = static_cast<std::uint64_t>(size) * 8 + kStreamingScriptCaptureScratch + 1;
							if (!m_budget->TryReserve(streaming::MemoryClass::CpuPending, reservation))
							{
								item.state = streaming::State::Blocked;
								item.error = "CPU budget blocks snapshot read";
								continue;
							}
							SnapshotForget(item.job);
							item.job = 0;
							item.staging = reservation;
							item.expectedSize = static_cast<std::uint32_t>(size);
							item.readingSize = false;
							if (size)
							{
								item.job = SnapshotBegin(id.c_str(), nullptr, 0, 2);
								continue;
							}
							std::string{}.swap(item.snapshot);
							item.attaching = true;
							continue;
						}
						if (static_cast<std::uint32_t>(size) != item.expectedSize)
						{
							SnapshotForget(item.job);
							item.job = 0;
							m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
							item.staging = 0;
							item.state = streaming::State::Failed;
							continue;
						}
						item.snapshot.assign(static_cast<std::size_t>(size), '\0');
						SnapshotCopy(item.job, item.snapshot.data(), size);
						SnapshotForget(item.job);
						item.job = 0;
						item.attaching = item.demand == streaming::Demand::Active;
						if (!item.attaching)
						{
							std::string{}.swap(item.snapshot);
							m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
							item.staging = 0;
						}
					}
					continue;
				}
				if (item.attaching)
				{
					if (item.demand != streaming::Demand::Active && !item.rollback)
					{
						m_callbacks.cancelAttach(id);
						item.attaching = false;
						std::string{}.swap(item.snapshot);
						m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
						item.staging = 0;
						if (item.snapshotRequest)
						{
							item.retiredSnapshots.push_back(item.snapshotRequest);
							item.snapshotRequest = 0;
						}
						item.state = streaming::State::Retiring;
						continue;
					}
					std::span<const std::byte> preparedSnapshot;
					if (!item.snapshot.empty())
					{
						if (!item.snapshotRequest)
						{
							auto document = StreamingSnapshotDocument(item.snapshot);
							if (!document)
							{
								item.error = document.error().message;
								item.state = streaming::State::Failed;
								item.attaching = false;
								continue;
							}
							item.gateNode = document->second;
							item.snapshotRequest = m_transport.PrepareSnapshot(document->first, item.priority);
							if (!item.snapshotRequest)
							{
								item.error = "Streaming snapshot parser queue is full";
								item.state = streaming::State::Blocked;
								continue;
							}
						}
						const auto state = m_transport.GetState(item.snapshotRequest);
						if (state == streaming::State::Failed || state == streaming::State::Cancelled)
						{
							item.error = m_transport.GetError(item.snapshotRequest);
							item.state = streaming::State::Failed;
							item.attaching = false;
							continue;
						}
						if (state != streaming::State::Loaded)
						{
							item.state = state;
							continue;
						}
						preparedSnapshot = m_transport.GetPreparedSnapshot(item.snapshotRequest);
						if (preparedSnapshot.empty())
						{
							item.error = "Prepared snapshot packet is missing";
							item.state = streaming::State::Failed;
							item.attaching = false;
							continue;
						}
					}
					auto active = m_callbacks.attach(id, item.region.scene, preparedSnapshot, item.gateNode);
					if (!active)
					{
						m_callbacks.cancelAttach(id);
						item.error = active.error().message;
						if (item.error.find("budget") != std::string::npos)
						{
							item.state = streaming::State::Blocked;
							continue;
						}
						item.state = streaming::State::Failed;
					}
					else if (!*active)
					{
						item.state = streaming::State::Uploading;
						continue;
					}
					else
					{
						item.active = std::move(**active);
						item.hasActive = true;
						item.committed = false;
						item.scriptsStarted = false;
						const bool restored = item.snapshot.empty() || static_cast<bool>(m_callbacks.restore(item.active, item.snapshot));
						item.state = restored ? streaming::State::Uploading : streaming::State::Failed;
						item.waitingReady = restored;
					}
					item.attaching = false;
					if (item.waitingReady)
					{
						continue;
					}
					if (item.rollback && item.state == streaming::State::Failed)
					{
						continue;
					}
					std::string{}.swap(item.snapshot);
					m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
					item.staging = 0;
					continue;
				}
				if (item.waitingReady)
				{
					if (item.demand != streaming::Demand::Active && !item.rollback)
					{
						const auto roots = m_callbacks.persistentRoots(id);
						item.actorRoots.insert(item.actorRoots.end(), roots.begin(), roots.end());
						if (!m_callbacks.detach(item.active))
						{
							item.state = streaming::State::Failed;
						}
						else
						{
							item.hasActive = false;
							item.committed = false;
							item.state = streaming::State::Retiring;
						}
					}
					else
					{
						if (!m_callbacks.ready())
						{
							item.state = m_callbacks.budgetBlocked() ? streaming::State::Blocked : streaming::State::Uploading;
							if (!item.rollback)
							{
								item.error = m_callbacks.budgetBlocked() ? "GPU budget blocks pending mesh upload" : "";
							}
							continue;
						}
						bool restored = true;
						if (!item.scriptsStarted)
						{
							auto started = m_callbacks.start(item.active);
							if (!started)
							{
								item.error = started.error().message;
								restored = false;
							}
							else if (!*started)
							{
								item.state = streaming::State::Uploading;
								continue;
							}
							else
							{
								item.scriptsStarted = true;
							}
						}
						if (restored && !item.snapshot.empty())
						{
							auto result = m_callbacks.restoreScripts(item.active, item.snapshot);
							if (!result)
							{
								item.error = result.error().message;
								restored = false;
							}
							else if (!*result)
							{
								item.state = streaming::State::Uploading;
								continue;
							}
						}
						if (restored)
						{
							m_callbacks.commit(item.active);
							item.committed = true;
						}
						item.state = restored ? (item.rollback ? streaming::State::Failed : streaming::State::Active) : streaming::State::Failed;
						if (!restored && item.rollback)
						{
							item.waitingReady = false;
							continue;
						}
						item.rollback = false;
					}
					item.waitingReady = false;
					std::string{}.swap(item.snapshot);
					m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
					item.staging = 0;
					continue;
				}
				if (item.state == streaming::State::Failed)
				{
					continue;
				}
				if (item.state == streaming::State::Retiring)
				{
					if (m_callbacks.retirementPending())
					{
						continue;
					}
					if (item.snapshotRequest)
					{
						item.retiredSnapshots.push_back(item.snapshotRequest);
						item.snapshotRequest = 0;
					}
					if (m_callbacks.promotedAlive(item.actorRoots))
					{
						PromotedLease lease{item.request, std::move(item.retiredSnapshots), {}};
						for (const auto root: item.actorRoots)
						{
							lease.actors.push_back(ActorLease{.root = root});
						}
						m_promoted.push_back(std::move(lease));
						item.actorRoots.clear();
					}
					else
					{
						if (item.request)
						{
							m_transport.Release(item.request);
						}
						for (const auto request: item.retiredSnapshots)
						{
							m_transport.Release(request);
						}
						item.retiredSnapshots.clear();
					}
					item.request = 0;
					item.state = streaming::State::Unknown;
					if (item.demand != streaming::Demand::None)
					{
						item.request = m_transport.Acquire(item.region.scene, item.priority);
					}
					continue;
				}
				if (item.hasActive && item.demand != streaming::Demand::Active)
				{
					auto snapshot = m_callbacks.capture(item.active);
					if (!snapshot)
					{
						item.error = snapshot.error().message;
						item.state = item.error == "CPU budget blocks snapshot workspace" ? streaming::State::Blocked : streaming::State::Failed;
						continue;
					}
					item.staging = snapshot->capacity() + 1 + snapshot->size();
					if (snapshot->size() > kStreamingSnapshotInputLimit)
					{
						m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
						item.staging = 0;
						item.state = streaming::State::Failed;
						continue;
					}
					item.snapshot = std::move(*snapshot);
					const auto roots = m_callbacks.persistentRoots(id);
					item.actorRoots.insert(item.actorRoots.end(), roots.begin(), roots.end());
					if (!m_callbacks.detach(item.active))
					{
						item.error = "Region detach failed; retained live entities and rollback snapshot";
						item.state = streaming::State::Failed;
						continue;
					}
					item.hasActive = false;
					item.committed = false;
					if (item.snapshotRequest)
					{
						item.retiredSnapshots.push_back(item.snapshotRequest);
						item.snapshotRequest = 0;
					}
					item.job = SnapshotBegin(id.c_str(), item.snapshot.data(), static_cast<int>(item.snapshot.size()), 1);
					item.writing = true;
					item.state = streaming::State::Saving;
					continue;
				}
				if (item.hasActive)
				{
					item.state = streaming::State::Active;
					continue;
				}
				if (item.demand == streaming::Demand::None)
				{
					if (item.request)
					{
						m_transport.Cancel(item.request);
						m_transport.Release(item.request);
						item.request = 0;
					}
					item.state = streaming::State::Unknown;
					continue;
				}
				if (!item.request)
				{
					item.state = streaming::State::Blocked;
					continue;
				}
				item.state = m_transport.GetState(item.request);
				item.error = (item.state == streaming::State::Failed || item.state == streaming::State::Blocked) ? m_transport.GetError(item.request) : std::string{};
				if (item.state == streaming::State::Loaded && item.demand == streaming::Demand::Active)
				{
					item.job = SnapshotBegin(id.c_str(), nullptr, 0, 0);
					item.writing = false;
					item.readingSize = true;
					item.state = streaming::State::Loading;
				}
			}
		}

		streaming::State GetState(std::string_view id) const override
		{
			auto it = m_regions.find(std::string(id));
			return it == m_regions.end() ? streaming::State::Unknown : it->second.state;
		}

		std::string GetDiagnosticsJson() const override
		{
			auto result = nlohmann::json::parse(m_transport.GetDiagnosticsJson(), nullptr, false);
			if (!result.is_object())
			{
				result = nlohmann::json::object();
			}
			auto rows = nlohmann::json::array();
			for (const auto& [id, item]: m_regions)
			{
				rows.push_back({{"id", id}, {"state", static_cast<std::uint32_t>(item.state)}, {"reason", item.error}});
			}
			result["regionBackend"] = std::move(rows);
			auto actors = nlohmann::json::array();
			for (const auto& lease: m_promoted)
			{
				for (const auto& actor: lease.actors)
				{
					actors.push_back({{"entity", actor.root.id}, {"acquired", actor.current != 0}, {"preparing", actor.pending != 0}, {"originalClaimRetained", lease.request != 0 || !lease.snapshotRequests.empty()}});
				}
			}
			result["persistentActors"] = std::move(actors);
			return result.dump();
		}

		void ResetWorld() override
		{
			for (auto& [id, item]: m_regions)
			{
				if (item.job && SnapshotState(item.job) == 0)
				{
					s_snapshotOrphans.emplace(item.job, SnapshotOrphan{m_budget, item.staging});
					SnapshotAbort(item.job, 1);
					item.staging = 0;
				}
				else if (item.job)
				{
					SnapshotForget(item.job);
				}
				if (item.staging)
				{
					m_budget->Release(streaming::MemoryClass::CpuPending, item.staging);
				}
				auto roots = std::move(item.actorRoots);
				if (item.hasActive)
				{
					const auto activeRoots = m_callbacks.persistentRoots(id);
					roots.insert(roots.end(), activeRoots.begin(), activeRoots.end());
				}
				if (m_callbacks.promotedAlive(roots))
				{
					if (item.snapshotRequest)
					{
						item.retiredSnapshots.push_back(item.snapshotRequest);
					}
					PromotedLease lease{item.request, std::move(item.retiredSnapshots), {}};
					for (const auto root: roots)
					{
						lease.actors.push_back(ActorLease{.root = root});
					}
					m_promoted.push_back(std::move(lease));
				}
				else
				{
					if (item.request)
					{
						m_transport.Cancel(item.request);
						m_transport.Release(item.request);
					}
					if (item.snapshotRequest)
					{
						m_transport.Cancel(item.snapshotRequest);
						m_transport.Release(item.snapshotRequest);
					}
					for (const auto request: item.retiredSnapshots)
					{
						m_transport.Release(request);
					}
				}
				if (item.attaching)
				{
					m_callbacks.cancelAttach(id);
				}
			}
			m_regions.clear();
			SnapshotReset();
		}

		void Shutdown() override
		{
			ResetWorld();
			m_transport.Shutdown();
		}

	private:
		struct Item
		{
			streaming::Region region;
			streaming::Demand demand = streaming::Demand::None;
			io::IOPriority priority = io::IOPriority::Normal;
			streaming::State state = streaming::State::Unknown;
			streaming::RequestId request = 0;
			streaming::RequestId snapshotRequest = 0;
			std::vector<streaming::RequestId> retiredSnapshots;
			std::uint64_t gateNode = 0;
			WebRegion active;
			bool hasActive = false, committed = false, writing = false, attaching = false, waitingReady = false, readingSize = false, scriptsStarted = false, rollback = false, abortingRead = false;
			int job = 0;
			std::uint32_t expectedSize = 0;
			std::uint64_t staging = 0;
			std::string snapshot;
			std::string error;
			std::vector<Entity> actorRoots;
		};

		struct ActorLease
		{
			Entity root;
			std::string document, pendingDocument;
			streaming::RequestId current = 0, pending = 0;
			std::uint64_t currentCharge = 0, pendingCharge = 0;
			std::vector<streaming::RequestId> retired;
		};

		struct PromotedLease
		{
			streaming::RequestId request;
			std::vector<streaming::RequestId> snapshotRequests;
			std::vector<ActorLease> actors;
		};

		std::vector<PromotedLease> m_promoted;
		std::map<std::string, Item> m_regions;
		WebStreamingCallbacks m_callbacks;
		std::shared_ptr<streaming::BudgetLedger> m_budget = std::make_shared<streaming::BudgetLedger>();
		aether::web::WebStreamingTransport m_transport;
	};

	Expected<std::unique_ptr<scene::IWorldStreamingBackend>> CreateWebStreamingBackend(std::string_view url, WebStreamingCallbacks callbacks)
	{
		auto backend = std::make_unique<WebStreamingBackend>(std::move(callbacks));
		AE_TRY_VOID(backend->Initialize(url));
		return std::unique_ptr<scene::IWorldStreamingBackend>(std::move(backend));
	}
} // namespace aether::app::web

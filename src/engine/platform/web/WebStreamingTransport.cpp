#include "WebStreamingTransport.hpp"
#include "WebFileSystem.hpp"
#include "io/FileSystem.hpp"
#include <emscripten.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// All bridge state belongs to one transport; results are pulled only on the game thread.
// clang-format off
EM_JS(void, ae_stream_open, (const char* url), {
	const base = new URL(UTF8ToString(url), document.baseURI);
	const s = globalThis.__aeStream = {base, manifest : null, error : null, result : null, abort : new AbortController(), worker : new Worker(new URL('streaming-worker.js', base))};
	s.worker.onmessage = e =>
	{
		s.result = e.data;
	};
	s.worker.onerror = e =>
	{
		s.result = {error : e.message || 'Streaming worker failed'};
	};
	fetch(base, {cache:'no-store', signal:s.abort.signal}).then(async r => {
   if (!r.ok){ throw Error('Manifest HTTP ' + r.status);
}
   const reader = r.body.getReader(), parts = []; let size = 0;
   for (;;) { const part = await reader.read(); if (part.done){ break;
} size += part.value.byteLength; if (size > 4 * 1024 * 1024) { await reader.cancel(); throw Error('Streaming manifest too large'); } parts.push(part.value); }
   const bytes = new Uint8Array(size); let offset = 0; for (const part of parts) { bytes.set(part, offset); offset += part.length; }
   s.manifest = new TextDecoder('utf-8', {fatal:true}).decode(bytes);
 }).catch(e => { s.error = String(e); });
});
EM_JS(char*, ae_stream_manifest, (), {
	const s = globalThis.__aeStream;
	if (!s || (!s.manifest && !s.error))
	{
		return 0;
	}
	const text = s.error ? JSON.stringify({error : s.error}) : s.manifest;
	const p = _malloc(lengthBytesUTF8(text) + 1);
	stringToUTF8(text, p, lengthBytesUTF8(text) + 1);
	s.manifest = null;
	s.error = null;
	return p;
});
EM_JS(void, ae_stream_fetch, (const char* url, double size, const char* hash, const char* path, double dependencies), {
	const s = globalThis.__aeStream;
	s.result = null;
	s.worker.postMessage({type : 'fetch', id : 1, url : new URL(UTF8ToString(url), s.base).href, size, sha256 : UTF8ToString(hash), path : UTF8ToString(path), dependencyBytes : dependencies});
});
EM_JS(void, ae_stream_cancel, (), {
	const s = globalThis.__aeStream;
	if (s)
	{
		s.worker.postMessage({type : 'cancel', id : 1});
	}
});
EM_JS(int, ae_stream_result, (), {
	const s = globalThis.__aeStream;
	return s && s.result ? (s.result.error ? -1 : s.result.phase === 'prepare' ? 2 : 1) : 0;
});
EM_JS(double, ae_stream_prepared_size, (), { return globalThis.__aeStream.result.preparedSize; });
EM_JS(double, ae_stream_base_size, (), { return globalThis.__aeStream.result.baseSize; });
EM_JS(double, ae_stream_mips_size, (), { return globalThis.__aeStream.result.mipsSize; });
EM_JS(void, ae_stream_add_dependency, (const char* path, const void* bytes, unsigned size), {
	const s = globalThis.__aeStream;
	if (!s.files)
	{
		s.files = [];
	}
	s.files.push({path : UTF8ToString(path), bytes : HEAPU8.slice(bytes, bytes + size).buffer});
});
EM_JS(void, ae_stream_continue, (int admit), {
	const s = globalThis.__aeStream;
	s.result = null;
	const files = s.files || [];
	s.files = null;
	s.worker.postMessage({type : 'continue', id : 1, admit : !!admit, files}, files.map(f => f.bytes));
});
EM_JS(char*, ae_stream_error, (), {
	const s = globalThis.__aeStream;
	const t = s.result.error || 'Streaming worker failed';
	const p = _malloc(lengthBytesUTF8(t) + 1);
	stringToUTF8(t, p, lengthBytesUTF8(t) + 1);
	return p;
});
EM_JS(void, ae_stream_copy, (void* dst, void* prepared, void* mips), {
	const r = globalThis.__aeStream.result;
	HEAPU8.set(new Uint8Array(r.bytes), dst);
	if (r.prepared)
	{
		HEAPU8.set(new Uint8Array(r.prepared), prepared);
	}
	if (r.mips)
	{
		HEAPU8.set(new Uint8Array(r.mips), mips);
	}
	globalThis.__aeStream.result = null;
});
EM_JS(void, ae_stream_close, (), {
	const s = globalThis.__aeStream;
	if (s)
	{
		s.abort.abort();
		s.worker.terminate();
	}
	delete globalThis.__aeStream;
});
EM_JS(void, ae_stream_discard, (), {
	const s = globalThis.__aeStream;
	if (s)
	{
		s.result = null;
	}
});
EM_JS(void, ae_scene_open, (), {
	const s = globalThis.__aeStream;
	s.sceneFiles = [];
	s.sceneAliases = [];
	s.sceneResult = null;
	s.sceneWorker = new Worker(new URL('streaming-scene-worker.js', s.base));
	s.sceneWorker.onmessage = e =>
	{
		s.sceneResult = e.data;
	};
	s.sceneWorker.onerror = e =>
	{
		s.sceneResult = {error : e.message || 'Scene parser worker failed'};
	};
});
EM_JS(void, ae_scene_add_file, (const char* path, const void* data, unsigned size), {
	const s = globalThis.__aeStream;
	s.sceneFiles.push({path : UTF8ToString(path), bytes : HEAPU8.slice(data, data + size).buffer});
});
EM_JS(void, ae_scene_add_alias, (const char* alias, const char* path), { globalThis.__aeStream.sceneAliases.push([ UTF8ToString(alias), UTF8ToString(path) ]); });
EM_JS(void, ae_scene_start, (const char* root, unsigned maxOutput, int snapshot), {
	const s = globalThis.__aeStream, paths = globalThis.aetherProject ?.paths || {};
	const files = s.sceneFiles;
	s.sceneFiles = null;
	s.sceneWorker.postMessage(
	        {type : 'prepare', root : UTF8ToString(root), scenesDir : paths.scenes || 'scenes', prefabsDir : paths.prefabs || 'assets/prefabs', maxOutput, snapshot : !!snapshot, files, aliases : s.sceneAliases}, files.map(f => f.bytes));
	s.sceneAliases = null;
});
EM_JS(int, ae_scene_result, (), {
	const s = globalThis.__aeStream;
	return !s || !s.sceneResult ? 0 : s.sceneResult.error ? -1 : 1;
});
EM_JS(unsigned, ae_scene_size, (), { return globalThis.__aeStream.sceneResult.bytes.byteLength; });
EM_JS(void, ae_scene_copy, (void* data), { HEAPU8.set(new Uint8Array(globalThis.__aeStream.sceneResult.bytes), data); });
EM_JS(unsigned, ae_scene_dependencies_size, (), { return globalThis.__aeStream.sceneResult.dependencies.byteLength; });
EM_JS(void, ae_scene_copy_dependencies, (void* data), { HEAPU8.set(new Uint8Array(globalThis.__aeStream.sceneResult.dependencies), data); });
EM_JS(char*, ae_scene_error, (), {
	const t = globalThis.__aeStream.sceneResult.error;
	const p = _malloc(lengthBytesUTF8(t) + 1);
	stringToUTF8(t, p, lengthBytesUTF8(t) + 1);
	return p;
});
EM_JS(void, ae_scene_close, (), {
	const s = globalThis.__aeStream;
	if (s && s.sceneWorker)
	{
		s.sceneWorker.terminate();
		s.sceneWorker = null;
		s.sceneFiles = null;
		s.sceneAliases = null;
		s.sceneResult = null;
	}
});
// clang-format on

namespace aether::web
{
	using streaming::MemoryClass;
	using json = nlohmann::json;
	constexpr std::uint64_t kManifestPending = 32ull * 1024 * 1024;
	constexpr std::uint64_t kManifestResident = 16ull * 1024 * 1024;
	constexpr std::uint64_t kManifestStaging = 8ull * 1024 * 1024;

	struct WebStreamingTransport::Impl
	{
		struct Asset
		{
			std::string url, hash, error, preparedPath, mipsPath;
			std::vector<std::string> deps;
			std::uint64_t size = 0, preparedSize = 0;
			bool mounted = false;
		};

		struct Request
		{
			std::string reference, error;
			io::IOPriority priority;
			State state = State::Queued;
			std::vector<std::string> assets;
			bool snapshot = false, snapshotParsed = false;
			std::string snapshotText, snapshotPath;
			std::uint64_t snapshotCharge = 0, dependencyCharge = 0;
		};

		streaming::BudgetLedger* budget = nullptr;
		std::unordered_map<std::string, Asset> assets;
		std::unordered_map<std::string, std::string> aliases;
		std::unordered_map<RequestId, Request> requests;
		std::unordered_map<std::string, std::uint64_t> preparedScenes;
		std::unordered_map<std::string, std::string> sceneErrors;
		std::string sceneInFlight;
		std::uint64_t scenePending = 0, sceneStaging = 0, sceneOutputLimit = 0;
		RequestId next = 1;
		std::string error, inFlight;
		std::uint64_t preparingBytes = 0, preparingInput = 0, preparingBase = 0, preparingMips = 0;
		bool manifest = false, cancelled = false, manifestPending = false, manifestResident = false;

		void DropSnapshot(const Request& request)
		{
			if (!request.snapshot)
			{
				return;
			}
			if (sceneInFlight == request.snapshotPath)
			{
				ae_scene_close();
				budget->Release(MemoryClass::CpuPending, scenePending);
				budget->Release(MemoryClass::Staging, sceneStaging);
				sceneInFlight.clear();
				scenePending = 0;
				sceneStaging = 0;
			}
			if (auto prepared = preparedScenes.find(request.snapshotPath); prepared != preparedScenes.end())
			{
				UnmountWebAsset(request.snapshotPath + ".prepared-scene");
				budget->Release(MemoryClass::CpuResident, prepared->second);
				preparedScenes.erase(prepared);
			}
			sceneErrors.erase(request.snapshotPath);
		}
	};

	WebStreamingTransport::WebStreamingTransport()
	      : m_impl(std::make_unique<Impl>())
	{
	}

	WebStreamingTransport::~WebStreamingTransport()
	{
		Shutdown();
	}

	Expected<void> WebStreamingTransport::Initialize(std::string_view url, streaming::BudgetLedger& budget)
	{
		if (m_impl->budget || url.empty())
		{
			return std::unexpected(AetherError::Asset("Invalid streaming transport initialization"));
		}
		if (!budget.TryReserve(MemoryClass::CpuPending, kManifestPending))
		{
			return std::unexpected(AetherError::Asset("Insufficient streaming manifest CPU budget"));
		}
		if (!budget.TryReserve(MemoryClass::Staging, kManifestStaging))
		{
			budget.Release(MemoryClass::CpuPending, kManifestPending);
			return std::unexpected(AetherError::Asset("Insufficient streaming manifest staging budget"));
		}
		m_impl->budget = &budget;
		m_impl->manifestPending = true;
		ae_stream_open(std::string(url).c_str());
		return {};
	}

	WebStreamingTransport::RequestId WebStreamingTransport::Acquire(std::string_view ref, io::IOPriority priority)
	{
		if (!m_impl->budget || ref.empty() || ref.size() > 4096 || m_impl->requests.size() >= m_impl->budget->Limits().maxPendingRequests)
		{
			return 0;
		}
		const auto id = m_impl->next++;
		m_impl->requests.emplace(id, Impl::Request{std::string(ref), {}, priority});
		return id;
	}

	WebStreamingTransport::RequestId WebStreamingTransport::PrepareSnapshot(std::string_view text, io::IOPriority priority)
	{
		auto& m = *m_impl;
		if (!m.budget || text.empty() || text.size() > 4u * 1024u * 1024u || m.requests.size() >= m.budget->Limits().maxPendingRequests)
		{
			return 0;
		}
		if (!m.budget->TryReserve(MemoryClass::CpuPending, text.size()))
		{
			return 0;
		}
		const auto id = m.next++;
		Impl::Request request{{}, {}, priority};
		request.snapshot = true;
		request.snapshotText = std::string(text);
		request.snapshotCharge = text.size();
		request.snapshotPath = "project://__aether_streaming_snapshot/" + std::to_string(id) + ".scene.toml";
		m.requests.emplace(id, std::move(request));
		return id;
	}

	std::span<const std::byte> WebStreamingTransport::GetPreparedSnapshot(RequestId id) const
	{
		const auto it = m_impl->requests.find(id);
		if (it == m_impl->requests.end() || !it->second.snapshot || it->second.state != State::Loaded)
		{
			return {};
		}
		return FindWebAsset(it->second.snapshotPath + ".prepared-scene");
	}

	bool WebStreamingTransport::Release(RequestId id)
	{
		auto& m = *m_impl;
		auto it = m.requests.find(id);
		if (it == m.requests.end())
		{
			return false;
		}
		m.DropSnapshot(it->second);
		const auto text = it->second.snapshotCharge, dependencies = it->second.dependencyCharge;
		m.requests.erase(it);
		if (text)
		{
			m.budget->Release(MemoryClass::CpuPending, text);
		}
		if (dependencies)
		{
			m.budget->Release(MemoryClass::CpuResident, dependencies);
		}
		return true;
	}

	bool WebStreamingTransport::Cancel(RequestId id)
	{
		auto& m = *m_impl;
		auto it = m.requests.find(id);
		if (it == m.requests.end())
		{
			return false;
		}
		auto& r = it->second;
		r.state = State::Cancelled;
		m.DropSnapshot(r);
		const auto text = r.snapshotCharge, dependencies = r.dependencyCharge;
		std::string{}.swap(r.snapshotText);
		std::vector<std::string>{}.swap(r.assets);
		r.snapshotCharge = 0;
		r.dependencyCharge = 0;
		if (text)
		{
			m.budget->Release(MemoryClass::CpuPending, text);
		}
		if (dependencies)
		{
			m.budget->Release(MemoryClass::CpuResident, dependencies);
		}
		return true;
	}

	bool WebStreamingTransport::SetPriority(RequestId id, io::IOPriority p)
	{
		auto it = m_impl->requests.find(id);
		if (it == m_impl->requests.end())
		{
			return false;
		}
		it->second.priority = p;
		return true;
	}

	WebStreamingTransport::State WebStreamingTransport::GetState(RequestId id) const
	{
		auto it = m_impl->requests.find(id);
		return it == m_impl->requests.end() ? State::Unknown : it->second.state;
	}

	std::string WebStreamingTransport::GetError(RequestId id) const
	{
		auto it = m_impl->requests.find(id);
		return it == m_impl->requests.end() ? std::string{} : it->second.error;
	}

	void WebStreamingTransport::Update()
	{
		auto& m = *m_impl;
		if (!m.budget)
		{
			return;
		}
		if (!m.manifest && m.error.empty())
		{
			if (char* raw = ae_stream_manifest())
			{
				auto doc = json::parse(raw, nullptr, false);
				std::free(raw);
				if (doc.is_discarded() || !doc.is_object() || !doc.contains("schemaVersion") || !doc["schemaVersion"].is_number_integer() || doc["schemaVersion"] != 1 || !doc.contains("assets") || !doc["assets"].is_object()
				        || !doc.contains("aliases") || !doc["aliases"].is_object())
				{
					m.error = "Malformed streaming manifest";
				}
				else if (doc["assets"].size() > 4096 || doc["aliases"].size() > 8192)
				{
					m.error = "Streaming manifest exceeds metadata limits";
				}
				else
				{
					std::size_t dependencyCount = 0;
					for (auto it = doc["assets"].begin(); it != doc["assets"].end(); ++it)
					{
						const auto& v = it.value();
						if (!v.is_object() || !v.contains("url") || !v["url"].is_string() || !v.contains("sha256") || !v["sha256"].is_string() || !v.contains("size") || !v["size"].is_number_unsigned() || !v.contains("dependencies")
						        || !v["dependencies"].is_array())
						{
							m.error = "Malformed streaming asset";
							break;
						}
						Impl::Asset a;
						a.url = v["url"].get<std::string>();
						a.hash = v["sha256"].get<std::string>();
						a.size = v["size"].get<std::uint64_t>();
						dependencyCount += v["dependencies"].size();
						if (dependencyCount > 16384)
						{
							m.error = "Streaming manifest exceeds dependency limit";
							break;
						}
						for (const auto& dep: v["dependencies"])
						{
							if (!dep.is_string() || dep.get_ref<const std::string&>().size() > 4096)
							{
								m.error = "Invalid streaming dependency";
								break;
							}
							a.deps.push_back(dep.get<std::string>());
						}
						if (!m.error.empty())
						{
							break;
						}
						if (it.key().size() > 4096 || !it.key().starts_with("project://") || it.key().find("..") != std::string::npos || a.url != "streaming/" + a.hash || a.hash.size() != 64
						        || !std::all_of(a.hash.begin(), a.hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) || a.size > 64ull * 1024 * 1024)
						{
							m.error = "Invalid streaming asset";
							break;
						}
						m.assets.emplace(it.key(), std::move(a));
					}
					if (m.error.empty())
					{
						for (auto it = doc["aliases"].begin(); it != doc["aliases"].end(); ++it)
						{
							if (!it.value().is_string() || it.key().size() > 4096 || it.value().get_ref<const std::string&>().size() > 4096)
							{
								m.error = "Invalid streaming alias";
								break;
							}
							m.aliases.emplace(it.key(), it.value().get<std::string>());
						}
					}
					m.manifest = m.error.empty();
				}
				doc = nullptr;
				if (!m.manifest)
				{
					m.assets.clear();
					m.aliases.clear();
				}
				m.budget->Release(MemoryClass::Staging, kManifestStaging);
				m.budget->Release(MemoryClass::CpuPending, kManifestPending - kManifestResident);
				if (m.manifest && m.budget->Transfer(MemoryClass::CpuPending, MemoryClass::CpuResident, kManifestResident))
				{
					m.manifestResident = true;
				}
				else
				{
					m.assets.clear();
					m.aliases.clear();
					m.manifest = false;
					m.budget->Release(MemoryClass::CpuPending, kManifestResident);
					if (m.error.empty())
					{
						m.error = "Streaming manifest exceeds resident budget";
					}
				}
				m.manifestPending = false;
			}
		}
		if (!m.error.empty())
		{
			for (auto& [id, r]: m.requests)
			{
				if (r.state != State::Cancelled)
				{
					r.state = State::Failed;
					r.error = m.error;
				}
			}
			return;
		}
		if (!m.manifest)
		{
			return;
		}
		for (auto& [id, r]: m.requests)
		{
			if (!r.snapshot && r.assets.empty() && r.state != State::Failed && r.state != State::Cancelled)
			{
				std::string root = r.reference;
				if (auto it = m.aliases.find(root); it != m.aliases.end())
				{
					root = it->second;
				}
				std::vector<std::string> stack{root};
				std::unordered_set<std::string> seen;
				while (!stack.empty())
				{
					auto p = std::move(stack.back());
					stack.pop_back();
					if (!seen.insert(p).second)
					{
						continue;
					}
					auto it = m.assets.find(p);
					if (it == m.assets.end())
					{
						r.state = State::Failed;
						r.error = "Missing streaming dependency: " + p;
						break;
					}
					r.assets.push_back(p);
					for (auto& d: it->second.deps)
					{
						stack.push_back(d);
					}
				}
			}
		}
		std::unordered_map<std::string, io::IOPriority> wanted;
		for (auto& [id, r]: m.requests)
		{
			if (r.state != State::Failed && r.state != State::Cancelled)
			{
				for (auto& p: r.assets)
				{
					auto [it, inserted] = wanted.emplace(p, r.priority);
					if (!inserted && r.priority < it->second)
					{
						it->second = r.priority;
					}
				}
			}
		}
		for (auto& [p, a]: m.assets)
		{
			if (a.mounted && !wanted.contains(p))
			{
				UnmountWebAsset(p);
				if (!a.preparedPath.empty())
				{
					UnmountWebAsset(a.preparedPath);
				}
				if (!a.mipsPath.empty())
				{
					UnmountWebAsset(a.mipsPath);
				}
				m.budget->Release(MemoryClass::CpuResident, a.size + a.preparedSize);
				a.mounted = false;
				a.preparedSize = 0;
				a.preparedPath.clear();
				a.mipsPath.clear();
			}
		}
		if (!m.inFlight.empty() && !wanted.contains(m.inFlight) && !m.cancelled)
		{
			ae_stream_cancel();
			m.cancelled = true;
		}
		if (!m.inFlight.empty())
		{
			if (int result = ae_stream_result())
			{
				auto& a = m.assets.at(m.inFlight);
				if (result == 2)
				{
					const auto amount = static_cast<std::uint64_t>(ae_stream_prepared_size());
					const auto base = static_cast<std::uint64_t>(ae_stream_base_size()), mips = static_cast<std::uint64_t>(ae_stream_mips_size());
					const auto limits = m.budget->Limits();
					std::uint64_t input = 0;
					if (m.inFlight.ends_with(".mesh"))
					{
						for (const auto& dep: a.deps)
						{
							input += m.assets.at(dep).size;
						}
					}
					if (m.cancelled || base > amount || mips != amount - base || amount > limits.stagingBytes / 3 || input > limits.stagingBytes - amount * 3 || amount > limits.cpuBytes / 4)
					{
						if (!m.cancelled)
						{
							a.error = "Prepared asset exceeds streaming budget";
						}
						ae_stream_continue(0);
					}
					else if (m.budget->TryReserve(MemoryClass::CpuPending, amount))
					{
						if (m.budget->TryReserve(MemoryClass::Staging, amount * 3 + input))
						{
							m.preparingBytes = amount;
							m.preparingInput = input;
							m.preparingBase = base;
							m.preparingMips = mips;
							if (input)
							{
								for (const auto& dep: a.deps)
								{
									const auto bytes = FindWebAsset(dep);
									ae_stream_add_dependency(dep.c_str(), bytes.data(), static_cast<unsigned>(bytes.size()));
								}
							}
							ae_stream_continue(1);
						}
						else
						{
							m.budget->Release(MemoryClass::CpuPending, amount);
						}
					}
				}
				else
				{
					const auto total = a.size + m.preparingBytes;
					if (result > 0 && !m.cancelled && wanted.contains(m.inFlight))
					{
						std::vector<std::byte> bytes(static_cast<size_t>(a.size));
						std::vector<std::byte> prepared(static_cast<size_t>(m.preparingBase));
						std::vector<std::byte> mips(static_cast<size_t>(m.preparingMips));
						ae_stream_copy(bytes.data(), prepared.data(), mips.data());
						auto mounted = MountWebAsset(m.inFlight, std::move(bytes));
						bool preparedMounted = false, mipsMounted = false;
						if (mounted && m.preparingBase)
						{
							if (m.inFlight.ends_with(".mesh"))
							{
								a.preparedPath = m.inFlight + ".prepared";
							}
							else
							{
								a.preparedPath = m.inFlight.substr(0, m.inFlight.rfind('.')) + ".texture";
							}
							preparedMounted = MountWebAsset(a.preparedPath, std::move(prepared)).has_value();
						}
						if (mounted && (!m.preparingBase || preparedMounted) && m.preparingMips)
						{
							a.mipsPath = (a.preparedPath.empty() ? m.inFlight : a.preparedPath) + ".mips";
							mipsMounted = MountWebAsset(a.mipsPath, std::move(mips)).has_value();
						}
						if (mounted && (!m.preparingBase || preparedMounted) && (!m.preparingMips || mipsMounted) && m.budget->Transfer(MemoryClass::CpuPending, MemoryClass::CpuResident, total))
						{
							a.mounted = true;
							a.preparedSize = m.preparingBytes;
						}
						else
						{
							if (mounted)
							{
								UnmountWebAsset(m.inFlight);
							}
							if (preparedMounted)
							{
								UnmountWebAsset(a.preparedPath);
							}
							if (mipsMounted)
							{
								UnmountWebAsset(a.mipsPath);
							}
							a.preparedPath.clear();
							a.mipsPath.clear();
							a.error = "Could not mount prepared streaming asset";
							m.budget->Release(MemoryClass::CpuPending, total);
						}
					}
					else
					{
						if (result < 0 && !m.cancelled && a.error.empty())
						{
							char* e = ae_stream_error();
							a.error = e;
							std::free(e);
						}
						m.budget->Release(MemoryClass::CpuPending, total);
					}
					ae_stream_discard();
					m.budget->Release(MemoryClass::Staging, a.size * 2 + m.preparingBytes * 3 + m.preparingInput);
					m.preparingBytes = 0;
					m.preparingInput = 0;
					m.preparingBase = 0;
					m.preparingMips = 0;
					m.inFlight.clear();
					m.cancelled = false;
				}
			}
		}
		for (auto& [id, r]: m.requests)
		{
			if (r.state != State::Failed && r.state != State::Cancelled)
			{
				r.state = r.snapshot && !r.snapshotParsed ? State::Loading : State::Loaded;
				for (auto& p: r.assets)
				{
					auto& a = m.assets.at(p);
					if (!a.error.empty())
					{
						r.state = State::Failed;
						r.error = a.error;
						break;
					}
					if (!a.mounted)
					{
						r.state = State::Loading;
					}
				}
			}
		}
		std::unordered_map<std::string, io::IOPriority> sceneWanted;
		for (const auto& [id, r]: m.requests)
		{
			if (r.state != State::Failed && r.state != State::Cancelled && (r.snapshot || !r.assets.empty()))
			{
				const auto& root = r.snapshot ? r.snapshotPath : r.assets.front();
				auto [it, added] = sceneWanted.emplace(root, r.priority);
				if (!added && r.priority < it->second)
				{
					it->second = r.priority;
				}
			}
		}
		for (auto it = m.preparedScenes.begin(); it != m.preparedScenes.end();)
		{
			if (!sceneWanted.contains(it->first))
			{
				UnmountWebAsset(it->first + ".prepared-scene");
				m.budget->Release(MemoryClass::CpuResident, it->second);
				it = m.preparedScenes.erase(it);
			}
			else
			{
				++it;
			}
		}
		if (!m.sceneInFlight.empty() && !sceneWanted.contains(m.sceneInFlight))
		{
			ae_scene_close();
			m.budget->Release(MemoryClass::CpuPending, m.scenePending);
			m.budget->Release(MemoryClass::Staging, m.sceneStaging);
			m.sceneInFlight.clear();
			m.scenePending = 0;
			m.sceneStaging = 0;
		}
		if (!m.sceneInFlight.empty())
		{
			if (const int status = ae_scene_result())
			{
				const auto root = m.sceneInFlight;
				bool committed = false;
				if (status > 0)
				{
					Impl::Request* snapshot = nullptr;
					for (auto& [id, r]: m.requests)
					{
						if (r.snapshot && r.snapshotPath == root && r.state != State::Cancelled)
						{
							snapshot = &r;
							break;
						}
					}
					std::vector<std::string> closure;
					std::uint64_t dependencyCharge = 0;
					bool valid = true;
					if (snapshot)
					{
						const auto dependencySize = ae_scene_dependencies_size();
						if (dependencySize < 4 || dependencySize > 1024u * 1024u)
						{
							valid = false;
						}
						else
						{
							std::vector<std::byte> packed(dependencySize);
							ae_scene_copy_dependencies(packed.data());
							std::size_t offset = 0;
							const auto read32 = [&](std::uint32_t& value)
							{
								if (offset + 4 > packed.size())
								{
									return false;
								}
								value = 0;
								for (unsigned i = 0; i < 4; ++i)
								{
									value |= std::uint32_t(std::to_integer<unsigned char>(packed[offset++])) << (i * 8);
								}
								return true;
							};
							std::uint32_t count = 0;
							if (!read32(count) || count > 65536)
							{
								valid = false;
							}
							std::vector<std::string> stack;
							for (std::uint32_t i = 0; valid && i < count; ++i)
							{
								std::uint32_t length = 0;
								if (!read32(length) || length > 4096 || length > packed.size() - offset)
								{
									valid = false;
									break;
								}
								std::string path(reinterpret_cast<const char*>(packed.data() + offset), length);
								offset += length;
								if (path.starts_with("engine://"))
								{
									continue;
								}
								stack.push_back(std::move(path));
							}
							if (offset != packed.size())
							{
								valid = false;
							}
							std::unordered_set<std::string> seen;
							while (valid && !stack.empty())
							{
								auto path = std::move(stack.back());
								stack.pop_back();
								if (auto alias = m.aliases.find(path); alias != m.aliases.end())
								{
									path = alias->second;
								}
								if (!seen.insert(path).second)
								{
									continue;
								}
								const auto found = m.assets.find(path);
								if (found == m.assets.end())
								{
									m.sceneErrors[root] = "Snapshot asset missing from streaming manifest: " + path;
									valid = false;
									break;
								}
								dependencyCharge += path.size() * 2 + 128;
								if (dependencyCharge > kManifestResident)
								{
									m.sceneErrors[root] = "Snapshot asset closure exceeds budget";
									valid = false;
									break;
								}
								closure.push_back(path);
								for (const auto& dependency: found->second.deps)
								{
									stack.push_back(dependency);
								}
							}
						}
						if (!valid && !m.sceneErrors.contains(root))
						{
							m.sceneErrors[root] = "Invalid snapshot asset dependencies";
						}
					}
					const auto size = static_cast<std::uint64_t>(ae_scene_size());
					if (valid && size <= m.sceneOutputLimit && size + dependencyCharge <= m.scenePending)
					{
						std::vector<std::byte> bytes(static_cast<std::size_t>(size));
						ae_scene_copy(bytes.data());
						auto mounted = MountWebAsset(root + ".prepared-scene", std::move(bytes));
						if (mounted && m.budget->Transfer(MemoryClass::CpuPending, MemoryClass::CpuResident, size + dependencyCharge))
						{
							m.preparedScenes.emplace(root, size);
							if (snapshot)
							{
								snapshot->assets = std::move(closure);
								snapshot->dependencyCharge = dependencyCharge;
								snapshot->snapshotParsed = true;
							}
							m.budget->Release(MemoryClass::CpuPending, m.scenePending - size - dependencyCharge);
							committed = true;
						}
						else if (mounted)
						{
							UnmountWebAsset(root + ".prepared-scene");
						}
					}
					if (!committed && !m.sceneErrors.contains(root))
					{
						m.sceneErrors[root] = "Could not publish prepared scene";
					}
				}
				else
				{
					char* text = ae_scene_error();
					m.sceneErrors[root] = text;
					std::free(text);
				}
				ae_scene_close();
				if (!committed)
				{
					m.budget->Release(MemoryClass::CpuPending, m.scenePending);
				}
				m.budget->Release(MemoryClass::Staging, m.sceneStaging);
				m.sceneInFlight.clear();
				m.scenePending = 0;
				m.sceneStaging = 0;
			}
		}
		if (m.sceneInFlight.empty())
		{
			Impl::Request* selected = nullptr;
			for (auto& [id, r]: m.requests)
			{
				const bool eligible = r.snapshot ? (!r.snapshotParsed && !r.snapshotText.empty() && r.state == State::Loading) : (r.state == State::Loaded && !r.assets.empty());
				if (!eligible)
				{
					continue;
				}
				const auto& root = r.snapshot ? r.snapshotPath : r.assets.front();
				if (!m.preparedScenes.contains(root) && !m.sceneErrors.contains(root) && (!selected || r.priority < selected->priority))
				{
					selected = &r;
				}
			}
			if (selected)
			{
				constexpr auto catalogPath = "project://asset-catalog.toml";
				std::vector<std::byte> catalog;
				std::uint64_t catalogPending = 0, catalogStaging = 0;
				bool catalogBlocked = false, catalogFailed = false;
				if (io::FileSystem::Exists(catalogPath))
				{
					auto bytes = io::FileSystem::ReadFileReserved(catalogPath,
					        [&](std::size_t size)
					        {
						        if (size > 4u * 1024u * 1024u)
						        {
							        catalogFailed = true;
							        return false;
						        }
						        catalogPending = static_cast<std::uint64_t>(size) * 16;
						        catalogStaging = static_cast<std::uint64_t>(size) * 4;
						        if (!m.budget->TryReserve(MemoryClass::CpuPending, catalogPending))
						        {
							        catalogBlocked = true;
							        catalogPending = 0;
							        catalogStaging = 0;
							        return false;
						        }
						        if (!m.budget->TryReserve(MemoryClass::Staging, catalogStaging))
						        {
							        m.budget->Release(MemoryClass::CpuPending, catalogPending);
							        catalogBlocked = true;
							        catalogPending = 0;
							        catalogStaging = 0;
							        return false;
						        }
						        return true;
					        });
					if (bytes)
					{
						catalog = std::move(*bytes);
					}
					else if (!catalogBlocked)
					{
						catalogFailed = true;
					}
				}
				const auto& selectedRoot = selected->snapshot ? selected->snapshotPath : selected->assets.front();
				if (catalogFailed)
				{
					m.sceneErrors[selectedRoot] = "Could not read asset catalog for scene preparation";
				}
				if (catalogBlocked || catalogFailed)
				{
					if (catalogPending)
					{
						m.budget->Release(MemoryClass::CpuPending, catalogPending);
					}
					if (catalogStaging)
					{
						m.budget->Release(MemoryClass::Staging, catalogStaging);
					}
					if (catalogBlocked)
					{
						selected->state = State::Blocked;
					}
				}
				else
				{
					std::uint64_t docs = selected->snapshot ? selected->snapshotText.size() : 0, aliases = 0;
					if (!selected->snapshot)
					{
						for (const auto& path: selected->assets)
						{
							if (path.ends_with(".scene.toml") || path.ends_with(".prefab.toml") || path.ends_with(".scene.bin") || path.ends_with(".prefab.bin"))
							{
								docs += m.assets.at(path).size;
							}
						}
					}
					for (const auto& [alias, path]: m.aliases)
					{
						if (alias.starts_with("uid://"))
						{
							aliases += alias.size() + path.size() + 32;
						}
					}
					const auto limits = m.budget->Limits();
					const auto usedStaging = m.budget->Bytes(MemoryClass::Staging);
					const auto usedCpu = m.budget->Bytes(MemoryClass::CpuResident) + m.budget->Bytes(MemoryClass::CpuPending) + m.budget->Bytes(MemoryClass::CpuRetiring) + usedStaging;
					const auto availableCpu = limits.cpuBytes > usedCpu ? limits.cpuBytes - usedCpu : 0;
					const auto availableStaging = limits.stagingBytes > usedStaging ? limits.stagingBytes - usedStaging : 0;
					const auto workspace = docs * 16 + aliases * 2 + (selected->snapshot ? kManifestResident : 0), copyBytes = docs * 3 + aliases * 4 + (selected->snapshot ? 3ull * 1024 * 1024 : 0);
					const auto outputLimit = availableCpu > workspace + copyBytes && availableStaging > copyBytes ? std::min<std::uint64_t>({32ull * 1024 * 1024, (availableCpu - workspace - copyBytes) / 4, (availableStaging - copyBytes) / 3}) : 0;
					const auto pending = workspace + outputLimit, staging = copyBytes + outputLimit * 3;
					if (outputLimit >= 256ull * 1024 && m.budget->TryReserve(MemoryClass::CpuPending, pending))
					{
						if (m.budget->TryReserve(MemoryClass::Staging, staging))
						{
							m.sceneInFlight = selectedRoot;
							m.scenePending = pending + catalogPending;
							m.sceneStaging = staging + catalogStaging;
							m.sceneOutputLimit = outputLimit;
							ae_scene_open();
							if (io::FileSystem::Exists(catalogPath))
							{
								ae_scene_add_file(catalogPath, catalog.data(), static_cast<unsigned>(catalog.size()));
							}
							if (selected->snapshot)
							{
								ae_scene_add_file(selected->snapshotPath.c_str(), selected->snapshotText.data(), static_cast<unsigned>(selected->snapshotText.size()));
								const auto charge = selected->snapshotCharge;
								std::string{}.swap(selected->snapshotText);
								selected->snapshotCharge = 0;
								m.budget->Release(MemoryClass::CpuPending, charge);
							}
							else
							{
								for (const auto& path: selected->assets)
								{
									if (path.ends_with(".scene.toml") || path.ends_with(".prefab.toml") || path.ends_with(".scene.bin") || path.ends_with(".prefab.bin"))
									{
										const auto bytes = FindWebAsset(path);
										ae_scene_add_file(path.c_str(), bytes.data(), static_cast<unsigned>(bytes.size()));
									}
								}
							}
							for (const auto& [alias, path]: m.aliases)
							{
								if (alias.starts_with("uid://"))
								{
									ae_scene_add_alias(alias.c_str(), path.c_str());
								}
							}
							ae_scene_start(m.sceneInFlight.c_str(), static_cast<unsigned>(outputLimit), selected->snapshot ? 1 : 0);
						}
						else
						{
							m.budget->Release(MemoryClass::CpuPending, pending);
						}
					}
					if (m.sceneInFlight.empty())
					{
						if (catalogPending)
						{
							m.budget->Release(MemoryClass::CpuPending, catalogPending);
						}
						if (catalogStaging)
						{
							m.budget->Release(MemoryClass::Staging, catalogStaging);
						}
					}
				}
			}
		}
		for (auto& [id, r]: m.requests)
		{
			if (r.state != State::Failed && r.state != State::Cancelled && (r.snapshot || !r.assets.empty()))
			{
				const auto& root = r.snapshot ? r.snapshotPath : r.assets.front();
				if (auto it = m.sceneErrors.find(root); it != m.sceneErrors.end())
				{
					r.state = State::Failed;
					r.error = it->second;
				}
				else if (r.state == State::Loaded && !m.preparedScenes.contains(root))
				{
					r.state = State::Loading;
				}
			}
		}
		if (m.inFlight.empty())
		{
			std::string candidate;
			io::IOPriority priority = io::IOPriority::Background;
			for (auto& [p, pr]: wanted)
			{
				auto& a = m.assets.at(p);
				bool ready = true;
				if (p.ends_with(".mesh"))
				{
					for (const auto& dep: a.deps)
					{
						if (!m.assets.at(dep).mounted)
						{
							ready = false;
							break;
						}
					}
				}
				if (ready && !a.mounted && a.error.empty() && (candidate.empty() || pr < priority))
				{
					candidate = p;
					priority = pr;
				}
			}
			if (!candidate.empty())
			{
				auto& a = m.assets.at(candidate);
				const auto limits = m.budget->Limits();
				if (a.size > limits.stagingBytes / 2 || a.size > limits.cpuBytes / 3)
				{
					a.error = "Streaming asset exceeds transfer budget";
				}
				else if (m.budget->TryReserve(MemoryClass::CpuPending, a.size))
				{
					if (m.budget->TryReserve(MemoryClass::Staging, a.size * 2))
					{
						std::uint64_t deps = 0;
						if (candidate.ends_with(".mesh"))
						{
							for (const auto& dep: a.deps)
							{
								deps += m.assets.at(dep).size;
							}
						}
						m.inFlight = candidate;
						ae_stream_fetch(a.url.c_str(), static_cast<double>(a.size), a.hash.c_str(), candidate.c_str(), static_cast<double>(deps));
					}
					else
					{
						m.budget->Release(MemoryClass::CpuPending, a.size);
					}
				}
				if (m.inFlight.empty())
				{
					for (auto& [id, r]: m.requests)
					{
						if (r.state == State::Loading && std::find(r.assets.begin(), r.assets.end(), candidate) != r.assets.end())
						{
							r.state = State::Blocked;
						}
					}
				}
			}
		}
	}

	void WebStreamingTransport::Shutdown()
	{
		auto& m = *m_impl;
		if (!m.budget)
		{
			return;
		}
		if (!m.sceneInFlight.empty())
		{
			ae_scene_close();
			m.budget->Release(MemoryClass::CpuPending, m.scenePending);
			m.budget->Release(MemoryClass::Staging, m.sceneStaging);
		}
		ae_stream_close();
		if (m.manifestPending)
		{
			m.budget->Release(MemoryClass::CpuPending, kManifestPending);
			m.budget->Release(MemoryClass::Staging, kManifestStaging);
		}
		if (m.manifestResident)
		{
			m.budget->Release(MemoryClass::CpuResident, kManifestResident);
		}
		if (!m.inFlight.empty())
		{
			auto n = m.assets.at(m.inFlight).size;
			m.budget->Release(MemoryClass::CpuPending, n + m.preparingBytes);
			m.budget->Release(MemoryClass::Staging, n * 2 + m.preparingBytes * 3 + m.preparingInput);
		}
		for (auto& [id, r]: m.requests)
		{
			const auto text = r.snapshotCharge, dependencies = r.dependencyCharge;
			std::string{}.swap(r.snapshotText);
			std::vector<std::string>{}.swap(r.assets);
			if (text)
			{
				m.budget->Release(MemoryClass::CpuPending, text);
			}
			if (dependencies)
			{
				m.budget->Release(MemoryClass::CpuResident, dependencies);
			}
		}
		for (const auto& [path, size]: m.preparedScenes)
		{
			UnmountWebAsset(path + ".prepared-scene");
			m.budget->Release(MemoryClass::CpuResident, size);
		}
		for (auto& [p, a]: m.assets)
		{
			if (a.mounted)
			{
				UnmountWebAsset(p);
				if (!a.preparedPath.empty())
				{
					UnmountWebAsset(a.preparedPath);
				}
				if (!a.mipsPath.empty())
				{
					UnmountWebAsset(a.mipsPath);
				}
				m.budget->Release(MemoryClass::CpuResident, a.size + a.preparedSize);
			}
		}
		m_impl = std::make_unique<Impl>();
	}

	std::string WebStreamingTransport::GetDiagnosticsJson() const
	{
		auto& m = *m_impl;
		json rows = json::array();
		for (auto& [p, a]: m.assets)
		{
			rows.push_back({{"reference", p}, {"bytes", a.size}, {"state", a.mounted ? "loaded" : m.inFlight == p ? "loading" : "unloaded"}, {"error", a.error}, {"dependencies", a.deps}});
		}
		json out = {{"assets", rows}, {"error", m.error}};
		out["sceneParserHeapInitialBytes"] = 16ull * 1024 * 1024;
		out["sceneParserHeapMaximumBytes"] = 128ull * 1024 * 1024;
		out["sceneParserHeapExcludedFromLedger"] = true;
		out["mipWorkerHeapInitialBytes"] = 16ull * 1024 * 1024;
		out["mipWorkerHeapMaximumBytes"] = 128ull * 1024 * 1024;
		out["mipWorkerHeapExcludedFromLedger"] = true;
		if (m.budget)
		{
			const auto limits = m.budget->Limits();
			out["cpuBudgetBytes"] = limits.cpuBytes;
			out["gpuBudgetBytes"] = limits.gpuBytes;
			out["stagingBudgetBytes"] = limits.stagingBytes;
			out["cpuResidentBytes"] = m.budget->Bytes(MemoryClass::CpuResident);
			out["cpuPendingBytes"] = m.budget->Bytes(MemoryClass::CpuPending);
			out["cpuRetiringBytes"] = m.budget->Bytes(MemoryClass::CpuRetiring);
			out["gpuResidentBytes"] = m.budget->Bytes(MemoryClass::GpuResident);
			out["gpuPendingBytes"] = m.budget->Bytes(MemoryClass::GpuPending);
			out["gpuRetiringBytes"] = m.budget->Bytes(MemoryClass::GpuRetiring);
			out["stagingBytes"] = m.budget->Bytes(MemoryClass::Staging);
		}
		return out.dump();
	}
} // namespace aether::web

#include "editor/ControlServer.hpp"

#include "platform/Window.hpp"

#include <exception>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>
#include <nlohmann/json.hpp>

#include "editor/ControlMethods.hpp"
#include "net/EnetInit.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	using nlohmann::json;
	using aether::net::AcquireEnet;
	using aether::net::ReleaseEnet;

	struct ControlServer::Impl
	{
		ENetHost* host = nullptr;
		std::thread thread;
		std::atomic<bool> running{false};

		std::vector<ControlMethod> methods;

		struct Inbound
		{
			ENetPeer* peer = nullptr;
			std::uint64_t reqId = 0;
			std::string method;
			std::string params;
		};

		struct Outbound
		{
			ENetPeer* peer = nullptr;
			std::string payload;
		};

		std::mutex inMutex;
		std::queue<Inbound> inQueue;
		std::mutex outMutex;
		std::queue<Outbound> outQueue;
	};

	ControlServer::ControlServer(ServiceContainer& services, MethodBuilder methodBuilder, std::string endpointName)
	      : m_services(services), m_methodBuilder(std::move(methodBuilder)), m_endpointName(std::move(endpointName))
	{
	}

	ControlServer::~ControlServer()
	{
		Stop();
	}

	bool ControlServer::IsRunning() const noexcept
	{
		return m_impl != nullptr && m_impl->running.load();
	}

	void ControlServer::SetFrameInfo(std::uint64_t frameIndex, double fps) noexcept
	{
		m_frameIndex.store(frameIndex, std::memory_order_relaxed);
		m_fps.store(fps, std::memory_order_relaxed);
	}

	std::vector<ControlServer::RequestLogEntry> ControlServer::RecentRequests() const
	{
		const std::lock_guard<std::mutex> lock(m_logMutex);
		return {m_log.begin(), m_log.end()};
	}

	void ControlServer::Start(int port)
	{
		if (m_impl != nullptr)
		{
			return;
		}
		if (!AcquireEnet())
		{
			AE_ERROR(LogCategory::App, "ControlServer: enet_initialize failed; control endpoint disabled.");
			return;
		}

		ENetAddress address{};
		enet_address_set_host(&address, "127.0.0.1"); // loopback only - never off-box
		address.port = static_cast<enet_uint16>(port);

		ENetHost* host = enet_host_create(&address, 8 /*peers*/, 2 /*channels*/, 0, 0);
		if (host == nullptr)
		{
			AE_ERROR(LogCategory::App, "ControlServer: could not bind ENet host on 127.0.0.1:{} (in use?); control endpoint disabled.", port);
			ReleaseEnet();
			return;
		}

		m_impl = std::make_unique<Impl>();
		m_impl->host = host;
		m_impl->methods = m_methodBuilder();
		m_port = port;
		m_impl->running.store(true);
		m_impl->thread = std::thread([this]() { ServiceLoop(); });
		AE_INFO(LogCategory::App, "ControlServer: listening on enet://127.0.0.1:{} ({} methods; {} control endpoint).", port, m_impl->methods.size(), m_endpointName);
	}

	void ControlServer::Stop()
	{
		if (m_impl == nullptr)
		{
			return;
		}
		m_impl->running.store(false);
		if (m_impl->thread.joinable())
		{
			m_impl->thread.join();
		}
		if (m_impl->host != nullptr)
		{
			enet_host_destroy(m_impl->host);
		}
		m_impl.reset();
		m_connectedClients.store(0, std::memory_order_relaxed);
		ReleaseEnet();
		AE_INFO(LogCategory::App, "ControlServer: stopped.");
	}

	void ControlServer::ServiceLoop()
	{
		ENetEvent event;
		while (m_impl->running.load())
		{
			// Flush replies the main thread produced since the last iteration.
			std::queue<Impl::Outbound> outLocal;
			{
				const std::lock_guard<std::mutex> lock(m_impl->outMutex);
				std::swap(outLocal, m_impl->outQueue);
			}
			while (!outLocal.empty())
			{
				Impl::Outbound& o = outLocal.front();
				ENetPacket* packet = enet_packet_create(o.payload.data(), o.payload.size(), ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send(o.peer, 0, packet);
				outLocal.pop();
			}
			enet_host_flush(m_impl->host);

			while (m_impl->running.load() && enet_host_service(m_impl->host, &event, 20) > 0)
			{
				if (event.type == ENET_EVENT_TYPE_CONNECT)
				{
					m_connectedClients.fetch_add(1, std::memory_order_relaxed);
				}
				else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
				{
					m_connectedClients.fetch_sub(1, std::memory_order_relaxed);
				}
				else if (event.type == ENET_EVENT_TYPE_RECEIVE)
				{
					const std::string message(reinterpret_cast<const char*>(event.packet->data), event.packet->dataLength);
					json parsed = json::parse(message, nullptr, false);
					if (!parsed.is_discarded() && parsed.is_object())
					{
						Impl::Inbound in;
						in.peer = event.peer;
						in.reqId = parsed.value("id", static_cast<std::uint64_t>(0));
						in.method = parsed.value("method", std::string{});
						in.params = parsed.contains("params") ? parsed["params"].dump() : std::string("{}");
						const std::lock_guard<std::mutex> lock(m_impl->inMutex);
						m_impl->inQueue.push(std::move(in));
						// Commands are drained on the main loop thread, which may be parked in
						// the idle event wait. Without this an automated session would sit
						// behind the idle interval for every single call.
						aether::Window::PostEmptyEvent();
					}
					enet_packet_destroy(event.packet);
				}
			}
		}
	}

	void ControlServer::DrainCommands()
	{
		if (m_impl == nullptr)
		{
			return;
		}
		std::queue<Impl::Inbound> inLocal;
		{
			const std::lock_guard<std::mutex> lock(m_impl->inMutex);
			std::swap(inLocal, m_impl->inQueue);
		}
		while (!inLocal.empty())
		{
			const Impl::Inbound& in = inLocal.front();
			std::string resultBody;
			try
			{
				resultBody = Dispatch(in.method, in.params);
			}
			catch (const std::exception& e)
			{
				AE_WARN(LogCategory::App, "Control command '{}' threw: {}", in.method, e.what());
				resultBody = json{{"error", std::string("command threw: ") + e.what()}}.dump();
			}

			json envelope;
			envelope["id"] = in.reqId;
			const json result = json::parse(resultBody, nullptr, false);
			bool ok = true;
			if (result.is_discarded())
			{
				envelope["error"] = "handler produced malformed JSON";
				ok = false;
			}
			else if (result.is_object() && result.contains("error"))
			{
				envelope["error"] = result["error"];
				ok = false;
			}
			else
			{
				envelope["result"] = result;
			}

			m_requestCount.fetch_add(1, std::memory_order_relaxed);
			{
				const std::lock_guard<std::mutex> lock(m_logMutex);
				m_log.push_back(RequestLogEntry{in.method, ok});
				while (m_log.size() > kMaxLog)
				{
					m_log.pop_front();
				}
			}

			{
				const std::lock_guard<std::mutex> lock(m_impl->outMutex);
				m_impl->outQueue.push(Impl::Outbound{in.peer, envelope.dump()});
			}
			inLocal.pop();
		}
	}

	std::string ControlServer::Dispatch(const std::string& method, const std::string& paramsJson)
	{
		json params = json::parse(paramsJson, nullptr, false);
		if (params.is_discarded())
		{
			params = json::object();
		}

		if (method == "describe")
		{
			json arr = json::array();
			for (const ControlMethod& m: m_impl->methods)
			{
				arr.push_back(json{{"name", m.name}, {"tool", m.tool}, {"description", m.description}, {"mutates", m.mutates}, {"paramsSchema", m.paramsSchema}});
			}
			return json{{"methods", arr}}.dump();
		}

		for (const ControlMethod& m: m_impl->methods)
		{
			if (m.name == method)
			{
				// Enforce the schema's required fields BEFORE the handler runs. Handlers read declared
				// fields with the plain `p["key"]`, which on a const json asserts when the key is absent -
				// so a caller that merely misspelled an argument took the whole editor down with it. The
				// required list was already declared on every method and simply nobody checked it; doing
				// it here covers every method at once instead of hardening call sites one at a time.
				if (const auto req = m.paramsSchema.find("required"); req != m.paramsSchema.end() && req->is_array())
				{
					for (const auto& key: *req)
					{
						if (!key.is_string())
						{
							continue;
						}
						const auto name = key.get<std::string>();
						if (!params.is_object() || !params.contains(name))
						{
							return json{{"error", "missing required parameter: " + name}}.dump();
						}
					}
				}

				MethodContext ctx{m_services, m_frameIndex.load(std::memory_order_relaxed), m_fps.load(std::memory_order_relaxed)};
				return m.handler(params, ctx).dump();
			}
		}
		return json{{"error", std::string("unknown method: ") + method}}.dump();
	}
} // namespace aether::editor

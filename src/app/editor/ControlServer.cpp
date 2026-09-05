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

#include "editor/ControlDiagnostics.hpp"
#include "editor/ControlMethods.hpp"
#include "net/EnetInit.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace aether::editor
{
	using nlohmann::json;
	using aether::net::AcquireEnet;
	using aether::net::ReleaseEnet;

	namespace
	{
		// The inbound queue is drained once per editor frame; a local client sending
		// faster than that would otherwise grow it (two strings plus a params JSON per
		// entry) without bound. 64 is several frames' worth of any sane automation.
		constexpr std::size_t kMaxQueuedRequests = 64;
		// No legitimate control request is anywhere near this large; anything bigger
		// is a flood (or a dump of a hostile packet) and is refused before parsing,
		// where the parse itself is the cost being attacked.
		constexpr std::size_t kMaxRequestBytes = 256 * 1024;
	} // namespace

	struct ControlServer::Impl
	{
		ENetHost* host = nullptr;
		std::thread thread;
		std::atomic<bool> running{false};

		std::vector<ControlMethod> methods;

		struct Inbound
		{
			ENetPeer* peer = nullptr;
			// Identifies the CONNECTION, not the peer slot: a slot freed by a disconnect
			// is recycled by the next client, and a stale reply handed to the recycled
			// slot goes to the wrong client.
			enet_uint32 connectId = 0;
			std::uint64_t reqId = 0;
			std::string method;
			std::string params;
		};

		struct Outbound
		{
			ENetPeer* peer = nullptr;
			enet_uint32 connectId = 0;
			std::string payload;
		};

		// The ENet thread only parses and enqueues; every handler runs on the game
		// thread in DrainCommands, which posts its replies back through outQueue.
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
				// Peer slots are a fixed array inside the host, so the pointer stays valid,
				// but a slot freed by a disconnect can be recycled by a NEW connection.
				// connectID is unique per connection, so a mismatch (or a dead state) means
				// the client this reply belongs to is gone: drop it rather than deliver it
				// to whoever took the slot.
				if (o.peer != nullptr && o.peer->state == ENET_PEER_STATE_CONNECTED && o.peer->connectID == o.connectId)
				{
					ENetPacket* packet = enet_packet_create(o.payload.data(), o.payload.size(), ENET_PACKET_FLAG_RELIABLE);
					if (packet == nullptr)
					{
						AE_WARN(LogCategory::App, "ControlServer: could not allocate a reply packet; dropping a reply.");
					}
					else if (enet_peer_send(o.peer, 0, packet) < 0)
					{
						// ENet did not queue the packet, so ownership never left us - not
						// destroying it here leaked one packet per failed send.
						enet_packet_destroy(packet);
					}
				}
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
					// Untrusted wire data. Everything in here must answer with an error
					// reply, never with an exception: this thread has no handler, so an
					// escape (a nlohmann type_error, a bad allocation, anything a future
					// edit adds) is std::terminate for the whole editor.
					const auto refuse = [&](std::string_view why, bool hadId, const json& id)
					{
						AE_WARN(LogCategory::App, "ControlServer: dropping request: {}.", why);
						const std::lock_guard<std::mutex> outLock(m_impl->outMutex);
						m_impl->outQueue.push(Impl::Outbound{event.peer, event.peer != nullptr ? event.peer->connectID : 0,
						        json{{"id", hadId ? id : json{}}, {"error", why}}.dump()});
					};
					if (event.packet->dataLength > kMaxRequestBytes)
					{
						refuse("request too large", false, json{});
					}
					else
					try
					{
						const std::string message(reinterpret_cast<const char*>(event.packet->data), event.packet->dataLength);
						json parsed = json::parse(message, nullptr, false);
						// parsed.value() throws on a type mismatch, so the fields are
						// type-checked up front; anything else falls to the catch below.
						const bool hasId = parsed.is_object() && parsed.contains("id");
						const bool hasMethod = parsed.is_object() && parsed.contains("method");
						const bool idOk = !hasId || parsed["id"].is_number_integer();
						const bool methodOk = !hasMethod || parsed["method"].is_string();
						if (!parsed.is_discarded() && parsed.is_object() && idOk && methodOk)
						{
							const std::lock_guard<std::mutex> lock(m_impl->inMutex);
							// Drop (with an error reply) rather than queue when the game
							// thread is behind: an unbounded queue is the memory spike, and
							// a client that far ahead is flooding, not automating.
							if (m_impl->inQueue.size() >= kMaxQueuedRequests)
							{
								const std::uint64_t reqId = parsed.value("id", static_cast<std::uint64_t>(0));
								const std::lock_guard<std::mutex> outLock(m_impl->outMutex);
								m_impl->outQueue.push(Impl::Outbound{event.peer, event.peer != nullptr ? event.peer->connectID : 0,
								        json{{"id", reqId}, {"error", "server busy - request dropped"}}.dump()});
								enet_packet_destroy(event.packet);
								continue;
							}
							Impl::Inbound in;
							in.peer = event.peer;
							in.connectId = event.peer != nullptr ? event.peer->connectID : 0;
							in.reqId = parsed.value("id", static_cast<std::uint64_t>(0));
							in.method = parsed.value("method", std::string{});
							in.params = parsed.contains("params") ? parsed["params"].dump() : std::string("{}");
							const bool wasEmpty = m_impl->inQueue.empty();
							m_impl->inQueue.push(std::move(in));
							// Commands are drained on the main loop thread, which may be parked in
							// the idle event wait. Without this an automated session would sit
							// behind the idle interval for every single call. One post per
							// transition to non-empty is enough; one per request just lets a
							// flood pin the main loop awake.
							if (wasEmpty)
							{
								aether::Window::PostEmptyEvent();
							}
						}
						else
						{
							std::string why = "request is not valid JSON";
							if (!parsed.is_discarded() && !parsed.is_object())
							{
								why = "request must be a JSON object";
							}
							else if (!idOk)
							{
								why = "field 'id' must be an integer";
							}
							else if (!methodOk)
							{
								why = "field 'method' must be a string";
							}
							refuse(why, hasId, hasId ? parsed.at("id") : json{});
						}
					}
					catch (const std::exception& e)
					{
						AE_WARN(LogCategory::App, "ControlServer: dropping malformed request: {}.", e.what());
						const std::lock_guard<std::mutex> lock(m_impl->outMutex);
						m_impl->outQueue.push(Impl::Outbound{event.peer, event.peer != nullptr ? event.peer->connectID : 0, json{{"id", nullptr}, {"error", std::string("malformed request: ") + e.what()}}.dump()});
					}
					catch (...)
					{
						AE_WARN(LogCategory::App, "ControlServer: dropping a request that failed with an unknown error.");
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
			m_impl->outQueue.push(Impl::Outbound{in.peer, in.connectId, envelope.dump()});
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

				// And reject parameters the method does not declare. A misspelled or wrong-named
				// argument was silently dropped, so the handler ran with its default and the
				// caller read the answer to a question it never asked - a level filter that
				// filters nothing makes an error hunt look clean. Names starting with '_' are
				// left alone by convention for client metadata.
				if (const auto props = m.paramsSchema.find("properties");
				    props != m.paramsSchema.end() && props->is_object() && !props->empty() && params.is_object())
				{
					for (const auto& [key, value]: params.items())
					{
						if (key.starts_with('_'))
						{
							continue;
						}
						if (const auto declared = props->find(key); declared != props->end())
						{
							const auto type = declared->find("type");
							if (type != declared->end() && type->is_string() && !JsonMatchesDeclaredType(value, type->get<std::string>()))
							{
								return json{{"error", "parameter '" + key + "' of " + m.name + " must be " + type->get<std::string>() + ", got " + std::string(JsonTypeName(value))}}.dump();
							}
							continue;
						}
						std::string accepted;
						for (const auto& [name, ignored]: props->items())
						{
							accepted += accepted.empty() ? "" : ", ";
							accepted += name;
						}
						return json{{"error", "unknown parameter: " + key + " (accepts: " + accepted + ")"}}.dump();
					}
				}

				MethodContext ctx{m_services, m_frameIndex.load(std::memory_order_relaxed), m_fps.load(std::memory_order_relaxed)};
				return m.handler(params, ctx).dump();
			}
		}
		std::vector<MethodIdentity> known;
		known.reserve(m_impl->methods.size());
		for (const ControlMethod& m: m_impl->methods)
		{
			known.push_back(MethodIdentity{m.name, m.tool});
		}
		return json{{"error", DescribeUnknownMethod(method, known)}}.dump();
	}
} // namespace aether::editor

#include "net/NetworkSubsystem.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <chrono>
#include <emscripten/websocket.h>

namespace aether::net
{
	namespace
	{
		constexpr std::size_t kMaxPacketBytes = 64 * 1024;
		constexpr std::size_t kMaxQueueBytes = 1024 * 1024;
		constexpr std::size_t kMaxQueuedFrames = 1024;
		using Frame = std::vector<std::byte>;

		std::string JsonString(std::string_view value)
		{
			static constexpr char hex[] = "0123456789abcdef";
			std::string result = "\"";
			for (const unsigned char c: value)
			{
				if (c == '"' || c == '\\')
				{
					result += '\\';
					result += static_cast<char>(c);
				}
				else if (c < 0x20)
				{
					result += "\\u00";
					result += hex[c >> 4];
					result += hex[c & 15];
				}
				else
				{
					result += static_cast<char>(c);
				}
			}
			result += '"';
			return result;
		}

		Frame MakeFrame(int kind, int channel, bool reliable, ConnectionId peer, std::span<const std::byte> payload)
		{
			Frame frame(8 + payload.size());
			auto* p = reinterpret_cast<unsigned char*>(frame.data());
			p[0] = 1;
			p[1] = static_cast<unsigned char>(kind);
			p[2] = static_cast<unsigned char>(channel);
			p[3] = reliable ? 1 : 0;
			for (int i = 0; i < 4; ++i)
			{
				p[4 + i] = static_cast<unsigned char>(peer >> (8 * i));
			}
			if (!payload.empty())
			{
				std::memcpy(p + 8, payload.data(), payload.size());
			}
			return frame;
		}
	} // namespace

	struct WebSocketState
	{
		EMSCRIPTEN_WEBSOCKET_T socket = 0;
		std::string hello;
		std::string error;
		std::deque<Frame> received;
		std::deque<Frame> pending;
		std::size_t receivedBytes = 0;
		std::size_t pendingBytes = 0;
		bool ready = false;
		bool closed = false;

		void Fail(const char* reason)
		{
			if (error.empty())
			{
				error = reason;
			}
			emscripten_websocket_close(socket, 1000, nullptr);
		}

		bool Send(Frame frame, bool required)
		{
			if (closed)
			{
				return false;
			}
			size_t buffered = 0;
			if (emscripten_websocket_get_buffered_amount(socket, &buffered) != EMSCRIPTEN_RESULT_SUCCESS || buffered + pendingBytes + frame.size() > kMaxQueueBytes || pending.size() >= kMaxQueuedFrames)
			{
				if (required)
				{
					Fail("Gateway send queue exceeded");
				}
				return false;
			}
			if (!ready)
			{
				pendingBytes += frame.size();
				pending.push_back(std::move(frame));
				return true;
			}
			if (emscripten_websocket_send_binary(socket, frame.data(), static_cast<std::uint32_t>(frame.size())) != EMSCRIPTEN_RESULT_SUCCESS)
			{
				if (required)
				{
					Fail("Gateway send failed");
				}
				return false;
			}
			return true;
		}

		static bool Open(int, const EmscriptenWebSocketOpenEvent*, void* user)
		{
			auto& state = *static_cast<WebSocketState*>(user);
			if (emscripten_websocket_send_utf8_text(state.socket, state.hello.c_str()) != EMSCRIPTEN_RESULT_SUCCESS)
			{
				state.Fail("Gateway handshake send failed");
			}
			state.hello.clear();
			return true;
		}

		static bool Message(int, const EmscriptenWebSocketMessageEvent* event, void* user)
		{
			auto& state = *static_cast<WebSocketState*>(user);
			if (event->isText && !state.ready)
			{
				// This is the gateway's fixed v1 acknowledgement, including Emscripten's NUL.
				constexpr char ready[] = "{\"version\":1,\"kind\":\"ready\"}";
				if (event->numBytes != sizeof(ready) || std::memcmp(event->data, ready, sizeof(ready)) != 0)
				{
					state.Fail("Invalid gateway handshake");
					return true;
				}
				state.ready = true;
				while (!state.pending.empty())
				{
					Frame frame = std::move(state.pending.front());
					state.pending.pop_front();
					state.pendingBytes -= frame.size();
					if (emscripten_websocket_send_binary(state.socket, frame.data(), static_cast<std::uint32_t>(frame.size())) != EMSCRIPTEN_RESULT_SUCCESS)
					{
						state.Fail("Gateway send failed");
						break;
					}
				}
				return true;
			}
			const auto* p = event->data;
			const auto size = event->numBytes;
			if (!state.ready || event->isText || size < 8 || size > kMaxPacketBytes + 8 || p[0] != 1 || p[2] >= kChannelCount || (p[3] & ~1u))
			{
				state.Fail("Invalid gateway frame");
				return true;
			}
			if ((p[1] != 1 && p[1] != 2 && p[1] != 3) || (p[1] != 3 && size != 8) || (p[1] == 3 && size == 8))
			{
				state.Fail("Invalid gateway event");
				return true;
			}
			if (state.receivedBytes + size > kMaxQueueBytes || state.received.size() >= kMaxQueuedFrames)
			{
				state.Fail("Gateway receive queue exceeded");
				return true;
			}
			Frame frame(size);
			std::memcpy(frame.data(), p, size);
			state.receivedBytes += size;
			state.received.push_back(std::move(frame));
			return true;
		}

		static bool Error(int, const EmscriptenWebSocketErrorEvent*, void* user)
		{
			auto& state = *static_cast<WebSocketState*>(user);
			if (state.error.empty())
			{
				state.error = "Gateway WebSocket error";
			}
			return true;
		}

		static bool Close(int, const EmscriptenWebSocketCloseEvent*, void* user)
		{
			auto& state = *static_cast<WebSocketState*>(user);
			state.closed = true;
			if (state.error.empty())
			{
				state.error = "Gateway WebSocket closed";
			}
			return true;
		}

		~WebSocketState()
		{
			if (socket > 0)
			{
				emscripten_websocket_set_onopen_callback(socket, nullptr, nullptr);
				emscripten_websocket_set_onmessage_callback(socket, nullptr, nullptr);
				emscripten_websocket_set_onerror_callback(socket, nullptr, nullptr);
				emscripten_websocket_set_onclose_callback(socket, nullptr, nullptr);
				emscripten_websocket_close(socket, 1000, nullptr);
				emscripten_websocket_delete(socket);
			}
		}
	};

	namespace
	{
		std::shared_ptr<WebSocketState> OpenGateway(const std::string& url, const std::string& token, const std::string& address, bool host, std::uint16_t port, int maxPeers)
		{
			if (!emscripten_websocket_is_supported())
			{
				return {};
			}
			auto state = std::make_shared<WebSocketState>();
			state->hello = "{\"version\":1,\"role\":" + JsonString(host ? "host" : "client") + ",\"port\":" + std::to_string(port) + ",\"maxPeers\":" + std::to_string(maxPeers) + ",\"token\":" + JsonString(token);
			if (!address.empty())
			{
				const bool room = host || address.starts_with("room:");
				state->hello += room ? ",\"room\":" : ",\"target\":";
				state->hello += JsonString(room && !host ? std::string_view(address).substr(5) : std::string_view(address));
			}
			state->hello += '}';
			EmscriptenWebSocketCreateAttributes attributes{};
			attributes.url = url.c_str();
			state->socket = emscripten_websocket_new(&attributes);
			if (state->socket <= 0)
			{
				return {};
			}
			if (emscripten_websocket_set_onopen_callback(state->socket, state.get(), WebSocketState::Open) != EMSCRIPTEN_RESULT_SUCCESS
			        || emscripten_websocket_set_onmessage_callback(state->socket, state.get(), WebSocketState::Message) != EMSCRIPTEN_RESULT_SUCCESS
			        || emscripten_websocket_set_onerror_callback(state->socket, state.get(), WebSocketState::Error) != EMSCRIPTEN_RESULT_SUCCESS
			        || emscripten_websocket_set_onclose_callback(state->socket, state.get(), WebSocketState::Close) != EMSCRIPTEN_RESULT_SUCCESS)
			{
				return {};
			}
			return state;
		}
	} // namespace

	NetworkSubsystem::~NetworkSubsystem()
	{
		Disconnect();
	}

	void NetworkSubsystem::ConfigureWebGateway(std::string_view url, std::string_view token, std::string_view room, std::string_view targetAlias)
	{
		m_gatewayUrl = url;
		m_gatewayToken = token;
		m_gatewayRoom = room;
		m_gatewayTargetAlias = targetAlias;
	}

	void NetworkSubsystem::SetWebGatewayRoom(std::string_view room)
	{
		m_gatewayRoom = room;
	}

	bool NetworkSubsystem::WebGatewayOpen() const
	{
		return m_webSocket && m_webSocket->ready && !m_webSocket->closed;
	}

	bool NetworkSubsystem::Host(std::uint16_t port, int maxPeers)
	{
		Disconnect();
		m_lastError.clear();
		if (m_gatewayUrl.empty() || m_gatewayToken.empty() || maxPeers <= 0)
		{
			m_lastError = "Gateway URL, token and positive peer limit are required";
			return false;
		}
		m_webSocket = OpenGateway(m_gatewayUrl, m_gatewayToken, m_gatewayRoom, true, port, std::min(maxPeers, 64));
		if (!m_webSocket)
		{
			m_lastError = "Could not create gateway WebSocket";
			return false;
		}
		m_role = NetRole::Host;
		return true;
	}

	bool NetworkSubsystem::Connect(std::string_view host, std::uint16_t port)
	{
		Disconnect();
		m_lastError.clear();
		if (m_gatewayUrl.empty() || m_gatewayToken.empty() || (host.empty() && m_gatewayTargetAlias.empty()))
		{
			m_lastError = "Gateway URL, token and target alias or room are required";
			return false;
		}
		const std::string destination(host.empty() ? std::string_view(m_gatewayTargetAlias) : host);
		m_webSocket = OpenGateway(m_gatewayUrl, m_gatewayToken, destination, false, port, 1);
		if (!m_webSocket)
		{
			m_lastError = "Could not create gateway WebSocket";
			return false;
		}
		m_role = NetRole::Client;
		return true;
	}

	bool NetworkSubsystem::ConnectThrough(const NatTraversal::Endpoint&)
	{
		m_lastError = "Native NAT traversal is unavailable in a browser; use a gateway room";
		return false;
	}

	void NetworkSubsystem::Disconnect()
	{
		if (m_webSocket && m_webSocket->ready && !m_webSocket->closed)
		{
			m_webSocket->Send(MakeFrame(5, 0, true, 0, {}), true);
		}
		m_webSocket.reset();
		m_webPeers.clear();
		m_events.reset();
		m_role = NetRole::Offline;
		m_localId = kInvalidConnection;
	}

	void NetworkSubsystem::DisconnectPeer(ConnectionId peer)
	{
		if (m_role == NetRole::Host && m_webSocket && m_webPeers.contains(peer))
		{
			m_webSocket->Send(MakeFrame(4, 0, true, peer, {}), true);
		}
	}

	void NetworkSubsystem::Flush()
	{
	} // WebSocket.send queues bytes immediately.

	void NetworkSubsystem::Send(ConnectionId peer, int channel, bool reliable, std::span<const std::byte> bytes)
	{
		if (!m_webSocket || bytes.empty() || bytes.size() > kMaxPacketBytes || channel < 0 || channel >= kChannelCount || (m_role == NetRole::Host && !m_webPeers.contains(peer)))
		{
			return;
		}
		m_webSocket->Send(MakeFrame(3, channel, reliable, m_role == NetRole::Client ? 0 : peer, bytes), reliable);
	}

	void NetworkSubsystem::Broadcast(int channel, bool reliable, std::span<const std::byte> bytes)
	{
		if (m_role == NetRole::Host)
		{
			for (const auto peer: m_webPeers)
			{
				Send(peer, channel, reliable, bytes);
			}
		}
	}

	std::uint32_t NetworkSubsystem::RoundTripMs(ConnectionId) const
	{
		// The shared session measures the complete route instead.
		return 0;
	}

	void NetworkSubsystem::Poll()
	{
		m_events = std::make_shared<std::vector<NetEvent>>();
		if (!m_webSocket)
		{
			return;
		}
		auto& state = *m_webSocket;
		std::size_t serviced = 0;
		std::size_t bytes = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
		while (!state.received.empty() && serviced < kMaxPollEvents &&
			bytes + kMaxPacketBytes <= kMaxPollBytes && std::chrono::steady_clock::now() < deadline)
		{
			++serviced;
			Frame frame = std::move(state.received.front());
			state.received.pop_front();
			state.receivedBytes -= frame.size();
			const auto* p = reinterpret_cast<const unsigned char*>(frame.data());
			const ConnectionId peer = static_cast<ConnectionId>(p[4]) | (static_cast<ConnectionId>(p[5]) << 8) | (static_cast<ConnectionId>(p[6]) << 16) | (static_cast<ConnectionId>(p[7]) << 24);
			if ((m_role == NetRole::Client && peer != 0) || (m_role == NetRole::Host && peer == 0))
			{
				continue;
			}
			if (p[1] == 1)
			{
				if (m_webPeers.insert(peer).second)
				{
					m_events->push_back(NetEvent{.kind = NetEvent::Kind::Connected, .peer = peer});
				}
			}
			else if (p[1] == 2)
			{
				if (m_webPeers.erase(peer))
				{
					m_events->push_back(NetEvent{.kind = NetEvent::Kind::Disconnected, .peer = peer});
				}
			}
			else if (p[1] == 3 && m_webPeers.contains(peer))
			{
				bytes += frame.size() - 8;
				NetEvent event{.kind = NetEvent::Kind::Data, .peer = peer, .channel = p[2]};
				event.data.assign(frame.begin() + 8, frame.end());
				m_events->push_back(std::move(event));
			}
		}
		if (state.closed)
		{
			// The gateway owns the whole transport, not just one peer. Once it
			// closes, stop the session even if it failed before any peer connected.
			// Do not dispatch earlier frames under the now-offline role.
			m_events->clear(); // This Poll's new batch has not been published to callers yet.
			m_events->push_back(NetEvent{.kind = NetEvent::Kind::Disconnected, .peer = kInvalidConnection});
			m_webPeers.clear();
			m_lastError = state.error;
			m_webSocket.reset();
			m_role = NetRole::Offline;
			m_localId = kInvalidConnection;
			return;
		}
	}
} // namespace aether::net

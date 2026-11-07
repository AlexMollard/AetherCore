#include "net/ServerConnection.hpp"

#include <atomic>
#include <cstring>

#ifdef __EMSCRIPTEN__
#	include <emscripten.h>
#endif

namespace aether::net
{
	namespace
	{
		std::atomic<std::uint64_t> s_next_handle{1};

		bool ValidUtf8(std::string_view text)
		{
			for (std::size_t i = 0; i < text.size();)
			{
				const auto lead = static_cast<unsigned char>(text[i++]);
				if (lead == 0)
				{
					return false;
				}
				if (lead < 0x80)
				{
					continue;
				}
				const int count = lead >= 0xc2 && lead <= 0xdf ? 1 : lead >= 0xe0 && lead <= 0xef ? 2 : lead >= 0xf0 && lead <= 0xf4 ? 3 : -1;
				if (count < 0 || i + static_cast<std::size_t>(count) > text.size())
				{
					return false;
				}
				std::uint32_t point = lead & (0x7fu >> (count + 1));
				for (int j = 0; j < count; ++j)
				{
					const auto next = static_cast<unsigned char>(text[i++]);
					if ((next & 0xc0u) != 0x80u)
					{
						return false;
					}
					point = (point << 6) | (next & 0x3fu);
				}
				if ((count == 1 && point < 0x80) || (count == 2 && point < 0x800) || (count == 3 && point < 0x10000) || (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff)
				{
					return false;
				}
			}
			return true;
		}

#ifdef __EMSCRIPTEN__
		std::string BrowserSetting(const char* key)
		{
			const int length = EM_ASM_INT({
				const value = globalThis.aetherProject?.network?.[UTF8ToString($0)];
				return typeof value === 'string' ? lengthBytesUTF8(value) : 0;
			}, key);
			if (length <= 0 || length > 4096)
			{
				return {};
			}
			std::string value(static_cast<std::size_t>(length), '\0');
			EM_ASM({ stringToUTF8(globalThis.aetherProject.network[UTF8ToString($0)], $1, $2); }, key, value.data(), value.size() + 1);
			return value;
		}
#endif
	}

	ServerConnections::Handle ServerConnections::Create(std::uint64_t owner)
	{
		if (m_connections.size() >= kMaxConnections)
		{
			return 0;
		}
		const Handle handle = s_next_handle.fetch_add(1, std::memory_order_relaxed);
		if (handle == 0)
		{
			return 0;
		}
		auto connection = std::make_unique<Connection>();
		connection->owner = owner;
		m_connections.emplace(handle, std::move(connection));
		return handle;
	}

	ServerConnections::Connection* ServerConnections::Find(Handle handle)
	{
		const auto it = m_connections.find(handle);
		return it == m_connections.end() ? nullptr : it->second.get();
	}

	void ServerConnections::Release(Handle handle)
	{
		m_connections.erase(handle);
	}

	void ServerConnections::ReleaseOwner(std::uint64_t owner)
	{
		std::erase_if(m_connections, [owner](const auto& entry) { return entry.second->owner == owner; });
	}

	void ServerConnections::Clear()
	{
		m_connections.clear();
	}

	void ServerConnections::Stop(Connection& connection)
	{
		connection.transport.Disconnect();
		connection.connected = false;
		connection.received.clear();
		connection.receivedBytes = 0;
		connection.sendsSincePoll = 0;
	}

	bool ServerConnections::Connect(Handle handle, std::string_view host, int port, std::string_view gatewayUrl, std::string_view gatewayToken, std::string_view targetAlias)
	{
		auto* connection = Find(handle);
		if (connection == nullptr)
		{
			return false;
		}
		Stop(*connection);
		connection->error.clear();
#ifdef __EMSCRIPTEN__
		const int configuredPort = EM_ASM_INT({
			const value = globalThis.aetherProject?.network?.serverPort;
			return value === undefined ? 0 : Number.isInteger(value) && value >= 1 && value <= 65535 ? value : -1;
		});
		if (configuredPort < 0)
		{
			connection->error = "Browser network.serverPort must be an integer from 1 to 65535";
			return false;
		}
		port = ResolveServerPort(port, configuredPort);
#else
		port = ResolveServerPort(port, 0);
#endif
		if (port < 1 || port > 65535 || host.size() > 253 || !ValidUtf8(host) || host.empty())
		{
			connection->error = "Server host and port (1..65535) are required";
			return false;
		}
#ifdef __EMSCRIPTEN__
		const std::string url = gatewayUrl.empty() ? BrowserSetting("gatewayUrl") : std::string(gatewayUrl);
		const std::string token = gatewayToken.empty() ? BrowserSetting("authToken") : std::string(gatewayToken);
		const std::string alias = targetAlias.empty() ? BrowserSetting("targetAlias") : std::string(targetAlias);
		if (url.empty() || url.size() > 2048 || token.empty() || token.size() > 4096 || alias.empty() || alias.size() > 128 || !ValidUtf8(url) || !ValidUtf8(token) || !ValidUtf8(alias))
		{
			connection->error = "Browser gateway URL, token and target alias are required";
			return false;
		}
		connection->transport.ConfigureWebGateway(url, token, {}, alias);
		// Host/port describe the native endpoint; only an allowlisted alias goes to the gateway.
		const bool accepted = connection->transport.Connect({}, static_cast<std::uint16_t>(port));
#else
		(void) gatewayUrl;
		(void) gatewayToken;
		(void) targetAlias;
		const bool accepted = connection->transport.Connect(host, static_cast<std::uint16_t>(port));
#endif
		if (!accepted)
		{
			connection->error = connection->transport.LastError();
		}
		return accepted;
	}

	void ServerConnections::Poll(Handle handle)
	{
		auto* connection = Find(handle);
		if (connection == nullptr)
		{
			return;
		}
		connection->sendsSincePoll = 0;
		connection->transport.Poll();
		const auto events = connection->transport.RetainEvents();
		for (const auto& event : *events)
		{
			if (event.kind == NetEvent::Kind::Connected)
			{
				connection->connected = true;
			}
			else if (event.kind == NetEvent::Kind::Disconnected)
			{
				connection->error = "Server disconnected";
				Stop(*connection);
				return;
			}
			else
			{
				const std::string_view text(reinterpret_cast<const char*>(event.data.data()), event.data.size());
				if (text.empty() || text.size() > kMaxMessageBytes || !ValidUtf8(text))
				{
					connection->error = "Server sent invalid UTF-8 or an oversized message";
					Stop(*connection);
					return;
				}
				if (connection->received.size() >= kMaxQueuedMessages || connection->receivedBytes + text.size() > kMaxQueuedBytes)
				{
					connection->error = "Server receive queue limit exceeded";
					Stop(*connection);
					return;
				}
				connection->received.emplace_back(text);
				connection->receivedBytes += text.size();
			}
		}
		if (!connection->transport.LastError().empty())
		{
			connection->error = connection->transport.LastError();
			if (!connection->transport.IsActive())
			{
				Stop(*connection);
			}
		}
	}

	bool ServerConnections::Send(Handle handle, std::string_view message, bool reliable)
	{
		auto* connection = Find(handle);
		if (connection == nullptr)
		{
			return false;
		}
		if (message.empty() || message.size() > kMaxMessageBytes || !ValidUtf8(message))
		{
			connection->error = "Message must contain valid UTF-8 without NUL and be at most 65536 bytes";
			return false;
		}
		if (!connection->connected)
		{
			connection->error = "Server is not connected; poll until the connection completes";
			return false;
		}
		if (connection->sendsSincePoll >= kMaxQueuedMessages || connection->transport.QueuedSendBytes(0) + message.size() > kMaxQueuedBytes)
		{
			connection->error = "Server send queue limit exceeded";
			return false;
		}
		connection->transport.Send(0, reliable ? kChannelReliable : kChannelSnapshot, reliable, std::as_bytes(std::span(message)));
		++connection->sendsSincePoll;
		return true;
	}

	int ServerConnections::Receive(Handle handle, char* buffer, int capacity)
	{
		auto* connection = Find(handle);
		if (connection == nullptr || connection->received.empty())
		{
			return 0;
		}
		const auto& text = connection->received.front();
		if (buffer == nullptr)
		{
			return static_cast<int>(text.size());
		}
		if (capacity < 0 || static_cast<std::size_t>(capacity) < text.size())
		{
			return -1;
		}
		const int size = static_cast<int>(text.size());
		std::memcpy(buffer, text.data(), text.size());
		connection->receivedBytes -= text.size();
		connection->received.pop_front();
		return size;
	}

	void ServerConnections::Disconnect(Handle handle)
	{
		if (auto* connection = Find(handle))
		{
			Stop(*connection);
		}
	}

	bool ServerConnections::IsConnected(Handle handle) const
	{
		const auto it = m_connections.find(handle);
		return it != m_connections.end() && it->second->connected;
	}

	std::string ServerConnections::LastError(Handle handle) const
	{
		const auto it = m_connections.find(handle);
		return it == m_connections.end() ? "Server connection handle is no longer valid" : it->second->error;
	}
}

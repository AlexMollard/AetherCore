#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "net/NetworkSubsystem.hpp"

namespace aether::net
{
	// Main-thread, scene-owned transports for application protocols. Never enters NetSession or the ECS.
	class ServerConnections
	{
	public:
		using Handle = std::uint64_t;
		static constexpr std::size_t kMaxConnections = 64;
		static constexpr std::size_t kMaxMessageBytes = 64 * 1024;
		static constexpr std::size_t kMaxQueuedMessages = 64;
		static constexpr std::size_t kMaxQueuedBytes = 256 * 1024;
		// Configured zero means absent; negative values represent an explicitly invalid setting.
		static constexpr int ResolveServerPort(int requested, int configured)
		{
			const int port = configured == 0 ? requested : configured;
			return port >= 1 && port <= 65535 ? port : 0;
		}

		static constexpr std::uint64_t OwnerKey(std::uint32_t entity, std::uint32_t scriptIndex)
		{
			return (static_cast<std::uint64_t>(entity) << 32) | scriptIndex;
		}
		Handle Create(std::uint64_t owner = 0);
		void Release(Handle handle);
		void ReleaseOwner(std::uint64_t owner);
		void Clear();
		bool Connect(Handle handle, std::string_view host, int port, std::string_view gatewayUrl = {}, std::string_view gatewayToken = {}, std::string_view targetAlias = {});
		void Poll(Handle handle);
		bool Send(Handle handle, std::string_view message, bool reliable);
		// Null buffer queries the next byte count. An undersized buffer returns -1 and preserves the message.
		int Receive(Handle handle, char* buffer, int capacity);
		void Disconnect(Handle handle);
		[[nodiscard]] bool IsConnected(Handle handle) const;
		[[nodiscard]] std::string LastError(Handle handle) const;

	private:
		struct Connection
		{
			NetworkSubsystem transport;
			std::deque<std::string> received;
			std::size_t receivedBytes = 0;
			std::size_t sendsSincePoll = 0;
			std::uint64_t owner = 0;
			bool connected = false;
			std::string error;
		};
		Connection* Find(Handle handle);
		static void Stop(Connection& connection);
		std::unordered_map<Handle, std::unique_ptr<Connection>> m_connections;
	};
}

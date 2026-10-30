#include "net/NetTraversalSession.hpp"

#ifdef __EMSCRIPTEN__

#	include <utility>

#	include "net/RoomCode.hpp"

namespace aether::net
{
	NetTraversalSession::NetTraversalSession(NetworkSubsystem& transport)
	      : m_transport(transport)
	{
	}

	void NetTraversalSession::ConfigureSignaling(SignalingBackend backend, std::string address)
	{
		m_backend = backend;
		m_signalingAddress = std::move(address);
		m_signalingChosen = true;
		m_useInjectedChannel = false;
	}

	void NetTraversalSession::ConfigureSignalingChannel(ISignalingChannel&)
	{
		m_signalingChosen = true;
		m_useInjectedChannel = true;
	}

	void NetTraversalSession::SetRendezvousDefault(std::string host, std::uint16_t port)
	{
		if (!m_signalingChosen && !host.empty())
		{
			m_backend = SignalingBackend::Rendezvous;
			m_signalingAddress = host + ":" + std::to_string(port);
		}
	}

	void NetTraversalSession::SetStunServer(std::string, std::uint16_t)
	{
		// Browser sessions use the configured WebSocket gateway, not UDP traversal.
	}

	void NetTraversalSession::SetTurnServer(std::string, std::uint16_t, std::string, std::string, bool)
	{
		// Browser sessions use the configured WebSocket gateway, not TURN.
	}

	void NetTraversalSession::ResetForNewAttempt()
	{
		m_transport.Disconnect();
		m_state = TraversalState::Idle;
		m_failure.clear();
		m_joinInProgress = false;
	}

	bool NetTraversalSession::HostWithCode(std::string_view roomCode, std::uint16_t port, int maxConnections)
	{
		ResetForNewAttempt();
		const auto normalized = NormalizeRoomCode(roomCode);
		if (!normalized)
		{
			Fail("'" + std::string(roomCode) + "' is not a valid room code");
			return false;
		}
		if (m_useInjectedChannel)
		{
			Fail("custom signaling channels are not supported by the browser gateway");
			return false;
		}
		m_isHostRole = true;
		m_transport.SetWebGatewayRoom(*normalized);
		if (!m_transport.Host(port, maxConnections))
		{
			Fail(m_transport.LastError());
			return false;
		}
		m_state = TraversalState::Connecting;
		return true;
	}

	bool NetTraversalSession::JoinByCode(std::string_view roomCode)
	{
		ResetForNewAttempt();
		const auto normalized = NormalizeRoomCode(roomCode);
		if (!normalized)
		{
			Fail("'" + std::string(roomCode) + "' is not a valid room code");
			return false;
		}
		if (m_useInjectedChannel)
		{
			Fail("custom signaling channels are not supported by the browser gateway");
			return false;
		}
		m_isHostRole = false;
		if (!m_transport.Connect("room:" + *normalized, 0))
		{
			Fail(m_transport.LastError());
			return false;
		}
		m_joinInProgress = true;
		m_state = TraversalState::Connecting;
		return true;
	}

	void NetTraversalSession::Tick(float)
	{
		if (m_state != TraversalState::Connecting)
		{
			return;
		}
		for (const NetEvent& event: m_transport.Events())
		{
			if (event.kind == NetEvent::Kind::Connected)
			{
				m_state = TraversalState::Connected;
				m_joinInProgress = false;
				return;
			}
			if (event.kind == NetEvent::Kind::Disconnected && !m_isHostRole)
			{
				const std::string error = m_transport.LastError();
				Fail(error.empty() ? "the gateway connection closed before a peer connected" : error);
				return;
			}
		}
		if (!m_transport.IsActive())
		{
			const std::string error = m_transport.LastError();
			Fail(error.empty() ? "the gateway connection closed before a peer connected" : error);
		}
	}

	void NetTraversalSession::Fail(std::string reason)
	{
		m_failure = std::move(reason);
		m_state = TraversalState::Failed;
		m_transport.Disconnect();
		m_joinInProgress = false;
	}
} // namespace aether::net

#endif

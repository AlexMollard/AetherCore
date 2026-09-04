#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "net/Signaling.hpp"

namespace aether::net
{
	// A signalling channel for two machines on the same LAN with no server between
	// them at all: candidates go out as a UDP broadcast, and every other instance of
	// this class listening on the same room code hears them. This is the case where
	// even a rendezvous server is one dependency too many, because both players are
	// sitting on the same switch and could reach each other directly if only they
	// knew it.
	//
	// Opens a socket of its own with ENet's cross-platform socket API, unlike
	// NatTraversal, which deliberately shares the transport's - a LAN broadcast
	// never crosses a NAT, so there is no mapping here for a second socket to lose.
	//
	// One instance per room per machine: it broadcasts under one room code and one
	// per-instance nonce, both fixed at construction.
	class BroadcastSignalingChannel final : public ISignalingChannel
	{
	public:
		// A broadcast line is capped well above anything EncodeCandidates can
		// produce for kMaxCandidates endpoints (worst case eight
		// "255.255.255.255:65535" entries, plus the tag, room code and nonce), so a
		// peer that floods this socket with garbage cannot make Poll's per-datagram
		// work scale with however long it claims to be.
		static constexpr std::size_t kMaxLineLength = 512;

		// `roomCode` need not already be normalized - NormalizeRoomCode is applied
		// here, and a code that fails it is a construction failure like any other:
		// see IsUsable(). `port` is the broadcast port every instance in the room
		// must agree on.
		explicit BroadcastSignalingChannel(std::string roomCode, std::uint16_t port = 24700);
		~BroadcastSignalingChannel() override;

		BroadcastSignalingChannel(const BroadcastSignalingChannel&) = delete;
		BroadcastSignalingChannel& operator=(const BroadcastSignalingChannel&) = delete;

		// Sends immediately, and remembers the payload so Poll() can keep repeating
		// it on a timer until a later Publish replaces what is remembered - see
		// Poll() for why the repetition lives there and not here.
		void Publish(const CandidateSet& candidates) override;

		// Drains every datagram waiting on the socket without ever blocking the
		// frame loop, and returns the union of what survived the room/nonce filter
		// and kMaxCandidates cap. Also the one place this channel repeats the last
		// Publish on a timer; see the .cpp for why that lives here and not in the
		// caller.
		[[nodiscard]] std::optional<CandidateSet> Poll() override;

		// False when the room code did not normalize, or the socket could not be
		// created, bound, or configured for broadcast - a player behind a firewall
		// that blocks it still gets a usable object and a reason, not a throw or a
		// crash.
		[[nodiscard]] bool IsUsable() const
		{
			return m_usable;
		}

		[[nodiscard]] const std::string& FailureReason() const
		{
			return m_failure;
		}

		// The parse-and-filter step, pulled out as a pure function so the wire
		// format and both anti-loopback checks (wrong room, own nonce) are testable
		// without a socket. Refuses anything not fully understood, the same policy
		// DecodeCandidates follows: the line arrived from an unauthenticated
		// broadcast, and salvaging part of it would aim a punch nobody agreed to.
		[[nodiscard]] static std::optional<CandidateSet> ParseLine(std::string_view line, std::string_view roomCode, std::string_view ownNonceHex);

	private:
		void Fail(std::string reason);
		void SendNow(const CandidateSet& candidates);

		std::string m_roomCode;
		std::string m_nonceHex;
		std::uint16_t m_port = 0;

		// The underlying type is ENetSocket - `int` on POSIX, `SOCKET` (a
		// pointer-sized unsigned integer) on Windows. Storing the concrete alias
		// here would drag <enet/enet.h> into every translation unit that only wants
		// Publish/Poll, the same reason NatTraversal.hpp forward-declares its ENet
		// types instead of including the header. -1 means no socket is open, and
		// round-trips exactly through the cast in the .cpp on both platforms.
		std::intptr_t m_socket = -1;

		bool m_usable = false;
		std::string m_failure;

		// The last set this instance asked to publish, and when it was last sent -
		// re-sent on a timer from inside Poll() so a peer that starts listening
		// after the first Publish still hears it.
		std::optional<CandidateSet> m_lastPublished;
		std::uint64_t m_lastSendMs = 0;
	};
} // namespace aether::net

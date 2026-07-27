#pragma once

#include <cstdint>
#include <vector>

namespace aether::net
{
	// A remote peer. 0 is never a valid connection, so it doubles as "none".
	using ConnectionId = std::uint32_t;
	inline constexpr ConnectionId kInvalidConnection = 0;

	enum class NetRole : std::uint8_t
	{
		Offline,
		Host,
		Client
	};

	// Channel 0 is reliable-ordered: anything whose loss would desync state
	// (spawn, despawn, RPC, chat). Channel 1 is unreliable-sequenced: state
	// snapshots, where a dropped packet is superseded by the next one and
	// retransmitting it is worse than dropping it. Channel 2 is reserved for
	// voice so it can never share ordering with state - do not use it here.
	inline constexpr int kChannelReliable = 0;
	inline constexpr int kChannelSnapshot = 1;
	inline constexpr int kChannelVoiceReserved = 2;
	inline constexpr int kChannelCount = 3;

	struct NetEvent
	{
		enum class Kind : std::uint8_t
		{
			Connected,
			Disconnected,
			Data
		};

		Kind kind = Kind::Data;
		ConnectionId peer = kInvalidConnection;
		int channel = 0;
		std::vector<std::byte> data;
	};
} // namespace aether::net

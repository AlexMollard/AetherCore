#include "net/NetOwnership.hpp"

namespace aether::net
{
	std::vector<std::byte> EncodeOwnershipRequest(std::uint32_t netId, ConnectionId newOwner)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::OwnershipRequest));
		w.U32(netId);
		w.U32(newOwner);
		return w.Take();
	}

	std::optional<OwnershipTransferMessage> DecodeOwnershipRequest(ByteReader& r)
	{
		OwnershipTransferMessage msg;
		msg.netId = r.U32();
		msg.newOwner = r.U32();
		if (!r.Ok() || msg.netId == 0)
		{
			return std::nullopt;
		}
		return msg;
	}

	std::vector<std::byte> EncodeOwnershipTransfer(std::uint32_t netId, ConnectionId newOwner)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::OwnershipTransfer));
		w.U32(netId);
		w.U32(newOwner);
		return w.Take();
	}

	std::optional<OwnershipTransferMessage> DecodeOwnershipTransfer(ByteReader& r)
	{
		OwnershipTransferMessage msg;
		msg.netId = r.U32();
		msg.newOwner = r.U32();
		if (!r.Ok() || msg.netId == 0)
		{
			return std::nullopt;
		}
		return msg;
	}

	OwnershipTransferRefusal ValidateOwnershipRequest(ConnectionId currentOwner, ConnectionId sender,
	        ConnectionId newOwner)
	{
		if (newOwner != sender && newOwner != kInvalidConnection)
		{
			return OwnershipTransferRefusal::TargetNotSelfOrHost;
		}
		if (newOwner == sender)
		{
			// Claiming. Free (host-owned) or already the sender's own is fine; a
			// different, still-connected owner is not - see the enum's own comment.
			if (currentOwner != kInvalidConnection && currentOwner != sender)
			{
				return OwnershipTransferRefusal::NotFreeOrOwn;
			}
			return OwnershipTransferRefusal::None;
		}
		// Releasing to the host: only the current owner may let go of it.
		if (currentOwner != sender)
		{
			return OwnershipTransferRefusal::NotCurrentOwner;
		}
		return OwnershipTransferRefusal::None;
	}
} // namespace aether::net

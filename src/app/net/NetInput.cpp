#include "net/NetInput.hpp"

#include <algorithm>

#include "net/NetComponents.hpp"
#include "net/NetSession.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	std::vector<std::byte> EncodeInput(std::uint32_t netId, std::uint32_t scriptTypeHash, std::uint16_t methodIndex,
	        std::uint32_t sequence, std::span<const std::byte> payload)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Input));
		w.U32(netId);
		w.U32(scriptTypeHash);
		w.U16(methodIndex);
		w.U32(sequence);
		w.U32(static_cast<std::uint32_t>(payload.size()));
		w.Bytes(payload);
		return w.Take();
	}

	std::optional<InputMessage> DecodeInput(ByteReader& r)
	{
		InputMessage msg;
		msg.netId = r.U32();
		msg.scriptTypeHash = r.U32();
		msg.methodIndex = r.U16();
		msg.sequence = r.U32();
		const std::uint32_t payloadBytes = r.U32();
		if (!r.Ok())
		{
			return std::nullopt;
		}
		// Bytes() is bounds-checked, so a hostile length yields an empty vector and a
		// failed reader rather than an over-read.
		msg.payload = r.Bytes(payloadBytes);
		if (!r.Ok() || msg.netId == 0)
		{
			return std::nullopt;
		}
		return msg;
	}

	InputSend InputSendPacer::Prepare(std::uint32_t netId, std::span<const std::byte> payload, float now, float rateHz)
	{
		Stream& stream = m_streams[netId];

		const bool changed = !stream.everSent
		        || stream.lastSent.size() != payload.size()
		        || !std::equal(stream.lastSent.begin(), stream.lastSent.end(), payload.begin());
		if (!changed && now < stream.nextRepeatTime)
		{
			return InputSend{};
		}

		// Advance from `now` rather than by accumulating intervals, for the same
		// reason NetworkSendSystem does: a long frame must not leave this stream owing
		// a burst of back-to-back repeats.
		const float rate = rateHz > 0.f ? rateHz : 1.f;
		stream.nextRepeatTime = now + 1.f / rate;
		stream.lastSent.assign(payload.begin(), payload.end());
		stream.everSent = true;
		++stream.sequence;
		return InputSend{.send = true, .sequence = stream.sequence};
	}

	void InputSendPacer::Forget(std::uint32_t netId)
	{
		m_streams.erase(netId);
	}

	void InputSendPacer::Clear()
	{
		m_streams.clear();
	}

	InputRoute RouteInput(const World& world, const NetSession& session, Entity entity)
	{
		InputRoute route;

		if (session.Role() != NetRole::Client)
		{
			// Offline, or the host. This peer is the authority, so its own input needs
			// no round trip: applying it locally IS applying it authoritatively.
			route.allowed = true;
			route.invokeLocally = true;
			return route;
		}

		const auto* identity = world.TryGet<NetworkIdentity>(entity);
		if (identity != nullptr)
		{
			// A client with no id of its own yet would otherwise match every host-owned
			// entity, whose owner defaults to the same kInvalidConnection - the same
			// guard NetworkContext::IsOwner and ResolveTransforms both make.
			const ConnectionId local = session.LocalConnection();
			if (local == kInvalidConnection || identity->owner != local)
			{
				return route; // not ours to drive
			}
		}
		// An entity with no NetworkIdentity is purely local on this client, so it is
		// ours by the same rule IsOwner uses - predicted here and sent nowhere.

		route.allowed = true;
		route.invokeLocally = true;
		route.send = session.NetIdFor(entity) != 0;
		return route;
	}

	bool InputSequenceGate::Accept(std::uint32_t netId, std::uint32_t sequence)
	{
		const auto it = m_lastApplied.find(netId);
		if (it != m_lastApplied.end() && sequence <= it->second)
		{
			// Reordered or duplicated. Applying it would drive the entity with input
			// the sender has already superseded, and - because nothing resends the
			// newer value until it changes again - leave it wrong until it does.
			return false;
		}
		m_lastApplied[netId] = sequence;
		return true;
	}

	void InputSequenceGate::Forget(std::uint32_t netId)
	{
		m_lastApplied.erase(netId);
	}

	void InputSequenceGate::Clear()
	{
		m_lastApplied.clear();
	}

	void ApplyInput(World& world, NetSession& session, const RpcBridge& bridge, const InputMessage& msg,
	        ConnectionId sender, InputSequenceGate& gate)
	{
		const Entity entity = session.EntityFor(msg.netId);
		if (!entity.IsValid())
		{
			return;
		}
		// OWNERSHIP FIRST - see the note on ApplyInput's declaration. ApplyRpc makes
		// the identical check again below; that duplication is deliberate, because the
		// gate between the two must never see a packet from a peer with no claim to
		// this entity.
		const auto* identity = world.TryGet<NetworkIdentity>(entity);
		if (identity == nullptr || identity->owner != sender)
		{
			return;
		}
		if (!gate.Accept(msg.netId, msg.sequence))
		{
			return;
		}

		// Dispatch is the RPC path verbatim: same script-type-hash resolution, same
		// bounds-checked method index, same bridge. Target Server with localIsHost is
		// the only direction an input message can have - the kind itself is
		// client-to-host, and the receive system drops it outright on a client - so
		// ApplyRpc's direction gate is satisfied by construction rather than by
		// anything the sender wrote.
		const RpcMessage call{
		        .netId = msg.netId,
		        .scriptTypeHash = msg.scriptTypeHash,
		        .methodIndex = msg.methodIndex,
		        .target = NetRpcTarget::Server,
		        .args = msg.payload,
		};
		ApplyRpc(world, session, bridge, call, sender, /*localIsHost=*/true);
	}
} // namespace aether::net

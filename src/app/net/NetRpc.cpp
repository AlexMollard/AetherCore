#include "net/NetRpc.hpp"

#include "net/NetSession.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	namespace
	{
		// The first script on `scripts` whose type name hashes to `typeHash`. Mirrors
		// NetScriptFields.cpp's FindScriptIndex: two scripts of the same type on one
		// entity are indistinguishable on the wire, so the first wins.
		std::optional<std::uint32_t> FindScriptIndex(const ScriptComponent& scripts, std::uint32_t typeHash)
		{
			for (std::size_t i = 0; i < scripts.scripts.size(); ++i)
			{
				if (ScriptTypeHash(scripts.scripts[i].path) == typeHash)
				{
					return static_cast<std::uint32_t>(i);
				}
			}
			return std::nullopt;
		}
	} // namespace

	std::vector<std::byte> EncodeRpc(std::uint32_t netId, std::uint32_t scriptTypeHash, std::uint16_t methodIndex,
	        std::span<const std::byte> args)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Rpc));
		w.U32(netId);
		w.U32(scriptTypeHash);
		w.U16(methodIndex);
		w.U32(static_cast<std::uint32_t>(args.size()));
		w.Bytes(args);
		return w.Take();
	}

	std::optional<RpcMessage> DecodeRpc(ByteReader& r)
	{
		RpcMessage msg;
		msg.netId = r.U32();
		msg.scriptTypeHash = r.U32();
		msg.methodIndex = r.U16();
		const std::uint32_t argBytes = r.U32();
		if (!r.Ok())
		{
			return std::nullopt;
		}
		// Bytes() is bounds-checked, so a hostile length yields an empty vector and
		// a failed reader rather than an over-read.
		msg.args = r.Bytes(argBytes);
		if (!r.Ok() || msg.netId == 0)
		{
			return std::nullopt;
		}
		return msg;
	}

	void ApplyRpc(World& world, NetSession& session, const RpcBridge& bridge, const RpcMessage& msg)
	{
		const Entity entity = session.EntityFor(msg.netId);
		if (!entity.IsValid())
		{
			return;
		}
		const auto* scripts = world.TryGet<ScriptComponent>(entity);
		if (scripts == nullptr)
		{
			return;
		}
		const std::optional<std::uint32_t> scriptIndex = FindScriptIndex(*scripts, msg.scriptTypeHash);
		if (!scriptIndex.has_value())
		{
			return; // unknown type hash: no script on this entity matches
		}
		bridge.Invoke(entity, *scriptIndex, msg.methodIndex, msg.args);
	}
}

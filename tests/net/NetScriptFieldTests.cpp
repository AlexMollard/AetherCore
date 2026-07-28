#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetSession.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"

using namespace aether;

namespace
{
	// A ScriptFieldBridge with no CLR behind it: the replicated property tables are
	// declared per type, and values live in a plain map. This is what makes the
	// packet logic testable at all - a real bridge needs a loaded script assembly.
	class FakeBridge final : public net::ScriptFieldBridge
	{
	public:
		void Declare(const std::string& typeName, std::vector<net::ScriptPropertyDesc> props)
		{
			m_props[typeName] = std::move(props);
		}

		void Seed(std::uint32_t entityId, std::uint32_t scriptIndex, std::uint16_t propertyIndex, ScriptPropertyValue v)
		{
			m_values[Key(entityId, scriptIndex, propertyIndex)] = std::move(v);
		}

		[[nodiscard]] const ScriptPropertyValue* Peek(std::uint32_t entityId, std::uint32_t scriptIndex,
		        std::uint16_t propertyIndex) const
		{
			const auto it = m_values.find(Key(entityId, scriptIndex, propertyIndex));
			return it == m_values.end() ? nullptr : &it->second;
		}

		[[nodiscard]] std::vector<net::ScriptPropertyDesc> ReplicatedProperties(const std::string& typeName) const override
		{
			const auto it = m_props.find(typeName);
			return it == m_props.end() ? std::vector<net::ScriptPropertyDesc>{} : it->second;
		}

		[[nodiscard]] bool GetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex,
		        ScriptPropertyValue& out) const override
		{
			const auto it = m_values.find(Key(entity.id, scriptIndex, propertyIndex));
			if (it == m_values.end())
			{
				return false;
			}
			out = it->second;
			return true;
		}

		void SetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex,
		        const ScriptPropertyValue& value) const override
		{
			m_values[Key(entity.id, scriptIndex, propertyIndex)] = value;
		}

	private:
		static std::string Key(std::uint32_t entityId, std::uint32_t scriptIndex, std::uint16_t propertyIndex)
		{
			return std::to_string(entityId) + "/" + std::to_string(scriptIndex) + "/" + std::to_string(propertyIndex);
		}

		std::map<std::string, std::vector<net::ScriptPropertyDesc>> m_props;
		mutable std::map<std::string, ScriptPropertyValue> m_values;
	};

	ScriptPropertyValue IntValue(std::int64_t v)
	{
		ScriptPropertyValue out;
		out.type = ScriptPropertyValue::Type::Int;
		out.i64 = v;
		return out;
	}

	ScriptPropertyValue FloatValue(float v)
	{
		ScriptPropertyValue out;
		out.type = ScriptPropertyValue::Type::Float;
		out.f4[0] = v;
		return out;
	}

	ScriptPropertyValue BoolValue(bool v)
	{
		ScriptPropertyValue out;
		out.type = ScriptPropertyValue::Type::Bool;
		out.i64 = v ? 1 : 0;
		return out;
	}

	ScriptPropertyValue Vector3Value(float x, float y, float z)
	{
		ScriptPropertyValue out;
		out.type = ScriptPropertyValue::Type::Vector3;
		out.f4[0] = x;
		out.f4[1] = y;
		out.f4[2] = z;
		return out;
	}

	ScriptPropertyValue EnumValue(std::int64_t v)
	{
		ScriptPropertyValue out;
		out.type = ScriptPropertyValue::Type::Enum;
		out.i64 = v;
		return out;
	}

	// A host world and a client world, each with one networked entity carrying the
	// same "Health" script bound to net id 1 - what replication actually operates on.
	struct TwoScriptedWorlds
	{
		World host;
		World client;
		net::NetSession hostSession;
		net::NetSession clientSession;
		Entity hostEntity;
		Entity clientEntity;
		FakeBridge hostBridge;
		FakeBridge clientBridge;

		explicit TwoScriptedWorlds(std::string typeName = "Health")
		{
			hostEntity = host.Create();
			host.Emplace<net::NetworkIdentity>(hostEntity).netId = 1;
			host.Emplace<ScriptComponent>(hostEntity).scripts.push_back(ScriptEntry{.path = typeName});
			hostSession.Bind(1, hostEntity);

			clientEntity = client.Create();
			client.Emplace<net::NetworkIdentity>(clientEntity).netId = 1;
			client.Emplace<ScriptComponent>(clientEntity).scripts.push_back(ScriptEntry{.path = typeName});
			clientSession.Bind(1, clientEntity);
		}
	};
} // namespace

TEST_CASE("The script type hash matches the canonical FNV-1a 32 vectors")
{
	// Pinned against the published FNV-1a test vectors rather than against a second
	// copy of the same loop, so a wrong offset basis or prime on either side of the
	// C++/C# boundary shows up here instead of as garbage on the wire.
	CHECK(net::ScriptTypeHash("") == 0x811c9dc5u);
	CHECK(net::ScriptTypeHash("a") == 0xe40c292cu);
	CHECK(net::ScriptTypeHash("b") == 0xe70c2de5u);
	CHECK(net::ScriptTypeHash("foobar") == 0xbf9cf968u);
	// The names this actually hashes are script type names.
	CHECK(net::ScriptTypeHash("Health") == 0x986145afu);
	CHECK(net::ScriptTypeHash("PlayerController") == 0x9fd34ba8u);
}

TEST_CASE("A replicated script field reaches the client instance")
{
	TwoScriptedWorlds tw;
	tw.hostBridge.Declare("Health", {{.index = 2, .type = ScriptPropertyValue::Type::Int}});
	tw.clientBridge.Declare("Health", {{.index = 2, .type = ScriptPropertyValue::Type::Int}});
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 2, IntValue(77));

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity});
	REQUIRE_FALSE(packet.empty());

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, packet, net::StateWriteGate::TrustAll());

	const ScriptPropertyValue* applied = tw.clientBridge.Peek(tw.clientEntity.id, 0, 2);
	REQUIRE(applied != nullptr);
	CHECK(applied->type == ScriptPropertyValue::Type::Int);
	CHECK(applied->i64 == 77);
}

TEST_CASE("The script-field record is exactly the documented layout")
{
	// u16 count, then per field u32 netId + u32 typeHash + u16 propertyIndex +
	// u8 fieldType + the value, which uses the same codec component fields use. If
	// either half drifts, replicated script fields decode as garbage - so pin it.
	TwoScriptedWorlds tw;
	tw.hostBridge.Declare("Health", {{.index = 2, .type = ScriptPropertyValue::Type::Int}});
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 2, IntValue(7));

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity});

	constexpr std::size_t kHeader = 2;              // u16 count
	constexpr std::size_t kRecordPrefix = 4 + 4 + 2 + 1; // netId, typeHash, propertyIndex, fieldType
	CHECK(packet.size() == kHeader + kRecordPrefix + 4); // an Int value is four bytes

	// And the value bytes really are what WriteFieldValue produces for the same value -
	// not just the same length. Compare the packet's trailing 4 bytes byte-for-byte.
	net::ByteWriter expected;
	net::WriteFieldValue(expected, reflect::MakeValue(7));
	REQUIRE(expected.Size() == 4);
	const std::span<const std::byte> trailing{packet.data() + (packet.size() - expected.Size()), expected.Size()};
	CHECK(std::equal(trailing.begin(), trailing.end(), expected.View().begin()));
}

TEST_CASE("An unmarked script field is never sent")
{
	TwoScriptedWorlds tw;
	// The type has properties, but none of them are declared replicated.
	tw.hostBridge.Declare("Health", {});
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 0, IntValue(5));

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity});
	CHECK(packet.empty());
}

TEST_CASE("An unchanged script field produces no packet at all")
{
	TwoScriptedWorlds tw;
	tw.hostBridge.Declare("Health", {{.index = 0, .type = ScriptPropertyValue::Type::Float}});
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 0, FloatValue(1.5f));

	net::SnapshotCache cache;
	CHECK_FALSE(net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity}).empty());
	CHECK(net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity}).empty());
}

TEST_CASE("Script fields and component fields do not collide in the shared cache")
{
	// The script field parks on a sentinel component index; a component field with
	// the same net id and field index must still be treated as its own slot.
	net::SnapshotCache cache;
	const net::FieldKey componentKey{.netId = 1, .componentIndex = 0, .fieldIndex = 2, .scriptTypeHash = 0};
	const net::FieldKey scriptKey{
	        .netId = 1,
	        .componentIndex = net::kScriptFieldComponentIndex,
	        .fieldIndex = 2,
	        .scriptTypeHash = net::ScriptTypeHash("Health"),
	};

	reflect::FieldValue v;
	v.type = reflect::FieldType::Int;
	v.num = 3;

	CHECK(cache.Changed(componentKey, v));
	CHECK(cache.Changed(scriptKey, v)); // a distinct slot, so still "changed"
	CHECK_FALSE(cache.Changed(scriptKey, v));
}

TEST_CASE("A script field for an unknown net id does not desync the cursor for the next field")
{
	TwoScriptedWorlds tw;
	tw.clientBridge.Declare("Health", {{.index = 2, .type = ScriptPropertyValue::Type::Int}});

	net::ByteWriter w;
	w.U16(2);
	// Field 1: well-formed, but nobody is bound to this net id - skipped.
	w.U32(9999);
	w.U32(net::ScriptTypeHash("Health"));
	w.U16(2);
	w.U8(static_cast<std::uint8_t>(reflect::FieldType::Int));
	net::WriteFieldValue(w, reflect::MakeValue(-1));
	// Field 2: the real net id and a distinctive value that must still land. Against
	// an implementation that skips field 1 without consuming its value, this decodes
	// as garbage and the assertion below fails.
	w.U32(1);
	w.U32(net::ScriptTypeHash("Health"));
	w.U16(2);
	w.U8(static_cast<std::uint8_t>(reflect::FieldType::Int));
	net::WriteFieldValue(w, reflect::MakeValue(42));

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, w.Take(), net::StateWriteGate::TrustAll());

	const ScriptPropertyValue* applied = tw.clientBridge.Peek(tw.clientEntity.id, 0, 2);
	REQUIRE(applied != nullptr);
	CHECK(applied->i64 == 42);
}

TEST_CASE("A script field for a type the entity does not carry does not desync the cursor")
{
	TwoScriptedWorlds tw;
	tw.clientBridge.Declare("Health", {{.index = 2, .type = ScriptPropertyValue::Type::Int}});

	net::ByteWriter w;
	w.U16(2);
	// Field 1: the right entity, but it carries no script of this type - skipped.
	w.U32(1);
	w.U32(net::ScriptTypeHash("SomeOtherScript"));
	w.U16(2);
	w.U8(static_cast<std::uint8_t>(reflect::FieldType::Int));
	net::WriteFieldValue(w, reflect::MakeValue(-1));
	// Field 2 must still land.
	w.U32(1);
	w.U32(net::ScriptTypeHash("Health"));
	w.U16(2);
	w.U8(static_cast<std::uint8_t>(reflect::FieldType::Int));
	net::WriteFieldValue(w, reflect::MakeValue(64));

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, w.Take(), net::StateWriteGate::TrustAll());

	const ScriptPropertyValue* applied = tw.clientBridge.Peek(tw.clientEntity.id, 0, 2);
	REQUIRE(applied != nullptr);
	CHECK(applied->i64 == 64);
}

TEST_CASE("A script field the receiver does not consider replicated is rejected, not applied")
{
	TwoScriptedWorlds tw;
	// Property 2 is replicated here; property 5 is not declared at all.
	tw.clientBridge.Declare("Health", {{.index = 2, .type = ScriptPropertyValue::Type::Int}});

	net::ByteWriter w;
	w.U16(2);
	w.U32(1);
	w.U32(net::ScriptTypeHash("Health"));
	w.U16(5); // not a replicated property on this peer
	w.U8(static_cast<std::uint8_t>(reflect::FieldType::Int));
	net::WriteFieldValue(w, reflect::MakeValue(-1));
	w.U32(1);
	w.U32(net::ScriptTypeHash("Health"));
	w.U16(2);
	w.U8(static_cast<std::uint8_t>(reflect::FieldType::Int));
	net::WriteFieldValue(w, reflect::MakeValue(9));

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, w.Take(), net::StateWriteGate::TrustAll());

	CHECK(tw.clientBridge.Peek(tw.clientEntity.id, 0, 5) == nullptr);
	const ScriptPropertyValue* applied = tw.clientBridge.Peek(tw.clientEntity.id, 0, 2);
	REQUIRE(applied != nullptr);
	CHECK(applied->i64 == 9);
}

TEST_CASE("A stale index whose type no longer matches is dropped rather than written")
{
	TwoScriptedWorlds tw;
	// The script assembly reloaded on this peer: property 2 is now a string, but the
	// sender still believes it is an int. Writing it anyway would corrupt the field.
	tw.clientBridge.Declare("Health", {{.index = 2, .type = ScriptPropertyValue::Type::String}});

	net::ByteWriter w;
	w.U16(1);
	w.U32(1);
	w.U32(net::ScriptTypeHash("Health"));
	w.U16(2);
	w.U8(static_cast<std::uint8_t>(reflect::FieldType::Int));
	net::WriteFieldValue(w, reflect::MakeValue(123));

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, w.Take(), net::StateWriteGate::TrustAll());

	CHECK(tw.clientBridge.Peek(tw.clientEntity.id, 0, 2) == nullptr);
}

TEST_CASE("A script-field packet with an unusable type tag is abandoned, not guessed at")
{
	TwoScriptedWorlds tw;
	tw.clientBridge.Declare("Health", {{.index = 2, .type = ScriptPropertyValue::Type::Int}});

	net::ByteWriter w;
	w.U16(1);
	w.U32(1);
	w.U32(net::ScriptTypeHash("Health"));
	w.U16(2);
	w.U8(200); // not a FieldType at all, so the value length is unknowable
	w.U32(123);

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, w.Take(), net::StateWriteGate::TrustAll());

	CHECK(tw.clientBridge.Peek(tw.clientEntity.id, 0, 2) == nullptr);
}

TEST_CASE("A truncated script-field packet applies nothing")
{
	TwoScriptedWorlds tw;
	tw.clientBridge.Declare("Health", {{.index = 2, .type = ScriptPropertyValue::Type::Int}});

	net::ByteWriter w;
	w.U16(1);
	w.U32(1);
	w.U32(net::ScriptTypeHash("Health"));
	w.U16(2);
	w.U8(static_cast<std::uint8_t>(reflect::FieldType::Int));
	// value missing

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, w.Take(), net::StateWriteGate::TrustAll());

	CHECK(tw.clientBridge.Peek(tw.clientEntity.id, 0, 2) == nullptr);
}

TEST_CASE("A string script field round-trips")
{
	TwoScriptedWorlds tw;
	tw.hostBridge.Declare("Health", {{.index = 1, .type = ScriptPropertyValue::Type::String}});
	tw.clientBridge.Declare("Health", {{.index = 1, .type = ScriptPropertyValue::Type::String}});

	ScriptPropertyValue text;
	text.type = ScriptPropertyValue::Type::String;
	text.str = "wounded";
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 1, text);

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity});
	REQUIRE_FALSE(packet.empty());

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, packet, net::StateWriteGate::TrustAll());

	const ScriptPropertyValue* applied = tw.clientBridge.Peek(tw.clientEntity.id, 0, 1);
	REQUIRE(applied != nullptr);
	CHECK(applied->str == "wounded");
}

TEST_CASE("A float script field round-trips")
{
	TwoScriptedWorlds tw;
	tw.hostBridge.Declare("Health", {{.index = 3, .type = ScriptPropertyValue::Type::Float}});
	tw.clientBridge.Declare("Health", {{.index = 3, .type = ScriptPropertyValue::Type::Float}});
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 3, FloatValue(2.5f));

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity});
	REQUIRE_FALSE(packet.empty());

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, packet, net::StateWriteGate::TrustAll());

	const ScriptPropertyValue* applied = tw.clientBridge.Peek(tw.clientEntity.id, 0, 3);
	REQUIRE(applied != nullptr);
	CHECK(applied->type == ScriptPropertyValue::Type::Float);
	CHECK(applied->f4[0] == doctest::Approx(2.5f));
}

TEST_CASE("A bool script field round-trips")
{
	TwoScriptedWorlds tw;
	tw.hostBridge.Declare("Health", {{.index = 4, .type = ScriptPropertyValue::Type::Bool}});
	tw.clientBridge.Declare("Health", {{.index = 4, .type = ScriptPropertyValue::Type::Bool}});
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 4, BoolValue(true));

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity});
	REQUIRE_FALSE(packet.empty());

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, packet, net::StateWriteGate::TrustAll());

	const ScriptPropertyValue* applied = tw.clientBridge.Peek(tw.clientEntity.id, 0, 4);
	REQUIRE(applied != nullptr);
	CHECK(applied->type == ScriptPropertyValue::Type::Bool);
	CHECK(applied->i64 == 1);
}

TEST_CASE("A Vector3 script field round-trips")
{
	TwoScriptedWorlds tw;
	tw.hostBridge.Declare("Health", {{.index = 5, .type = ScriptPropertyValue::Type::Vector3}});
	tw.clientBridge.Declare("Health", {{.index = 5, .type = ScriptPropertyValue::Type::Vector3}});
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 5, Vector3Value(1.f, 2.f, 3.f));

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity});
	REQUIRE_FALSE(packet.empty());

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, packet, net::StateWriteGate::TrustAll());

	const ScriptPropertyValue* applied = tw.clientBridge.Peek(tw.clientEntity.id, 0, 5);
	REQUIRE(applied != nullptr);
	CHECK(applied->type == ScriptPropertyValue::Type::Vector3);
	CHECK(applied->f4[0] == doctest::Approx(1.f));
	CHECK(applied->f4[1] == doctest::Approx(2.f));
	CHECK(applied->f4[2] == doctest::Approx(3.f));
}

TEST_CASE("An enum script field round-trips within 32 bits")
{
	TwoScriptedWorlds tw;
	tw.hostBridge.Declare("Health", {{.index = 6, .type = ScriptPropertyValue::Type::Enum}});
	tw.clientBridge.Declare("Health", {{.index = 6, .type = ScriptPropertyValue::Type::Enum}});
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 6, EnumValue(3));

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity});
	REQUIRE_FALSE(packet.empty());

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, packet, net::StateWriteGate::TrustAll());

	const ScriptPropertyValue* applied = tw.clientBridge.Peek(tw.clientEntity.id, 0, 6);
	REQUIRE(applied != nullptr);
	CHECK(applied->type == ScriptPropertyValue::Type::Enum);
	CHECK(applied->i64 == 3);
}

TEST_CASE("An enum value that does not fit in 32 bits is truncated - documented, not a surprise")
{
	// enumValue on the wire (and in reflect::FieldValue, see Reflection.hpp) is a
	// plain 32-bit int, while ScriptPropertyValue::i64 - what a managed enum
	// property actually reports through - is 64 bits. ToFieldValue's Enum branch
	// narrows with a bare static_cast<int>, so a managed value outside the 32-bit
	// range loses everything above the low 32 bits the moment it hits the wire.
	// Pinned here as observed behaviour rather than left as a silent surprise:
	// 0x1_0000_0007 round-trips as 7, not as itself.
	TwoScriptedWorlds tw;
	tw.hostBridge.Declare("Health", {{.index = 6, .type = ScriptPropertyValue::Type::Enum}});
	tw.clientBridge.Declare("Health", {{.index = 6, .type = ScriptPropertyValue::Type::Enum}});
	constexpr std::int64_t kOverflowing = (std::int64_t{1} << 32) + 7;
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 6, EnumValue(kOverflowing));

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity});
	REQUIRE_FALSE(packet.empty());

	net::ApplyScriptFieldPacket(tw.client, tw.clientSession, tw.clientBridge, packet, net::StateWriteGate::TrustAll());

	const ScriptPropertyValue* applied = tw.clientBridge.Peek(tw.clientEntity.id, 0, 6);
	REQUIRE(applied != nullptr);
	CHECK(applied->i64 == 7); // NOT kOverflowing - the high 32 bits are gone
}

TEST_CASE("A duplicate script of the same type on one entity replicates only once")
{
	// Two entries of the same type share every wire key, so replicating both would
	// make them overwrite each other's cache slot every frame.
	TwoScriptedWorlds tw;
	tw.host.TryGet<ScriptComponent>(tw.hostEntity)->scripts.push_back(ScriptEntry{.path = "Health"});
	tw.hostBridge.Declare("Health", {{.index = 0, .type = ScriptPropertyValue::Type::Int}});
	tw.hostBridge.Seed(tw.hostEntity.id, 0, 0, IntValue(1));
	tw.hostBridge.Seed(tw.hostEntity.id, 1, 0, IntValue(2));

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity});
	REQUIRE_FALSE(packet.empty());

	net::ByteReader r{packet};
	CHECK(r.U16() == 1);

	// And the second build says nothing, rather than flip-flopping between the two.
	CHECK(net::BuildScriptFieldPacket(tw.host, tw.hostSession, cache, tw.hostBridge, {tw.hostEntity}).empty());
}

TEST_CASE("Only entities in the relevant set contribute script fields")
{
	// Two networked, scripted entities on the host; only one is passed as "relevant".
	// An empty relevant vector would only prove the loop runs zero times - this proves
	// the loop actually filters by entity rather than by, say, whether netId != 0.
	World host;
	net::NetSession hostSession;
	FakeBridge hostBridge;
	hostBridge.Declare("Health", {{.index = 0, .type = ScriptPropertyValue::Type::Int}});

	const Entity relevantEntity = host.Create();
	host.Emplace<net::NetworkIdentity>(relevantEntity).netId = 1;
	host.Emplace<ScriptComponent>(relevantEntity).scripts.push_back(ScriptEntry{.path = "Health"});
	hostSession.Bind(1, relevantEntity);
	hostBridge.Seed(relevantEntity.id, 0, 0, IntValue(3));

	const Entity irrelevantEntity = host.Create();
	host.Emplace<net::NetworkIdentity>(irrelevantEntity).netId = 2;
	host.Emplace<ScriptComponent>(irrelevantEntity).scripts.push_back(ScriptEntry{.path = "Health"});
	hostSession.Bind(2, irrelevantEntity);
	hostBridge.Seed(irrelevantEntity.id, 0, 0, IntValue(99));

	net::SnapshotCache cache;
	const std::vector<std::byte> packet =
	        net::BuildScriptFieldPacket(host, hostSession, cache, hostBridge, {relevantEntity});
	REQUIRE_FALSE(packet.empty());

	net::ByteReader r{packet};
	CHECK(r.U16() == 1); // exactly one field: the relevant entity's
	CHECK(r.U32() == 1); // its net id, not the irrelevant entity's (2)
}

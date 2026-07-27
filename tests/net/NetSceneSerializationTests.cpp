#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetSession.hpp"
#include "net/NetSpawn.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"

#include "../material/FakeSlotSink.hpp"
#include "../material/FakeTextureSink.hpp"

using namespace aether;
using namespace aether::app::scene;

// A networking component is useless if it does not survive the scene file. The whole
// "host and client derive the same net ids from the same scene with no handshake"
// mechanism (AssignScenePlacedNetIds) depends on the editor's Network Identity marks
// still being there after a reload - and a component declared with AE_COMPONENT but
// no AE_GENERIC_SERIALIZE is skipped by all three serializer paths (capture, TOML
// read, apply). It looks correct live and is silently gone on load, which is exactly
// how Network Identity shipped.
namespace
{
	// Every reflected component in the "Networking" category. Driven off the registry
	// rather than a hand-written list so the next networking component added cannot
	// regress the same way.
	std::vector<const reflect::ComponentType*> NetworkingComponents()
	{
		std::vector<const reflect::ComponentType*> types;
		for (const reflect::ComponentType& ct: reflect::ComponentTypes())
		{
			if (ct.category == "Networking")
			{
				types.push_back(&ct);
			}
		}
		return types;
	}

	int IndexOfNamed(const SceneDescription& scene, std::string_view name)
	{
		for (std::size_t i = 0; i < scene.entities.size(); ++i)
		{
			if (scene.entities[i].name == name)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}
} // namespace

TEST_CASE("Every Networking component survives capture -> TOML -> parse -> apply")
{
	const std::vector<const reflect::ComponentType*> networking = NetworkingComponents();
	// If this ever hits zero the loop below passes vacuously and proves nothing.
	REQUIRE(networking.size() >= 3);

	for (const reflect::ComponentType* ct: networking)
	{
		CAPTURE(ct->name);
		REQUIRE(ct->emplaceDefault != nullptr);
		REQUIRE(ct->has != nullptr);

		FakeSlotSink sink(8);
		FakeTextureSink tsink;
		TextureRegistry treg(tsink);
		MaterialRegistry mreg(sink, treg);

		World source;
		const Entity e = source.Create();
		source.Emplace<NameComponent>(e, NameComponent{.name = "Replicated"});
		source.Emplace<TransformComponent>(e, TransformComponent{});
		source.Emplace<SceneNodeComponent>(e, SceneNodeComponent{.id = 4242});
		ct->emplaceDefault(source, e);
		REQUIRE(ct->has(source, e));

		const std::string toml = WriteToml(CaptureScene(source, mreg, treg));
		const auto parsed = ParseToml(toml);
		REQUIRE(parsed.has_value());

		World fresh;
		const std::vector<Entity> created = ApplyScene(*parsed, fresh, ApplySceneDeps{});
		const int index = IndexOfNamed(*parsed, "Replicated");
		REQUIRE(index >= 0);
		REQUIRE(static_cast<std::size_t>(index) < created.size());
		const Entity applied = created[static_cast<std::size_t>(index)];
		REQUIRE(applied.IsValid());

		// The presence of the mark is the assertion. Network Identity reflects no
		// fields at all - netId/owner/scenePlaced are runtime state a session assigns -
		// so "the component is still here" is the entire contract.
		CHECK(ct->has(fresh, applied));
	}
}

TEST_CASE("A reloaded scene still derives net ids for its scene-placed entities")
{
	// The end-to-end consequence of the test above: after a save/load round trip
	// AssignScenePlacedNetIds must still find the entity. With Network Identity
	// missing from the file this found zero entities and the entire handshake-free
	// id derivation never fired outside tests that emplace the component by hand.
	FakeSlotSink sink(8);
	FakeTextureSink tsink;
	TextureRegistry treg(tsink);
	MaterialRegistry mreg(sink, treg);

	World source;
	const Entity e = source.Create();
	source.Emplace<NameComponent>(e, NameComponent{.name = "Player"});
	source.Emplace<TransformComponent>(e, TransformComponent{});
	source.Emplace<SceneNodeComponent>(e, SceneNodeComponent{.id = 1234});
	source.Emplace<net::NetworkIdentity>(e);

	const auto parsed = ParseToml(WriteToml(CaptureScene(source, mreg, treg)));
	REQUIRE(parsed.has_value());

	World fresh;
	const std::vector<Entity> created = ApplyScene(*parsed, fresh, ApplySceneDeps{});
	REQUIRE_FALSE(created.empty());

	net::NetSession session;
	net::AssignScenePlacedNetIds(fresh, session);

	int replicated = 0;
	for (const Entity entity: created)
	{
		if (const auto* identity = fresh.TryGet<net::NetworkIdentity>(entity))
		{
			CHECK(identity->netId != 0);
			CHECK(identity->scenePlaced);
			++replicated;
		}
	}
	CHECK(replicated == 1);
}

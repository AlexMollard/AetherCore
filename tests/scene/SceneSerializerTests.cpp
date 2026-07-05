#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "effects/EffectManager.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/PipelineCache.hpp"
#include "material/TextureRegistry.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "../material/FakePipelineFactory.hpp"
#include "../material/FakeSlotSink.hpp"
#include "../material/FakeTextureSink.hpp"

using namespace aether;
using namespace aether::app::scene;

namespace
{
    World MakeWorld()
    {
        World world;
        (void) world.Create(); // burn id 0 (null entity)
        return world;
    }

    // entt's entity storage iterates newest-first, so captured order is not
    // creation order (and is not required to be - parent indices are file-local).
    // Tests locate records and applied entities by name.
    int IndexOf(const SceneDescription& scene, std::string_view name)
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

    const EntityRecord& RecordOf(const SceneDescription& scene, std::string_view name)
    {
        static const EntityRecord kEmpty{};
        const int i = IndexOf(scene, name);
        return i >= 0 ? scene.entities[static_cast<std::size_t>(i)] : kEmpty;
    }

    Entity AppliedOf(const SceneDescription& scene, const std::vector<Entity>& created, std::string_view name)
    {
        const int i = IndexOf(scene, name);
        return i >= 0 ? created[static_cast<std::size_t>(i)] : Entity{};
    }
} // namespace

TEST_CASE("Capture -> WriteToml -> ParseToml round-trips every record type") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World world = MakeWorld();

    // Floor: named, tagged, transformed, static box physics, primitive mesh source.
    Entity floor = world.Create();
    world.Emplace<NameComponent>(floor, NameComponent{.name = "Floor"});
    const std::uint32_t groundTag = TagCreate("serializer_test_ground");
    TagAdd(&world, floor.id, groundTag);
    world.Emplace<TransformComponent>(floor, TransformComponent{.localToWorld = ComposeTransform({0, -1, 0}, {0, 0, 0}, {112, 2, 112})});
    world.Emplace<PhysicsDebugShapeComponent>(floor, PhysicsDebugShapeComponent{.shapeType = PhysicsShapeType::Box, .halfExtents = {56, 1, 56}});
    world.Emplace<RigidBodyComponent>(floor, RigidBodyComponent{.motionType = PhysicsMotionType::Static});
    world.Emplace<MeshSourceComponent>(floor, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Primitive, .path = "cube", .primitiveIndex = 0});

    // Child: parented under floor, dynamic sphere, textured material instance.
    Entity child = world.Create();
    world.Emplace<NameComponent>(child, NameComponent{.name = "Toy"});
    world.Emplace<TransformComponent>(child, TransformComponent{.localToWorld = ComposeTransform({1, 2, 3}, {10, 20, 30}, {2, 2, 2})});
    ecs::SetParent(world, child, floor);
    world.Emplace<PhysicsDebugShapeComponent>(child, PhysicsDebugShapeComponent{.shapeType = PhysicsShapeType::Sphere, .radius = 0.75f});
    world.Emplace<RigidBodyComponent>(child, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});
    MaterialAsset asset;
    asset.baseColorFactor = {0.9f, 0.2f, 0.1f, 1.0f};
    asset.metallicFactor = 0.7f;
    asset.roughnessFactor = 0.35f;
    asset.doubleSided = true;
    asset.albedoTex = treg.Acquire("brick.png");
    REQUIRE(asset.albedoTex.IsValid());
    world.Emplace<MaterialInstanceComponent>(child, MaterialInstanceComponent{asset});
    world.Emplace<MaterialComponent>(child, MaterialComponent{}); // capture keys off its presence

    // Orb: effect-driven with skinned state (animDb rebuild is model-side; the
    // record only carries playback state).
    Entity orb = world.Create();
    world.Emplace<NameComponent>(orb, NameComponent{.name = "Plasma Orb"});
    world.Emplace<TransformComponent>(orb, TransformComponent{});
    world.Emplace<EffectRefComponent>(orb, EffectRefComponent{.name = "plasma"});
    world.Emplace<EffectParamsComponent>(orb, EffectParamsComponent{7, EffectParams{.tint = {0.9f, 0.4f, 1.0f, 1.0f}, .speed = 2.0f, .scale = 3.0f, .intensity = 1.8f}});
    world.Emplace<SkinnedMeshComponent>(orb, SkinnedMeshComponent{.animDb = nullptr, .clipIndex = 2, .animTime = 4.5f, .playbackSpeed = 0.8f, .looping = false});

    const SceneDescription captured = CaptureScene(world, mreg, treg);
    REQUIRE(captured.entities.size() == 3);

    const std::string toml = WriteToml(captured);
    const auto parsed = ParseToml(toml);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 3);

    const EntityRecord& f = RecordOf(*parsed, "Floor");
    CHECK(f.name == "Floor");
    CHECK(std::find(f.tags.begin(), f.tags.end(), "serializer_test_ground") != f.tags.end());
    CHECK(f.hasTransform);
    CHECK(f.scale.x == doctest::Approx(112.0f));
    CHECK(f.parentIndex == -1);
    REQUIRE(f.mesh.has_value());
    CHECK(f.mesh->kind == MeshSourceComponent::Kind::Primitive);
    CHECK(f.mesh->path == "cube");
    REQUIRE(f.physics.has_value());
    CHECK(f.physics->shapeType == PhysicsShapeType::Box);
    CHECK(f.physics->motionType == PhysicsMotionType::Static);
    CHECK(f.physics->halfExtents.x == doctest::Approx(56.0f));

    const EntityRecord& c = RecordOf(*parsed, "Toy");
    CHECK(c.name == "Toy");
    CHECK(c.parentIndex == IndexOf(*parsed, "Floor"));
    REQUIRE(c.physics.has_value());
    CHECK(c.physics->shapeType == PhysicsShapeType::Sphere);
    CHECK(c.physics->radius == doctest::Approx(0.75f));
    CHECK(c.physics->motionType == PhysicsMotionType::Dynamic);
    CHECK(c.eulerDeg.y == doctest::Approx(20.0f).epsilon(1e-3));
    REQUIRE(c.material.has_value());
    CHECK(c.material->asset.baseColorFactor.r == doctest::Approx(0.9f));
    CHECK(c.material->asset.metallicFactor == doctest::Approx(0.7f));
    CHECK(c.material->asset.doubleSided);
    CHECK(!c.material->albedoPath.empty()); // resolved path round-tripped
    CHECK(c.material->normalPath.empty());

    const EntityRecord& o = RecordOf(*parsed, "Plasma Orb");
    CHECK(o.name == "Plasma Orb");
    REQUIRE(o.effect.has_value());
    CHECK(o.effect->name == "plasma");
    CHECK(o.effect->params.speed == doctest::Approx(2.0f));
    CHECK(o.effect->params.tint.z == doctest::Approx(1.0f));
    REQUIRE(o.skinned.has_value());
    CHECK(o.skinned->clipIndex == 2);
    CHECK(o.skinned->animTime == doctest::Approx(4.5f));
    CHECK(!o.skinned->looping);
}

TEST_CASE("Behavior components round-trip through capture, TOML and apply") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World source = MakeWorld();

    Entity orb = source.Create();
    source.Emplace<NameComponent>(orb, NameComponent{.name = "Orb"});
    source.Emplace<TransformComponent>(orb, TransformComponent{});
    source.Emplace<BobComponent>(orb, BobComponent{.amplitude = 1.5f, .frequency = 0.8f, .phase = 2.1f, .baseCaptured = true, .baseY = 20.0f, .time = 33.0f});

    Entity cube = source.Create();
    source.Emplace<NameComponent>(cube, NameComponent{.name = "Spinner"});
    source.Emplace<TransformComponent>(cube, TransformComponent{});
    source.Emplace<SpinComponent>(cube, SpinComponent{.eulerDegPerSec = {20.0f, 40.0f, 0.0f}});
    source.Emplace<MaterialPulseComponent>(cube, MaterialPulseComponent{.emissiveA = {0.1f, 0.0f, 0.0f}, .emissiveB = {2.0f, 1.0f, 0.2f}, .frequency = 2.0f, .time = 9.0f});

    Entity fox = source.Create();
    source.Emplace<NameComponent>(fox, NameComponent{.name = "Fox"});
    source.Emplace<TransformComponent>(fox, TransformComponent{});
    source.Emplace<OrbitComponent>(fox, OrbitComponent{.center = {14, 0, 15}, .radius = 6.0f, .angularSpeedDeg = 25.0f, .angleDeg = 123.0f, .yawOffsetDeg = 90.0f, .height = 0.0f});

    const auto parsed = ParseToml(WriteToml(CaptureScene(source, mreg, treg)));
    REQUIRE(parsed.has_value());

    const EntityRecord& o = RecordOf(*parsed, "Orb");
    REQUIRE(o.bob.has_value());
    CHECK(o.bob->amplitude == doctest::Approx(1.5f));
    CHECK(o.bob->frequency == doctest::Approx(0.8f));
    CHECK(o.bob->phase == doctest::Approx(2.1f));
    CHECK(!o.bob->baseCaptured); // transient state resets through the round-trip
    CHECK(o.bob->time == doctest::Approx(0.0f));

    const EntityRecord& s = RecordOf(*parsed, "Spinner");
    REQUIRE(s.spin.has_value());
    CHECK(s.spin->eulerDegPerSec.x == doctest::Approx(20.0f));
    REQUIRE(s.materialPulse.has_value());
    CHECK(s.materialPulse->emissiveB.r == doctest::Approx(2.0f));
    CHECK(s.materialPulse->frequency == doctest::Approx(2.0f));
    CHECK(s.materialPulse->time == doctest::Approx(0.0f));

    const EntityRecord& f = RecordOf(*parsed, "Fox");
    REQUIRE(f.orbit.has_value());
    CHECK(f.orbit->center.x == doctest::Approx(14.0f));
    CHECK(f.orbit->radius == doctest::Approx(6.0f));
    CHECK(f.orbit->angleDeg == doctest::Approx(123.0f)); // resumes in place
    CHECK(f.orbit->yawOffsetDeg == doctest::Approx(90.0f));

    World fresh = MakeWorld();
    const auto created = ApplyScene(*parsed, fresh, ApplySceneDeps{});
    const Entity appliedFox = AppliedOf(*parsed, created, "Fox");
    REQUIRE(appliedFox.IsValid());
    REQUIRE(fresh.TryGet<OrbitComponent>(appliedFox) != nullptr);
    CHECK(fresh.Get<OrbitComponent>(appliedFox).angleDeg == doctest::Approx(123.0f));
    const Entity appliedOrb = AppliedOf(*parsed, created, "Orb");
    REQUIRE(fresh.TryGet<BobComponent>(appliedOrb) != nullptr);
    CHECK(!fresh.Get<BobComponent>(appliedOrb).baseCaptured);
}

TEST_CASE("Lights and environment records round-trip through TOML") {
    SceneDescription scene;
    scene.name = "lit";

    EnvironmentRecord env;
    env.ambient = {0.20f, 0.22f, 0.27f};
    env.sunDirection = {-0.4f, -0.8f, -0.3f};
    env.sunIntensity = 4.0f;
    env.sunColor = {1.0f, 0.95f, 0.85f};
    env.skyHorizon = {0.55f, 0.65f, 0.80f};
    env.skyZenith = {0.15f, 0.25f, 0.50f};
    env.skyVoid = {0.02f, 0.02f, 0.03f};
    scene.environment = env;

    LightRecord point;
    point.isSpot = false;
    point.position = {-30, 6, -30};
    point.radius = 18.0f;
    point.color = {1.0f, 0.6f, 0.3f};
    point.intensity = 12.0f;
    point.castsShadow = true;
    scene.lights.push_back(point);

    LightRecord spot;
    spot.isSpot = true;
    spot.position = {10, 12, -35};
    spot.direction = {0.0f, -1.0f, 0.2f};
    spot.innerAngleRad = 0.30f;
    spot.outerAngleRad = 0.55f;
    spot.intensity = 20.0f;
    scene.lights.push_back(spot);

    const auto parsed = ParseToml(WriteToml(scene));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->environment.has_value());
    CHECK(parsed->environment->sunIntensity == doctest::Approx(4.0f));
    CHECK(parsed->environment->ambient.b == doctest::Approx(0.27f));
    CHECK(parsed->environment->skyZenith.b == doctest::Approx(0.50f));

    REQUIRE(parsed->lights.size() == 2);
    const LightRecord& p = parsed->lights[0];
    CHECK(!p.isSpot);
    CHECK(p.position.x == doctest::Approx(-30.0f));
    CHECK(p.radius == doctest::Approx(18.0f));
    CHECK(p.castsShadow);
    const LightRecord& s = parsed->lights[1];
    CHECK(s.isSpot);
    CHECK(s.innerAngleRad == doctest::Approx(0.30f));
    CHECK(s.outerAngleRad == doctest::Approx(0.55f));
    CHECK(s.direction.z == doctest::Approx(0.2f));
}

TEST_CASE("ApplyScene rebuilds names, tags, transforms, physics descs and hierarchy") {
    // No GPU-facing deps: mesh/material/effect resolution is skipped gracefully,
    // everything value-typed must round-trip into a fresh world.
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);

    World source = MakeWorld();
    Entity parent = source.Create();
    source.Emplace<NameComponent>(parent, NameComponent{.name = "Parent"});
    source.Emplace<TransformComponent>(parent, TransformComponent{.localToWorld = ComposeTransform({5, 0, -2}, {0, 45, 0}, {1, 1, 1})});
    Entity kid = source.Create();
    source.Emplace<NameComponent>(kid, NameComponent{.name = "Kid"});
    source.Emplace<TransformComponent>(kid, TransformComponent{});
    ecs::SetParent(source, kid, parent);
    source.Emplace<PhysicsDebugShapeComponent>(kid, PhysicsDebugShapeComponent{.shapeType = PhysicsShapeType::Capsule, .radius = 0.3f, .halfHeight = 0.9f});
    source.Emplace<RigidBodyComponent>(kid, RigidBodyComponent{.motionType = PhysicsMotionType::Kinematic});
    const std::uint32_t tag = TagCreate("serializer_test_apply");
    TagAdd(&source, kid.id, tag);

    const auto parsed = ParseToml(WriteToml(CaptureScene(source, mreg, treg)));
    REQUIRE(parsed.has_value());

    World fresh = MakeWorld();
    const std::vector<Entity> created = ApplyScene(*parsed, fresh, ApplySceneDeps{});
    REQUIRE(created.size() == 2);

    const Entity appliedParent = AppliedOf(*parsed, created, "Parent");
    const Entity appliedKid = AppliedOf(*parsed, created, "Kid");
    REQUIRE(appliedParent.IsValid());
    REQUIRE(appliedKid.IsValid());

    CHECK(fresh.Get<NameComponent>(appliedParent).name == "Parent");
    CHECK(fresh.Get<NameComponent>(appliedKid).name == "Kid");
    CHECK(TagHas(&fresh, appliedKid.id, tag));

    glm::vec3 pos{}, euler{}, scale{};
    DecomposeTRS(fresh.Get<TransformComponent>(appliedParent).localToWorld, pos, euler, scale);
    CHECK(pos.x == doctest::Approx(5.0f));
    CHECK(euler.y == doctest::Approx(45.0f).epsilon(1e-3));

    REQUIRE(fresh.TryGet<HierarchyComponent>(appliedKid) != nullptr);
    CHECK(fresh.Get<HierarchyComponent>(appliedKid).parent == appliedParent);

    const auto* capsule = fresh.TryGet<CapsuleBodyDesc>(appliedKid);
    REQUIRE(capsule != nullptr);
    CHECK(capsule->radius == doctest::Approx(0.3f));
    CHECK(capsule->halfHeight == doctest::Approx(0.9f));
    CHECK(capsule->motionType == PhysicsMotionType::Kinematic);
}

TEST_CASE("Effect records apply through the real effect path on load") {
    // Record-level scene: one effect-driven orb. The effect path needs no
    // mesh/material - it resolves the pipeline and params independently.
    SceneDescription scene;
    EntityRecord orb;
    orb.name = "Orb";
    orb.hasTransform = true;
    EffectRecord fx;
    fx.name = "molten";
    fx.params.tint = {1.0f, 0.25f, 0.05f, 1.0f};
    fx.params.speed = 3.5f;
    fx.params.scale = 1.25f;
    fx.params.intensity = 2.0f;
    orb.effect = fx;
    scene.entities.push_back(orb);

    const auto parsed = ParseToml(WriteToml(scene));
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == kSceneFormatVersion);

    // Real EffectManager + pipeline cache over the fake factory. The param
    // buffer stays uninitialized (no GPU): slot allocation fails SAFELY and
    // the components must still apply with the saved override params.
    aether::app::effects::EffectManager effects;
    aether::app::effects::EffectDef molten;
    molten.templateDesc.shaderVfsPath = "shaders://molten.spv";
    effects.Register("molten", molten);
    PipelineCache pipelines;
    FakePipelineFactory factory;
    pipelines.Initialize({}, std::ref(factory));
    EffectParamBuffer params;

    World world = MakeWorld();
    ApplySceneDeps deps{};
    deps.effectManager = &effects;
    deps.effectParams = &params;
    deps.pipelines = &pipelines;
    const auto created = ApplyScene(*parsed, world, deps);
    const Entity e = AppliedOf(*parsed, created, "Orb");
    REQUIRE(e.IsValid());

    const auto* ref = world.TryGet<EffectRefComponent>(e);
    REQUIRE(ref != nullptr);
    CHECK(ref->name == "molten");
    const auto* ep = world.TryGet<EffectParamsComponent>(e);
    REQUIRE(ep != nullptr);
    // Saved params override the effect's defaults on load.
    CHECK(ep->params.speed == doctest::Approx(3.5f));
    CHECK(ep->params.intensity == doctest::Approx(2.0f));
    CHECK(ep->params.tint.x == doctest::Approx(1.0f));
    const auto* pipe = world.TryGet<PipelineComponent>(e);
    REQUIRE(pipe != nullptr);
    CHECK(pipe->pipeline != nullptr);
    CHECK(factory.buildCount == 1);
}

TEST_CASE("Pre-versioning scene files parse as format v1") {
    const char* oldToml = "[scene]\nname = 'legacy'\n\n[[entities]]\nname = 'Box'\nposition = [1.0, 2.0, 3.0]\neuler = [0.0, 0.0, 0.0]\nscale = [1.0, 1.0, 1.0]\n";
    const auto parsed = ParseToml(oldToml);
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == 1);
    REQUIRE(parsed->entities.size() == 1);
    CHECK(parsed->entities[0].name == "Box");
    CHECK(!parsed->entities[0].orbit.has_value());
}

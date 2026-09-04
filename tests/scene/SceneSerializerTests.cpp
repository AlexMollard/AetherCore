#include <doctest/doctest.h>

#include <algorithm>

#include <chrono>
#include <filesystem>

#include <glm/glm.hpp>

#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "material/EffectManager.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/PipelineCache.hpp"
#include "material/TextureRegistry.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"
#include "editor/ReflectionJson.hpp"
#include "../material/FakePipelineFactory.hpp"
#include "../material/FakeSlotSink.hpp"
#include "../material/FakeTextureSink.hpp"

using namespace aether;
using namespace aether::app::scene;

namespace
{
    World MakeWorld()
    {
        return World{};
    }

    // creation order (and is not required to be - parent indices are file-local).
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

    // Reflection-driven pure-data components (Bob, Spin, Orbit, Material Pulse, Scale Pulse,
    // Look At, Parallax) live in EntityRecord::reflected keyed by display name + reflected field name.
    bool HasGeneric(const EntityRecord& rec, std::string_view type)
    {
        for (const GenericComponent& g: rec.reflected)
        {
            if (g.type == type)
            {
                return true;
            }
        }
        return false;
    }

    const reflect::FieldValue* GenericVal(const EntityRecord& rec, std::string_view type, std::string_view field)
    {
        for (const GenericComponent& g: rec.reflected)
        {
            if (g.type != type)
            {
                continue;
            }
            for (const auto& [name, val]: g.fields)
            {
                if (name == field)
                {
                    return &val;
                }
            }
        }
        return nullptr;
    }

    double GNum(const EntityRecord& rec, std::string_view type, std::string_view field)
    {
        const reflect::FieldValue* v = GenericVal(rec, type, field);
        return v != nullptr ? v->num : 0.0;
    }

    glm::vec4 GVec(const EntityRecord& rec, std::string_view type, std::string_view field)
    {
        const reflect::FieldValue* v = GenericVal(rec, type, field);
        return v != nullptr ? v->vec : glm::vec4(0.0f);
    }
}

TEST_CASE("Capture -> WriteToml -> ParseToml round-trips every record type") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World world = MakeWorld();

    Entity floor = world.Create();
    world.Emplace<NameComponent>(floor, NameComponent{.name = "Floor"});
    const std::uint32_t groundTag = TagCreate("serializer_test_ground");
    TagAdd(&world, floor.id, groundTag);
    world.Emplace<TransformComponent>(floor, TransformComponent{.localToWorld = ComposeTransform({0, -1, 0}, {0, 0, 0}, {112, 2, 112})});
    world.Emplace<ColliderComponent>(floor, ColliderComponent{.shape = PhysicsShapeType::Box, .halfExtents = {56, 1, 56}});
    world.Emplace<RigidBodyComponent>(floor, RigidBodyComponent{.motionType = PhysicsMotionType::Static});
    world.Emplace<MeshSourceComponent>(floor, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Primitive, .path = "cube", .primitiveIndex = 0});

    Entity child = world.Create();
    world.Emplace<NameComponent>(child, NameComponent{.name = "Toy"});
    world.Emplace<TransformComponent>(child, TransformComponent{.localToWorld = ComposeTransform({1, 2, 3}, {10, 20, 30}, {2, 2, 2})});
    ecs::SetParent(world, child, floor);
    world.Emplace<ColliderComponent>(child, ColliderComponent{.shape = PhysicsShapeType::Sphere, .radius = 0.75f});
    world.Emplace<RigidBodyComponent>(child, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});
    MaterialAsset asset;
    asset.baseColorFactor = {0.9f, 0.2f, 0.1f, 1.0f};
    asset.metallicFactor = 0.7f;
    asset.roughnessFactor = 0.35f;
    asset.doubleSided = true;
    asset.albedoTex = treg.Acquire("brick.png");
    REQUIRE(asset.albedoTex.IsValid());
    world.Emplace<MaterialInstanceComponent>(child, MaterialInstanceComponent{asset});
    world.Emplace<MaterialComponent>(child, MaterialComponent{});

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
    CHECK(!c.material->albedoPath.empty());
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
    REQUIRE(HasGeneric(o, "Bob"));
    CHECK(GNum(o, "Bob", "amplitude") == doctest::Approx(1.5f));
    CHECK(GNum(o, "Bob", "frequency") == doctest::Approx(0.8f));
    CHECK(GNum(o, "Bob", "phase") == doctest::Approx(2.1f));
    // Runtime fields (baseCaptured/time) are not reflected, so they never serialize; the
    // post-apply World check below confirms they stay default on load.

    const EntityRecord& s = RecordOf(*parsed, "Spinner");
    REQUIRE(HasGeneric(s, "Spin"));
    CHECK(GVec(s, "Spin", "euler_deg_per_sec").x == doctest::Approx(20.0f));
    REQUIRE(HasGeneric(s, "Material Pulse"));
    CHECK(GVec(s, "Material Pulse", "emissive_b").r == doctest::Approx(2.0f));
    CHECK(GNum(s, "Material Pulse", "frequency") == doctest::Approx(2.0f));

    const EntityRecord& f = RecordOf(*parsed, "Fox");
    REQUIRE(HasGeneric(f, "Orbit"));
    CHECK(GVec(f, "Orbit", "center").x == doctest::Approx(14.0f));
    CHECK(GNum(f, "Orbit", "radius") == doctest::Approx(6.0f));
    CHECK(GNum(f, "Orbit", "angle_deg") == doctest::Approx(123.0f));
    CHECK(GNum(f, "Orbit", "yaw_offset_deg") == doctest::Approx(90.0f));

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

TEST_CASE("Parallax component round-trips through capture, TOML and apply") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World source = MakeWorld();

    Entity layer = source.Create();
    source.Emplace<NameComponent>(layer, NameComponent{.name = "Clouds"});
    source.Emplace<TransformComponent>(layer, TransformComponent{});
    // Runtime fields (base/time/baseCaptured) must be stripped on capture.
    source.Emplace<ParallaxComponent>(layer, ParallaxComponent{.factor = {0.2f, 0.35f}, .scrollSpeed = {0.5f, 0.0f}, .baseCaptured = true, .base = {12.0f, 3.0f}, .time = 42.0f});

    const auto parsed = ParseToml(WriteToml(CaptureScene(source, mreg, treg)));
    REQUIRE(parsed.has_value());

    const EntityRecord& c = RecordOf(*parsed, "Clouds");
    REQUIRE(HasGeneric(c, "Parallax"));
    CHECK(GVec(c, "Parallax", "factor").x == doctest::Approx(0.2f));
    CHECK(GVec(c, "Parallax", "factor").y == doctest::Approx(0.35f));
    CHECK(GVec(c, "Parallax", "scroll_speed").x == doctest::Approx(0.5f));
    CHECK(GVec(c, "Parallax", "scroll_speed").y == doctest::Approx(0.0f));
    // Runtime fields (base/time/baseCaptured) are not reflected; the World check below
    // confirms they stay default on load.

    World fresh = MakeWorld();
    const auto created = ApplyScene(*parsed, fresh, ApplySceneDeps{});
    const Entity applied = AppliedOf(*parsed, created, "Clouds");
    REQUIRE(applied.IsValid());
    REQUIRE(fresh.TryGet<ParallaxComponent>(applied) != nullptr);
    CHECK(fresh.Get<ParallaxComponent>(applied).factor.y == doctest::Approx(0.35f));
    CHECK(!fresh.Get<ParallaxComponent>(applied).baseCaptured);
}

TEST_CASE("Particle emitter round-trips through capture, TOML and apply") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World source = MakeWorld();

    Entity fx = source.Create();
    source.Emplace<NameComponent>(fx, NameComponent{.name = "Sparkle"});
    source.Emplace<TransformComponent>(fx, TransformComponent{});
    ParticleEmitterComponent emitter;
    emitter.texturePath = "project://fx/spark.png";
    emitter.burstCount = 14;
    emitter.emitOnStart = true;
    emitter.autoDestroyWhenDone = true;
    emitter.lifetimeMax = 0.7f;
    emitter.startSize = 0.4f;
    emitter.endSize = 0.05f;
    emitter.gravity = {0.0f, -3.0f};
    emitter.blendMode = SpriteBlendMode::Additive;
    emitter.sortingLayer = 20;
    // Runtime state that must never serialize.
    emitter.particles.push_back(Particle{});
    emitter.started = true;
    emitter.pendingBurst = 3;
    source.Emplace<ParticleEmitterComponent>(fx, emitter);

    const auto parsed = ParseToml(WriteToml(CaptureScene(source, mreg, treg)));
    REQUIRE(parsed.has_value());

    // Particle Emitter serializes generically under its legacy "particles" key; the
    // authored values and runtime-stripping are verified on the applied component.
    const EntityRecord& r = RecordOf(*parsed, "Sparkle");
    CHECK(HasGeneric(r, "Particle Emitter"));

    World fresh = MakeWorld();
    const auto created = ApplyScene(*parsed, fresh, ApplySceneDeps{});
    const Entity applied = AppliedOf(*parsed, created, "Sparkle");
    REQUIRE(applied.IsValid());
    REQUIRE(fresh.TryGet<ParticleEmitterComponent>(applied) != nullptr);
    const ParticleEmitterComponent& out = fresh.Get<ParticleEmitterComponent>(applied);
    CHECK(out.texturePath == "project://fx/spark.png");
    CHECK(out.burstCount == 14);
    CHECK(out.emitOnStart);
    CHECK(out.autoDestroyWhenDone);
    CHECK(out.blendMode == SpriteBlendMode::Additive);
    CHECK(out.sortingLayer == 20);
    CHECK(out.gravity.y == doctest::Approx(-3.0f));
    CHECK(out.endSize == doctest::Approx(0.05f));
    // Runtime fields never round-trip: the applied emitter starts clean.
    CHECK(out.particles.empty());
    CHECK(!out.started);
    CHECK(out.pendingBurst == 0);
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
    source.Emplace<ColliderComponent>(kid, ColliderComponent{.shape = PhysicsShapeType::Capsule, .radius = 0.3f, .halfHeight = 0.9f});
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

    const auto* collider = fresh.TryGet<ColliderComponent>(appliedKid);
    REQUIRE(collider != nullptr);
    CHECK(collider->shape == PhysicsShapeType::Capsule);
    CHECK(collider->radius == doctest::Approx(0.3f));
    CHECK(collider->halfHeight == doctest::Approx(0.9f));
    const auto* body = fresh.TryGet<RigidBodyComponent>(appliedKid);
    REQUIRE(body != nullptr);
    CHECK(body->motionType == PhysicsMotionType::Kinematic);
}

TEST_CASE("Effect records apply through the real effect path on load") {
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

    // the components must still apply with the saved override params.
    aether::effects::EffectManager effects;
    aether::effects::EffectDef molten;
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
    CHECK(parsed->features == DefaultSceneFeatures(SceneKind::Scene3D));
    REQUIRE(parsed->entities.size() == 1);
    CHECK(parsed->entities[0].name == "Box");
    CHECK(!HasGeneric(parsed->entities[0], "Orbit"));
}

TEST_CASE("Scene feature flags round-trip and use kind-aware legacy defaults") {
    SceneDescription scene;
    scene.name = "Feature Flags";
    scene.kind = SceneKind::Mixed;
    scene.features = SceneFeatureFlags::Sprites | SceneFeatureFlags::Physics2D | SceneFeatureFlags::Meshes3D;

    const auto parsed = ParseToml(WriteToml(scene));
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == kSceneFormatVersion);
    CHECK(parsed->kind == SceneKind::Mixed);
    CHECK(parsed->features == scene.features);

    const auto legacy2D = ParseToml("[scene]\nname = 'legacy 2d'\nkind = '2d'\nversion = 9\nfeatures = ['sprites', 'physics_2d']\n");
    REQUIRE(legacy2D.has_value());
    CHECK(legacy2D->features == (SceneFeatureFlags::Sprites | SceneFeatureFlags::Physics2D | SceneFeatureFlags::Tilemaps));

    const auto missingFeatures = ParseToml("[scene]\nname = 'legacy 2d defaults'\nkind = '2d'\nversion = 9\n");
    REQUIRE(missingFeatures.has_value());
    CHECK(missingFeatures->features == DefaultSceneFeatures(SceneKind::Scene2D));

    // Pre-v14 files never knew the physics flags (physics ran unconditionally),
    // so the v14 migration grants the kind's physics flag even to an explicitly
    // empty feature list - preserving old behaviour.
    const auto explicitlyEmpty = ParseToml("[scene]\nname = 'no features'\nkind = 'mixed'\nversion = 10\nfeatures = []\n");
    REQUIRE(explicitlyEmpty.has_value());
    CHECK(explicitlyEmpty->features == SceneFeatureFlags::Physics3D);

    const auto modernEmpty = ParseToml("[scene]\nname = 'no features v14'\nkind = 'mixed'\nversion = 14\nfeatures = []\n");
    REQUIRE(modernEmpty.has_value());
    CHECK(modernEmpty->features == SceneFeatureFlags::None);
}

TEST_CASE("CameraComponent gradient stops round-trip through TOML") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World world = MakeWorld();

    const Entity cam = world.Create();
    world.Emplace<NameComponent>(cam, NameComponent{.name = "Cam"});
    world.Emplace<TransformComponent>(cam, TransformComponent{});
    CameraComponent camera{};
    camera.background = CameraBackground::Gradient;
    camera.gradientAngleDegrees = 30.0f;
    camera.gradientStops = {
            {{1.0f, 0.0f, 0.0f}, 0.0f},
            {{0.0f, 1.0f, 0.0f}, 0.5f},
            {{0.0f, 0.0f, 1.0f}, 1.0f},
    };
    world.Emplace<CameraComponent>(cam, camera);

    const auto parsed = ParseToml(WriteToml(CaptureScene(world, mreg, treg)));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    REQUIRE(parsed->entities[0].camera.has_value());
    const auto& out = *parsed->entities[0].camera;
    CHECK(out.background == CameraBackground::Gradient);
    CHECK(out.gradientAngleDegrees == doctest::Approx(30.0f));
    REQUIRE(out.gradientStops.size() == 3);
    CHECK(out.gradientStops[1].position == doctest::Approx(0.5f));
    CHECK(out.gradientStops[2].colour.b == doctest::Approx(1.0f));
}

TEST_CASE("Legacy use_sky_gradient=false migrates to SolidColour") {
    const auto parsed = ParseToml(
            "[scene]\nkind = '2d'\nname = 'legacy'\nversion = 15\n"
            "[[entities]]\nname = 'Cam'\n"
            "[entities.camera]\nprojection = 'orthographic'\nuse_sky_gradient = false\n"
            "clear_color = [0.2, 0.3, 0.4]\nmain = true\n");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    REQUIRE(parsed->entities[0].camera.has_value());
    const auto& cam = *parsed->entities[0].camera;
    CHECK(cam.background == CameraBackground::SolidColour);
    CHECK(cam.clearColor.g == doctest::Approx(0.3f));
}

TEST_CASE("Legacy use_sky_gradient=true migrates to SkyGradient") {
    const auto parsed = ParseToml(
            "[scene]\nkind = 'mixed'\nname = 'legacy'\nversion = 15\n"
            "[[entities]]\nname = 'Cam'\n"
            "[entities.camera]\nuse_sky_gradient = true\nmain = true\n");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities[0].camera.has_value());
    CHECK(parsed->entities[0].camera->background == CameraBackground::SkyGradient);
}

TEST_CASE("Gradient stops are clamped and sorted on load") {
    const auto parsed = ParseToml(
            "[scene]\nkind = '2d'\nname = 'grad'\nversion = 15\n"
            "[[entities]]\nname = 'Cam'\n"
            "[entities.camera]\nbackground = 'gradient'\n"
            "[[entities.camera.gradient_stops]]\ncolour = [0,0,1]\nposition = 1.5\n"
            "[[entities.camera.gradient_stops]]\ncolour = [1,0,0]\nposition = -0.5\n");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities[0].camera.has_value());
    const auto& stops = parsed->entities[0].camera->gradientStops;
    REQUIRE(stops.size() == 2);
    CHECK(stops.front().position == doctest::Approx(0.0f)); // clamped from -0.5, sorted first
    CHECK(stops.front().colour.r == doctest::Approx(1.0f)); // the red stop
    CHECK(stops.back().position == doctest::Approx(1.0f));  // clamped from 1.5
}

TEST_CASE("CameraComponent defaults to SkyGradient with two gradient stops") {
    CameraComponent cam{};
    CHECK(cam.background == CameraBackground::SkyGradient);
    REQUIRE(cam.gradientStops.size() == 2);
    CHECK(cam.gradientStops.front().position == doctest::Approx(0.0f));
    CHECK(cam.gradientStops.back().position == doctest::Approx(1.0f));
    CHECK(cam.gradientAngleDegrees == doctest::Approx(0.0f));
}

TEST_CASE("Reflected List field round-trips through MCP JSON (gradient stops)") {
    // gradient_stops was previously hand-parsed and unreachable via get_component /
    // set_component; as a reflected List field it now round-trips through the same
    // FieldValueToJson / JsonToFieldValue the MCP handlers use.
    const reflect::ComponentType* rt = reflect::FindComponentType("Camera");
    REQUIRE(rt != nullptr);
    const reflect::FieldDesc* field = rt->FindField("gradient_stops");
    REQUIRE(field != nullptr);
    CHECK(field->type == reflect::FieldType::List);

    CameraComponent cam{};
    cam.gradientStops = {
            {{1.0f, 0.0f, 0.0f}, 0.0f},
            {{0.0f, 1.0f, 0.0f}, 0.5f},
            {{0.0f, 0.0f, 1.0f}, 1.0f},
    };

    const nlohmann::json j = editor::FieldValueToJson(field->get(&cam), field);
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 3);
    CHECK(j[1]["position"].get<double>() == doctest::Approx(0.5));
    CHECK(j[2]["colour"][2].get<double>() == doctest::Approx(1.0));

    CameraComponent restored{};
    field->set(&restored, editor::JsonToFieldValue(j, *field));
    REQUIRE(restored.gradientStops.size() == 3);
    CHECK(restored.gradientStops[0].colour.r == doctest::Approx(1.0f));
    CHECK(restored.gradientStops[1].position == doctest::Approx(0.5f));
    CHECK(restored.gradientStops[2].colour.b == doctest::Approx(1.0f));
}

#ifdef AETHER_SCENES_SOURCE_DIR
TEST_CASE("SceneTextHasNoCameraSource flags camera-less scenes but not prefab-backed ones") {
    // A scene with a main camera has a camera source.
    const auto with = io::file_util::ReadText(std::filesystem::path(AETHER_SCENES_SOURCE_DIR) / "default2d.scene.toml");
    REQUIRE(with.has_value());
    CHECK_FALSE(SceneTextHasNoCameraSource(*with));

    // A scene with entities but no main camera and no prefab instances lacks one.
    CHECK(SceneTextHasNoCameraSource("version = 16\n[[entities]]\nname = 'Ground'\nparent = -1\n"));

    // A prefab instance may carry the camera, so never warn in that case.
    CHECK_FALSE(SceneTextHasNoCameraSource("version = 16\n[[prefab_instances]]\nprefab = 'CameraRig'\n"));
}

TEST_CASE("Blank 2D template scene has an orthographic main camera") {
    const auto text = io::file_util::ReadText(std::filesystem::path(AETHER_SCENES_SOURCE_DIR) / "default2d.scene.toml");
    REQUIRE(text.has_value());
    const auto scene = ParseToml(*text);
    REQUIRE(scene.has_value());
    CHECK(scene->kind == SceneKind::Scene2D);
    CHECK(scene->features == (SceneFeatureFlags::Sprites | SceneFeatureFlags::Physics2D | SceneFeatureFlags::Tilemaps));
    REQUIRE(scene->entities.size() == 2);

    // Found by what it IS, not by where it sits. The serializer orders parents before their
    // children, so parenting the camera to the Player moved it to the second slot - an index
    // this test used to hard-code, which made a scene reorder look like a missing camera.
    const auto cameraIt = std::ranges::find_if(scene->entities, [](const EntityRecord& e) { return e.camera.has_value(); });
    REQUIRE(cameraIt != scene->entities.end());
    const EntityRecord& cameraEntity = *cameraIt;

    CHECK(cameraEntity.mainCamera);
    CHECK(cameraEntity.camera->projection == CameraProjection::Orthographic);
    CHECK(cameraEntity.camera->orthographicHeight == doctest::Approx(10.0f));
    // 2D scenes clear to a flat camera-owned colour instead of the 3D sky.
    CHECK(cameraEntity.camera->background == CameraBackground::SolidColour);
    CHECK(cameraEntity.camera->clearColor.r == doctest::Approx(0.10f));

    // The camera rides the Player, so a new project's view follows what you are moving
    // instead of watching it walk off the edge of the screen.
    const auto playerIndex = static_cast<int>(std::distance(scene->entities.begin(),
            std::ranges::find_if(scene->entities, [](const EntityRecord& e) { return !e.scripts.empty(); })));
    CHECK(cameraEntity.parentIndex == playerIndex);

    World world = MakeWorld();
    const auto created = ApplyScene(*scene, world, ApplySceneDeps{});
    REQUIRE(created.size() == 2);
    CHECK(world.GetSceneKind() == SceneKind::Scene2D);
    CHECK(world.GetSceneFeatures() == (SceneFeatureFlags::Sprites | SceneFeatureFlags::Physics2D | SceneFeatureFlags::Tilemaps));
    const auto cameraEntityIndex = static_cast<std::size_t>(std::distance(scene->entities.begin(), cameraIt));
    CHECK(world.Get<CameraComponent>(created[cameraEntityIndex]).projection == CameraProjection::Orthographic);
}

// The template's whole job: a project made from it must DO something the first time Play is
// pressed. It used to be a lone camera with the starter script attached to nothing, so a new
// project opened on an empty grid and Play did visibly nothing.
TEST_CASE("Blank 2D template ships a player that runs without being wired up") {
    const auto text = io::file_util::ReadText(std::filesystem::path(AETHER_SCENES_SOURCE_DIR) / "default2d.scene.toml");
    REQUIRE(text.has_value());
    const auto scene = ParseToml(*text);
    REQUIRE(scene.has_value());

    const auto player = std::ranges::find_if(scene->entities, [](const auto& e) { return e.name == "Player"; });
    REQUIRE(player != scene->entities.end());

    // Attached, not merely present in the project: an unattached script is what made the old
    // template look broken.
    REQUIRE(player->scripts.size() == 1);
    CHECK(player->scripts[0].type == "Player");

    // Visible without any art. A sprite is sized as pixelSize / pixelsPerUnit, so leaving
    // pixelSize at zero - the usual "let the texture decide" - yields a sprite that exists,
    // moves, and is zero pixels across. That is worse than no sprite, because nothing about
    // it looks wrong in the outliner.
    REQUIRE(player->sprite.has_value());
    CHECK(player->sprite->texturePath.empty()); // empty path samples white so the tint shows
    CHECK(player->sprite->pixelSize.x > 0.0f);
    CHECK(player->sprite->pixelSize.y > 0.0f);
    CHECK(player->sprite->pixelsPerUnit > 0.0f);
    CHECK(player->sprite->tint.a > 0.0f);

    // Roughly one world unit against a camera ten units tall - big enough to spot immediately.
    CHECK(player->sprite->pixelSize.x / player->sprite->pixelsPerUnit == doctest::Approx(1.0f));
}
#endif

TEST_CASE("ReplaceScene spares transient subtrees during gameplay switches only") {
    World world = MakeWorld();

    // A persistent script actor: aether_mark_transient sets both markers, so this
    // mirrors how the engine actually tags a DontDestroyOnLoad entity. The gameplay
    // spare keys off DontDestroyOnLoad, not SceneTransient (prefab-instance roots are
    // SceneTransient but must be re-expanded, not kept - see the Scene.Load leak fix).
    const Entity player = world.Create();
    world.Emplace<NameComponent>(player, NameComponent{.name = "Player"});
    world.Emplace<TransformComponent>(player, TransformComponent{});
    world.GetRegistry().emplace<SceneTransientComponent>(World::ToEntt(player));
    world.GetRegistry().emplace<DontDestroyOnLoadComponent>(World::ToEntt(player));
    const Entity playerMesh = world.Create();
    world.Emplace<TransformComponent>(playerMesh, TransformComponent{});
    REQUIRE(ecs::SetParent(world, playerMesh, player));

    const Entity prop = world.Create();
    world.Emplace<NameComponent>(prop, NameComponent{.name = "Prop"});
    world.Emplace<TransformComponent>(prop, TransformComponent{});

    // Gameplay switch (Scene.Load): the prop goes, the player subtree stays.
    ReplaceScene(SceneDescription{}, world, ApplySceneDeps{}, SceneLoadMode::GameplaySwitch);

    auto& reg = world.GetRegistry();
    CHECK(reg.valid(World::ToEntt(player)));
    CHECK(reg.valid(World::ToEntt(playerMesh)));
    CHECK(!reg.valid(World::ToEntt(prop)));

    // Authoring load (editor Open / play stop): persistence does not apply -
    // the whole world resets, transients included.
    ReplaceScene(SceneDescription{}, world, ApplySceneDeps{});
    CHECK(!reg.valid(World::ToEntt(player)));
    CHECK(!reg.valid(World::ToEntt(playerMesh)));
}

TEST_CASE("Light entities round-trip through capture, TOML and apply (v3)") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World world = MakeWorld();

    const glm::vec3 spotDir = glm::normalize(glm::vec3(0.3f, -0.8f, 0.5f));
    (void) ecs::CreatePointLightEntity(world, {-40.0f, 13.0f, 40.0f}, PointLightComponent{.color = {1.0f, 0.85f, 0.6f, }, .intensity = 45.0f, .radius = 34.0f, .castsShadow = true}, "Plaza Light");
    (void) ecs::CreateSpotLightEntity(world, {0.0f, 13.0f, -44.0f}, spotDir, SpotLightComponent{.color = {0.85f, 0.92f, 1.0f}, .intensity = 60.0f, .radius = 50.0f, .innerAngleRad = 0.35f, .outerAngleRad = 0.55f, .castsShadow = true}, "Stage Spot");

    const auto parsed = ParseToml(WriteToml(CaptureScene(world, mreg, treg)));
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == kSceneFormatVersion);
    CHECK(parsed->lights.empty());

    // Lights serialize generically (genericSerialize), so the record carries them in
    // `reflected`; the authored values are verified on the applied components below.
    const EntityRecord& p = RecordOf(*parsed, "Plaza Light");
    CHECK(HasGeneric(p, "Point Light"));
    CHECK(p.position.x == doctest::Approx(-40.0f));

    const EntityRecord& s = RecordOf(*parsed, "Stage Spot");
    CHECK(HasGeneric(s, "Spot Light"));

    World fresh = MakeWorld();
    const auto created = ApplyScene(*parsed, fresh, ApplySceneDeps{});
    const Entity spot = AppliedOf(*parsed, created, "Stage Spot");
    REQUIRE(spot.IsValid());
    REQUIRE(fresh.TryGet<SpotLightComponent>(spot) != nullptr);
    const SpotLightComponent& appliedSpot = fresh.Get<SpotLightComponent>(spot);
    CHECK(appliedSpot.innerAngleRad == doctest::Approx(0.35f));
    CHECK(appliedSpot.outerAngleRad == doctest::Approx(0.55f));
    const glm::mat4& m = fresh.Get<TransformComponent>(spot).localToWorld;
    const glm::vec3 fwd = -glm::normalize(glm::vec3(m[2]));
    CHECK(glm::dot(fwd, spotDir) == doctest::Approx(1.0f).epsilon(1e-3));
    const Entity point = AppliedOf(*parsed, created, "Plaza Light");
    REQUIRE(fresh.TryGet<PointLightComponent>(point) != nullptr);
    const PointLightComponent& appliedPoint = fresh.Get<PointLightComponent>(point);
    CHECK(appliedPoint.intensity == doctest::Approx(45.0f));
    CHECK(appliedPoint.radius == doctest::Approx(34.0f));
    CHECK(appliedPoint.castsShadow);
}

TEST_CASE("Legacy [[lights]] records migrate to light entities on apply") {
    const char* v2Toml =
        "[scene]\nname = 'legacy'\nversion = 2\n\n"
        "[[lights]]\ntype = 'point'\nposition = [34.0, 11.0, 40.0]\ncolor = [1.0, 0.25, 0.25]\nintensity = 36.0\nradius = 30.0\nshadow = true\n\n"
        "[[lights]]\ntype = 'spot'\nposition = [0.0, 13.0, -44.0]\ncolor = [0.85, 0.92, 1.0]\nintensity = 60.0\nradius = 50.0\ndirection = [0.0, -0.15, -1.0]\ninner_rad = 0.35\nouter_rad = 0.55\nshadow = true\n";
    const auto parsed = ParseToml(v2Toml);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->lights.size() == 2);

    World world = MakeWorld();
    (void) ApplyScene(*parsed, world, ApplySceneDeps{});

    auto& reg = world.GetRegistry();
    int points = 0;
    for (const auto e: reg.view<PointLightComponent>())
    {
        const auto& l = reg.get<PointLightComponent>(e);
        CHECK(l.intensity == doctest::Approx(36.0f));
        CHECK(l.castsShadow);
        const auto& tc = reg.get<TransformComponent>(e);
        CHECK(tc.localToWorld[3].x == doctest::Approx(34.0f));
        ++points;
    }
    CHECK(points == 1);
    int spots = 0;
    for (const auto e: reg.view<SpotLightComponent>())
    {
        const auto& tc = reg.get<TransformComponent>(e);
        const glm::vec3 fwd = -glm::normalize(glm::vec3(tc.localToWorld[2]));
        CHECK(glm::dot(fwd, glm::normalize(glm::vec3(0.0f, -0.15f, -1.0f))) == doctest::Approx(1.0f).epsilon(1e-3));
        ++spots;
    }
    CHECK(spots == 1);
}

TEST_CASE("Prefabs capture one subtree and instantiate re-rooted") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World world = MakeWorld();

    // Bystander that must NOT be captured.
    Entity outside = world.Create();
    world.Emplace<NameComponent>(outside, NameComponent{.name = "Outside"});
    world.Emplace<TransformComponent>(outside, TransformComponent{});

    Entity root = world.Create();
    world.Emplace<NameComponent>(root, NameComponent{.name = "Rig"});
    world.Emplace<TransformComponent>(root, TransformComponent{.localToWorld = ComposeTransform({2, 0, 0}, {0, 0, 0}, {1, 1, 1})});
    // A transient marker must NOT exclude prefab capture (the player is the
    world.GetRegistry().emplace<SceneTransientComponent>(World::ToEntt(root));

    Entity arm = world.Create();
    world.Emplace<NameComponent>(arm, NameComponent{.name = "Arm"});
    world.Emplace<TransformComponent>(arm, TransformComponent{.localToWorld = ComposeTransform({3, 1, 0}, {0, 0, 0}, {1, 1, 1})});
    world.Emplace<SpinComponent>(arm, SpinComponent{.eulerDegPerSec = {0, 90, 0}});
    REQUIRE(ecs::SetParent(world, arm, root));

    const auto parsed = ParseToml(WriteToml(CapturePrefab(world, root, mreg, treg)));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 2);
    CHECK(parsed->entities[0].name == "Rig");
    CHECK(parsed->entities[0].parentIndex == -1);
    CHECK(IndexOf(*parsed, "Outside") == -1);
    const EntityRecord& armRec = RecordOf(*parsed, "Arm");
    CHECK(armRec.parentIndex == 0);
    REQUIRE(HasGeneric(armRec, "Spin"));

    World fresh = MakeWorld();
    const Entity newRoot = InstantiatePrefab(*parsed, fresh, ApplySceneDeps{}, ComposeTransform({12, 0, 0}, {0, 0, 0}, {1, 1, 1}));
    REQUIRE(newRoot.IsValid());
    CHECK(fresh.Get<TransformComponent>(newRoot).localToWorld[3].x == doctest::Approx(12.0f));
    const auto* h = fresh.TryGet<HierarchyComponent>(newRoot);
    REQUIRE(h != nullptr);
    REQUIRE(h->children.size() == 1);
    const glm::mat4& armM = fresh.Get<TransformComponent>(h->children[0]).localToWorld;
    CHECK(armM[3].x == doctest::Approx(13.0f));
    CHECK(armM[3].y == doctest::Approx(1.0f));
    CHECK(fresh.TryGet<SpinComponent>(h->children[0]) != nullptr);
}

TEST_CASE("Float serialization is clean (shortest float32, no -0.0) and lossless") {
    SceneDescription scene;
    scene.version = kSceneFormatVersion;
    EntityRecord e;
    e.name = "Floaty";
    e.hasTransform = true;
    e.position = {0.3f, 1.0f / 3.0f, -0.0f};
    e.eulerDeg = {-0.0f, 90.0f, 0.0f};
    e.scale = {1.0f, 1.0f, 1.0f};
    scene.entities.push_back(e);

    const std::string toml = WriteToml(scene);
    // Float32 0.3 must not spill its double-promotion tail, and -0.0 must read as 0.0.
    CHECK(toml.find("0.30000001") == std::string::npos);
    CHECK(toml.find("-0.0") == std::string::npos);
    CHECK(toml.find("0.3") != std::string::npos);

    // Still lossless: values round-trip back to the same float32.
    const auto parsed = ParseToml(toml);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    const EntityRecord& r = parsed->entities[0];
    CHECK(r.position.x == 0.3f);
    CHECK(r.position.y == 1.0f / 3.0f);
    CHECK(r.position.z == 0.0f);
    CHECK(r.eulerDeg.y == doctest::Approx(90.0f));
}

TEST_CASE("Prefab instance overrides, removed guids and added entities round-trip through TOML") {
    SceneDescription scene;
    scene.version = kSceneFormatVersion;

    PrefabInstanceRecord inst;
    inst.prefabPath = "ParallaxBackground";
    inst.name = "Backdrop";
    inst.position = {5.0f, 6.0f, 0.0f};
    inst.eulerDeg = {0.0f, 0.0f, 0.0f};
    inst.scale = {1.0f, 1.0f, 1.0f};

    // A per-field override keyed by the source prefab entity's stable guid: only the
    // position key differs, so only that key is stored.
    EntityRecord prefabEntity;
    prefabEntity.guid = 3;
    prefabEntity.name = "BG Hills Far";
    prefabEntity.hasTransform = true;
    prefabEntity.position = {40.0f, 2.5f, 0.0f};
    prefabEntity.scale = {1.0f, 1.0f, 1.0f};
    EntityRecord liveEntity = prefabEntity;
    liveEntity.position = {40.0f, 9.0f, 0.0f};
    const std::string partial = ComputePrefabOverrideToml(liveEntity, prefabEntity);
    REQUIRE_FALSE(partial.empty());
    inst.overrides.push_back(PrefabEntityOverride{.guid = 3, .partialToml = partial});

    // A prefab entity deleted in this instance.
    inst.removedGuids.push_back(7);

    // An entity added beyond the prefab (attaches to the instance root on load).
    EntityRecord addRec;
    addRec.name = "Extra Cloud";
    addRec.parentIndex = -1;
    addRec.hasTransform = true;
    addRec.position = {1.0f, 2.0f, 3.0f};
    addRec.scale = {1.0f, 1.0f, 1.0f};
    inst.addedEntities.push_back(addRec);

    scene.prefabInstances.push_back(inst);

    const auto parsed = ParseToml(WriteToml(scene));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->prefabInstances.size() == 1);
    const PrefabInstanceRecord& pi = parsed->prefabInstances[0];
    CHECK(pi.prefabPath == "ParallaxBackground");
    CHECK(pi.name == "Backdrop");

    REQUIRE(pi.overrides.size() == 1);
    CHECK(pi.overrides[0].guid == 3);
    REQUIRE_FALSE(pi.overrides[0].partialToml.empty());

    // Field-level merge: the overridden key wins; every other key - including a
    // texture the prefab changes *after* the override was authored - tracks the prefab.
    EntityRecord newerPrefab = prefabEntity;
    newerPrefab.name = "BG Hills Far v2"; // a later prefab edit to a NON-overridden key
    const EntityRecord merged = MergePrefabOverride(newerPrefab, pi.overrides[0].partialToml);
    CHECK(merged.position.y == doctest::Approx(9.0f)); // instance override
    CHECK(merged.name == "BG Hills Far v2");           // later prefab edit propagates

    REQUIRE(pi.removedGuids.size() == 1);
    CHECK(pi.removedGuids[0] == 7);

    REQUIRE(pi.addedEntities.size() == 1);
    CHECK(pi.addedEntities[0].name == "Extra Cloud");
    CHECK(pi.addedEntities[0].parentIndex == -1);
    CHECK(pi.addedEntities[0].position.z == doctest::Approx(3.0f));
}

TEST_CASE("Script components round-trip through capture, TOML and apply") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World world = MakeWorld();
    Entity e = world.Create();
    world.Emplace<NameComponent>(e, NameComponent{.name = "Scripted"});
    world.Emplace<TransformComponent>(e, TransformComponent{});
    world.Emplace<ScriptComponent>(e, ScriptComponent{.scripts = {ScriptEntry{.path = "Spinner", .attached = true}}});

    const auto parsed = ParseToml(WriteToml(CaptureScene(world, mreg, treg)));
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == kSceneFormatVersion);
    const EntityRecord& rec = RecordOf(*parsed, "Scripted");
    REQUIRE(rec.scripts.size() == 1);
    CHECK(rec.scripts[0].type == "Spinner");

    World fresh = MakeWorld();
    const auto created = ApplyScene(*parsed, fresh, ApplySceneDeps{});
    const Entity applied = AppliedOf(*parsed, created, "Scripted");
    const auto* sc = fresh.TryGet<ScriptComponent>(applied);
    REQUIRE(sc != nullptr);
    REQUIRE(sc->scripts.size() == 1);
    CHECK(sc->scripts[0].path == "Spinner");
    CHECK(!sc->scripts[0].attached);
}

TEST_CASE("CaptureSubtrees copies multiple roots with local parent links") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World world = MakeWorld();

    Entity a = world.Create();
    world.Emplace<NameComponent>(a, NameComponent{.name = "A"});
    world.Emplace<TransformComponent>(a, TransformComponent{});
    Entity aChild = world.Create();
    world.Emplace<NameComponent>(aChild, NameComponent{.name = "A child"});
    world.Emplace<TransformComponent>(aChild, TransformComponent{});
    REQUIRE(ecs::SetParent(world, aChild, a));

    Entity b = world.Create();
    world.Emplace<NameComponent>(b, NameComponent{.name = "B"});
    world.Emplace<TransformComponent>(b, TransformComponent{});

    Entity outside = world.Create();
    world.Emplace<NameComponent>(outside, NameComponent{.name = "Outside"});

    const auto parsed = ParseToml(WriteToml(CaptureSubtrees(world, {a, b}, mreg, treg)));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 3);
    CHECK(IndexOf(*parsed, "Outside") == -1);
    CHECK(RecordOf(*parsed, "A").parentIndex == -1);
    CHECK(RecordOf(*parsed, "B").parentIndex == -1);
    CHECK(RecordOf(*parsed, "A child").parentIndex == IndexOf(*parsed, "A"));
}

TEST_CASE("UI components round-trip through capture, TOML and apply") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World world = MakeWorld();

    // UI Canvas / Rect / Text serialize generically; UI Image stays bespoke (texture).
    const Entity widget = world.Create();
    world.Emplace<NameComponent>(widget, NameComponent{.name = "Widget"});
    world.Emplace<TransformComponent>(widget, TransformComponent{});
    world.Emplace<ui::UICanvas>(widget, ui::UICanvas{ui::UICanvas::ScaleMode::ScaleWithReference, {1280.f, 720.f}, 3});
    world.Emplace<ui::UIRect>(widget, ui::UIRect{{0.1f, 0.2f}, {0.8f, 0.9f}, {3.f, 4.f}, {-3.f, -4.f}, {0.25f, 0.75f}, glm::vec4{0.f}});
    world.Emplace<ui::UIImage>(widget, ui::UIImage{.color = {0.1f, 0.2f, 0.3f, 0.4f}, .cornerRadius = 7.5f});
    world.Emplace<ui::UIText>(widget, ui::UIText{"Hello", "Custom", 18.f, {0.5f, 0.6f, 0.7f, 0.8f}, ui::UIText::HAlign::Right, ui::UIText::VAlign::Middle, false});

    const std::string toml = WriteToml(CaptureScene(world, mreg, treg));
    // Enums persist as names now (a backward-compatible upgrade from integer keys).
    CHECK(toml.find("scale_mode = 'scale_with_reference'") != std::string::npos);
    CHECK(toml.find("h_align = 'right'") != std::string::npos);

    const auto out = ParseToml(toml);
    REQUIRE(out.has_value());
    const EntityRecord& e = RecordOf(*out, "Widget");
    CHECK(HasGeneric(e, "UI Canvas"));
    CHECK(HasGeneric(e, "UI Rect"));
    CHECK(HasGeneric(e, "UI Text"));

    World fresh = MakeWorld();
    const auto created = ApplyScene(*out, fresh, ApplySceneDeps{});
    const Entity w = AppliedOf(*out, created, "Widget");
    REQUIRE(w.IsValid());

    REQUIRE(fresh.TryGet<ui::UICanvas>(w) != nullptr);
    const ui::UICanvas& canvas = fresh.Get<ui::UICanvas>(w);
    CHECK(canvas.scaleMode == ui::UICanvas::ScaleMode::ScaleWithReference);
    CHECK(canvas.referenceResolution.x == doctest::Approx(1280.f));
    CHECK(canvas.sortBias == 3);

    REQUIRE(fresh.TryGet<ui::UIRect>(w) != nullptr);
    const ui::UIRect& rect = fresh.Get<ui::UIRect>(w);
    CHECK(rect.anchorMin.x == doctest::Approx(0.1f));
    CHECK(rect.anchorMax.y == doctest::Approx(0.9f));
    CHECK(rect.offsetMax.y == doctest::Approx(-4.f));
    CHECK(rect.pivot.y == doctest::Approx(0.75f));

    REQUIRE(fresh.TryGet<ui::UIImage>(w) != nullptr);
    CHECK(fresh.Get<ui::UIImage>(w).cornerRadius == doctest::Approx(7.5f));

    REQUIRE(fresh.TryGet<ui::UIText>(w) != nullptr);
    const ui::UIText& text = fresh.Get<ui::UIText>(w);
    CHECK(text.text == "Hello");
    CHECK(text.fontName == "Custom");
    CHECK(text.pixelSize == doctest::Approx(18.f));
    CHECK(text.color.b == doctest::Approx(0.7f));
    CHECK(text.hAlign == ui::UIText::HAlign::Right);
    CHECK(text.vAlign == ui::UIText::VAlign::Middle);
    CHECK_FALSE(text.wrap);
}

TEST_CASE("Legacy integer UI enum keys still load after the string upgrade") {
    // Shipped scenes wrote UI enums as integers before they became reflected enums;
    // the reader must still accept them (h_align = 2 -> Right, scale_mode = 1 -> ref).
    const auto parsed = ParseToml(
            "[scene]\nkind = '2d'\nname = 'legacy ui'\nversion = 15\n"
            "[[entities]]\nname = 'Label'\n"
            "[entities.ui_canvas]\nscale_mode = 1\n"
            "[entities.ui_text]\ntext = 'Hi'\nh_align = 2\nv_align = 1\n");
    REQUIRE(parsed.has_value());

    World world = MakeWorld();
    const auto created = ApplyScene(*parsed, world, ApplySceneDeps{});
    const Entity label = AppliedOf(*parsed, created, "Label");
    REQUIRE(label.IsValid());
    REQUIRE(world.TryGet<ui::UICanvas>(label) != nullptr);
    CHECK(world.Get<ui::UICanvas>(label).scaleMode == ui::UICanvas::ScaleMode::ScaleWithReference);
    REQUIRE(world.TryGet<ui::UIText>(label) != nullptr);
    CHECK(world.Get<ui::UIText>(label).hAlign == ui::UIText::HAlign::Right);
    CHECK(world.Get<ui::UIText>(label).vAlign == ui::UIText::VAlign::Middle);
}

TEST_CASE("Scene and prefab file helpers read and list through mounted project VFS") {
    namespace fs = std::filesystem;

    if (io::FileSystem::IsInitialized())
    {
        io::FileSystem::Shutdown();
    }
    ClearProjectSceneDirectories();

    const fs::path root = fs::temp_directory_path() / "aethercore_scene_vfs_test";
    std::error_code ec;
    fs::remove_all(root, ec);

    SceneDescription scene;
    scene.name = "MountedScene";
    EntityRecord entity;
    entity.name = "Vfs Entity";
    scene.entities.push_back(entity);

    REQUIRE(io::file_util::WriteText(root / "scenes" / "MountedScene.scene.toml", WriteToml(scene)).has_value());
    REQUIRE(io::file_util::WriteText(root / "assets" / "prefabs" / "MountedPrefab.prefab.toml", WriteToml(scene)).has_value());

    io::FileSystem::Initialize();
    io::FileSystem::Mount("project", root);

    const auto sceneNames = ListSceneFiles();
    CHECK(std::find(sceneNames.begin(), sceneNames.end(), "MountedScene") != sceneNames.end());
    const auto prefabNames = ListPrefabFiles();
    CHECK(std::find(prefabNames.begin(), prefabNames.end(), "MountedPrefab") != prefabNames.end());

    const auto loadedScene = ReadSceneFile("MountedScene");
    REQUIRE(loadedScene.has_value());
    REQUIRE(loadedScene->entities.size() == 1);
    CHECK(loadedScene->entities[0].name == "Vfs Entity");

    const auto loadedPrefab = ReadPrefabFile("MountedPrefab");
    REQUIRE(loadedPrefab.has_value());
    REQUIRE(loadedPrefab->entities.size() == 1);
    CHECK(loadedPrefab->entities[0].name == "Vfs Entity");

    io::FileSystem::Shutdown();
    fs::remove_all(root, ec);
}

TEST_CASE("A cooked .bin older than its .toml source is skipped (stale-cook guard)") {
    // Regression: ReadSceneFile preferred the cooked .bin unconditionally. When the
    // .toml source changed WITHOUT a re-cook (git pull of the tracked .toml while the
    // gitignored .bin lags, a hand-edit, or a tool that rewrites only the .toml), the
    // stale .bin silently won and the edits vanished at runtime - e.g. a level's
    // GoalFlag.NextScene still pointing at the old level, so progression broke.
    namespace fs = std::filesystem;

    if (io::FileSystem::IsInitialized())
    {
        io::FileSystem::Shutdown();
    }
    ClearProjectSceneDirectories();

    const fs::path root = fs::temp_directory_path() / "aethercore_stale_bin_test";
    std::error_code ec;
    fs::remove_all(root, ec);

    io::FileSystem::Initialize();
    io::FileSystem::Mount("project", root);
    // Point the on-disk scene dir at the temp root so the freshness mtime check
    // (which stats ScenesDirectory()) targets these files.
    SetProjectSceneDirectories(root / "scenes", root / "assets" / "prefabs");

    // SaveSceneFile writes a matched .toml + cooked .bin pair, both "Cooked Entity".
    SceneDescription cooked;
    cooked.name = "StaleScene";
    {
        EntityRecord e;
        e.name = "Cooked Entity";
        cooked.entities.push_back(e);
    }
    REQUIRE(SaveSceneFile("StaleScene", cooked));

    const fs::path tomlPath = root / "scenes" / "StaleScene.scene.toml";
    const fs::path binPath = root / "scenes" / "StaleScene.scene.bin";
    REQUIRE(fs::exists(binPath));

    // Matched pair: the cooked binary is trusted.
    {
        const auto fresh = ReadSceneFile("StaleScene");
        REQUIRE(fresh.has_value());
        REQUIRE(fresh->entities.size() == 1);
        CHECK(fresh->entities[0].name == "Cooked Entity");
    }

    // The source is edited AFTER the cook: rewrite the .toml with "Edited Entity" and
    // stamp its mtime newer than the (now stale) .bin.
    SceneDescription edited;
    edited.name = "StaleScene";
    {
        EntityRecord e;
        e.name = "Edited Entity";
        edited.entities.push_back(e);
    }
    REQUIRE(io::file_util::WriteText(tomlPath, WriteToml(edited)).has_value());
    fs::last_write_time(tomlPath, fs::last_write_time(binPath) + std::chrono::seconds(5));

    // The stale .bin (still "Cooked Entity") must NOT win - the newer source does.
    {
        const auto loaded = ReadSceneFile("StaleScene");
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->entities.size() == 1);
        CHECK(loaded->entities[0].name == "Edited Entity");
    }

    // Refreshing the cook (bin newer than source again) restores the fast cooked path.
    fs::last_write_time(binPath, fs::last_write_time(tomlPath) + std::chrono::seconds(5));
    {
        const auto reloaded = ReadSceneFile("StaleScene");
        REQUIRE(reloaded.has_value());
        REQUIRE(reloaded->entities.size() == 1);
        CHECK(reloaded->entities[0].name == "Cooked Entity");
    }

    io::FileSystem::Shutdown();
    ClearProjectSceneDirectories();
    fs::remove_all(root, ec);
}

TEST_CASE("Prefab serialization omits the [scene] header and round-trips header-less") {
    // A prefab is a fragment: writing it with includeSceneHeader=false must not emit a
    // [scene] block, even though the description carries a (2D) kind.
    SceneDescription prefab;
    prefab.kind = SceneKind::Scene2D;
    prefab.name = "Frag";
    EntityRecord e;
    e.name = "Root";
    e.hasTransform = true;
    e.scale = {1, 1, 1};
    prefab.entities.push_back(e);

    const std::string bare = WriteToml(prefab, /*includeSceneHeader=*/false);
    CHECK(bare.find("[scene]") == std::string::npos);
    CHECK(bare.find("kind =") == std::string::npos);
    CHECK(bare.find("features") == std::string::npos);

    // A real scene still gets its header by default.
    CHECK(WriteToml(prefab).find("[scene]") != std::string::npos);

    // Round-trip: parse the bare prefab and re-write it header-less -> stays bare. The
    // old bug re-stamped a header-less prefab as kind='3d' with 3D features.
    const auto parsed = ParseToml(bare);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    const std::string again = WriteToml(*parsed, false);
    CHECK(again.find("[scene]") == std::string::npos);
    CHECK(again.find("meshes_3d") == std::string::npos);
}

TEST_CASE("Capturing an unedited prefab instance reports no removed entities") {
    namespace fs = std::filesystem;
    if (io::FileSystem::IsInitialized())
    {
        io::FileSystem::Shutdown();
    }
    ClearProjectSceneDirectories();

    const fs::path root = fs::temp_directory_path() / "aethercore_prefab_delta_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    io::FileSystem::Initialize();
    io::FileSystem::Mount("project", root);

    // A prefab: a container root (guid 1) with one child (guid 2).
    SceneDescription prefab;
    prefab.name = "CamRig";
    EntityRecord container;
    container.name = "Rig";
    container.parentIndex = -1;
    container.hasTransform = true;
    container.scale = {1, 1, 1};
    EntityRecord child;
    child.name = "Cam";
    child.parentIndex = 0;
    child.hasTransform = true;
    child.scale = {1, 1, 1};
    prefab.entities = {container, child};
    REQUIRE(SavePrefabFile("CamRig", prefab));
    const auto saved = ReadPrefabFile("CamRig"); // guid-assigned copy
    REQUIRE(saved.has_value());

    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);
    World world = MakeWorld();

    const Entity instanceRoot = InstantiatePrefabInstance("CamRig", *saved, world, ApplySceneDeps{}, glm::mat4(1.0f));
    REQUIRE(instanceRoot.IsValid());

    // Capturing with no edits must not report the instance root's own guid (or any
    // other) as removed - regression for the "removed = [1]" bug that wiped the whole
    // instance on the next load.
    const SceneDescription captured = CaptureScene(world, mreg, treg);
    REQUIRE(captured.prefabInstances.size() == 1);
    const PrefabInstanceRecord& pi = captured.prefabInstances[0];
    CHECK(pi.removedGuids.empty());
    CHECK(pi.overrides.empty());
    CHECK(pi.addedEntities.empty());

    io::FileSystem::Shutdown();
    fs::remove_all(root, ec);
}

TEST_CASE("RestoreSceneInPlace round-trips the Play/Stop path without asserting") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);

    World world = MakeWorld();
    world.SetSceneKind(SceneKind::Scene2D);
    world.SetSceneFeatures(SceneFeatureFlags::Sprites | SceneFeatureFlags::Physics2D);

    Entity cam = world.Create();
    world.Emplace<NameComponent>(cam, NameComponent{.name = "Camera"});
    world.Emplace<TransformComponent>(cam, TransformComponent{});
    world.Emplace<CameraComponent>(cam, CameraComponent{.projection = CameraProjection::Orthographic, .fovDegrees = 55.0f, .orthographicHeight = 18.0f});
    world.Emplace<OrbitCameraComponent>(cam, OrbitCameraComponent{.target = {0, 1, 0}, .yaw = 15.0f, .pitch = 25.0f, .distance = 8.0f});
    world.Emplace<MainCameraComponent>(cam, MainCameraComponent{});

    // renderer and a sprite - the render/active markers reset must also strip.
    Entity disabledParent = world.Create();
    world.Emplace<NameComponent>(disabledParent, NameComponent{.name = "DisabledParent"});
    world.Emplace<TransformComponent>(disabledParent, TransformComponent{});
    world.Emplace<DisabledComponent>(disabledParent);
    Entity child = world.Create();
    world.Emplace<NameComponent>(child, NameComponent{.name = "Child"});
    world.Emplace<TransformComponent>(child, TransformComponent{});
    world.Emplace<MeshRendererComponent>(child, MeshRendererComponent{.visible = false, .castShadows = false});
    ecs::SetParent(world, child, disabledParent);
    Entity sprite = world.Create();
    world.Emplace<NameComponent>(sprite, NameComponent{.name = "Sprite"});
    world.Emplace<TransformComponent>(sprite, TransformComponent{});
    world.Emplace<SpriteRendererComponent>(sprite);

    const SceneDescription snapshot = CaptureScene(world, mreg, treg);
    CHECK(snapshot.kind == SceneKind::Scene2D);
    CHECK(snapshot.features == (SceneFeatureFlags::Sprites | SceneFeatureFlags::Physics2D));
    const auto serialized = ParseToml(WriteToml(snapshot));
    REQUIRE(serialized.has_value());
    CHECK(serialized->kind == SceneKind::Scene2D);
    REQUIRE(serialized->entities[0].camera.has_value());
    CHECK(serialized->entities[0].camera->projection == CameraProjection::Orthographic);
    CHECK(serialized->entities[0].camera->orthographicHeight == doctest::Approx(18.0f));
    const std::uint32_t camIdBefore = cam.id;

    Entity spawned = world.Create();
    world.Emplace<NameComponent>(spawned, NameComponent{.name = "SpawnedOrb"});
    world.Emplace<TransformComponent>(spawned, TransformComponent{});
    ecs::SetParent(world, sprite, spawned);
    world.Remove<DisabledComponent>(disabledParent);
    world.EmplaceOrReplace<DisabledComponent>(sprite);
    world.SetSceneKind(SceneKind::Scene3D);
    world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));

    const std::vector<Entity> restored = RestoreSceneInPlace(snapshot, world, ApplySceneDeps{});
    REQUIRE(restored.size() == snapshot.entities.size());
    CHECK(world.GetSceneKind() == SceneKind::Scene2D);
    CHECK(world.GetSceneFeatures() == (SceneFeatureFlags::Sprites | SceneFeatureFlags::Physics2D));

    const Entity camAfter = AppliedOf(snapshot, restored, "Camera");
    CHECK(camAfter.id == camIdBefore);
    REQUIRE(world.TryGet<OrbitCameraComponent>(camAfter) != nullptr);
    CHECK(world.Get<CameraComponent>(camAfter).projection == CameraProjection::Orthographic);
    CHECK(world.Get<CameraComponent>(camAfter).orthographicHeight == doctest::Approx(18.0f));
    CHECK(world.Get<OrbitCameraComponent>(camAfter).distance == doctest::Approx(8.0f));
    CHECK(world.Has<MainCameraComponent>(camAfter));

    const Entity dp = AppliedOf(snapshot, restored, "DisabledParent");
    const Entity sp = AppliedOf(snapshot, restored, "Sprite");
    CHECK(world.Has<DisabledComponent>(dp));
    CHECK(!world.Has<DisabledComponent>(sp));

    const auto* sh = world.TryGet<HierarchyComponent>(sp);
    CHECK((sh == nullptr || !sh->parent.IsValid()));

    const Entity ch = AppliedOf(snapshot, restored, "Child");
    REQUIRE(world.TryGet<MeshRendererComponent>(ch) != nullptr);
    CHECK(world.Get<MeshRendererComponent>(ch).visible == false);

    CHECK(world.GetRegistry().valid(World::ToEntt(spawned)) == false);
}

TEST_CASE("Restoring strips a live keyboard capture left behind by Play") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);

    World world = MakeWorld();
    Entity field = world.Create();
    world.Emplace<NameComponent>(field, NameComponent{.name = "Field"});
    world.Emplace<TransformComponent>(field, TransformComponent{});

    const SceneDescription snapshot = CaptureScene(world, mreg, treg);

    // Stopping while a text box is mid-edit: the marker is runtime-only, so it is never in the
    // snapshot and re-applying cannot overwrite it. Left behind, it wedges every editor shortcut
    // that stands down for in-game text input, for the rest of the session.
    world.Emplace<ui::UIKeyboardCapture>(field);

    const std::vector<Entity> restored = RestoreSceneInPlace(snapshot, world, ApplySceneDeps{});
    REQUIRE(restored.size() == 1);
    CHECK_FALSE(world.Has<ui::UIKeyboardCapture>(restored[0]));
}

TEST_CASE("2D sprite authoring fields survive a scene save and load round trip") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);

    World world = MakeWorld();
    world.SetSceneKind(SceneKind::Scene2D);
    world.SetSceneFeatures(SceneFeatureFlags::Sprites);
    const Entity entity = world.Create();
    world.Emplace<NameComponent>(entity, NameComponent{.name = "Saved Sprite"});
    world.Emplace<TransformComponent>(entity, TransformComponent{});

    SpriteRendererComponent sprite;
    sprite.texturePath = "project://assets/hero.png";
    sprite.atlasPath = "project://assets/hero.spriteatlas.toml";
    sprite.spriteId = AssetObjectId{0x1234abcd};
    sprite.uvRect = {0.125f, 0.25f, 0.375f, 0.5f};
    sprite.tint = {0.2f, 0.4f, 0.6f, 0.8f};
    sprite.pixelSize = {48.0f, 64.0f};
    sprite.pivot = {0.25f, 0.75f};
    sprite.pixelsPerUnit = 32.0f;
    sprite.sortingLayer = 3;
    sprite.orderInLayer = -7;
    sprite.blendMode = SpriteBlendMode::Multiply;
    sprite.visible = false;
    sprite.flipX = true;
    sprite.flipY = true;
    sprite.pixelSnap = true;
    world.Emplace<SpriteRendererComponent>(entity, sprite);

    SpriteAnimatorComponent animator;
    animator.animationPath = "project://assets/hero.spriteanim.toml";
    animator.speed = 1.75f;
    animator.startFrame = 4;
    animator.loopMode = SpriteAnimationLoopMode::Hold;
    animator.useAssetLoopMode = false;
    animator.autoplay = false;
    // Runtime preview state must never become authored scene state.
    animator.frameTime = 0.42f;
    animator.currentFrame = 9;
    animator.playing = true;
    animator.initialized = true;
    world.Emplace<SpriteAnimatorComponent>(entity, animator);

    const SceneDescription captured = CaptureScene(world, mreg, treg);
    const auto parsed = ParseToml(WriteToml(captured));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    REQUIRE(parsed->entities[0].sprite.has_value());
    REQUIRE(parsed->entities[0].spriteAnimator.has_value());

    const SpriteRendererComponent& savedSprite = *parsed->entities[0].sprite;
    CHECK(savedSprite.texturePath == sprite.texturePath);
    CHECK(savedSprite.atlasPath == sprite.atlasPath);
    CHECK(savedSprite.tint == sprite.tint);
    // The animator drives the frame (spriteId/uvRect/pixelSize/pivot) every update and
    // re-applies startFrame on load, so capture canonicalizes those to a fixed value
    // instead of persisting whatever preview frame was live - otherwise a previewing
    // animation floods every save with false-positive frame diffs.
    CHECK_FALSE(savedSprite.spriteId.IsValid());
    CHECK(savedSprite.uvRect == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    CHECK(savedSprite.pixelSize == glm::vec2(0.0f));
    CHECK(savedSprite.pivot == glm::vec2(0.0f));
    CHECK(savedSprite.pixelsPerUnit == doctest::Approx(sprite.pixelsPerUnit));
    CHECK(savedSprite.sortingLayer == sprite.sortingLayer);
    CHECK(savedSprite.orderInLayer == sprite.orderInLayer);
    CHECK(savedSprite.blendMode == sprite.blendMode);
    CHECK(savedSprite.visible == sprite.visible);
    CHECK(savedSprite.flipX == sprite.flipX);
    CHECK(savedSprite.flipY == sprite.flipY);
    CHECK(savedSprite.pixelSnap == sprite.pixelSnap);

    const SpriteAnimatorComponent& savedAnimator = *parsed->entities[0].spriteAnimator;
    CHECK(savedAnimator.animationPath == animator.animationPath);
    CHECK(savedAnimator.speed == doctest::Approx(animator.speed));
    CHECK(savedAnimator.startFrame == animator.startFrame);
    CHECK(savedAnimator.loopMode == animator.loopMode);
    CHECK(savedAnimator.useAssetLoopMode == animator.useAssetLoopMode);
    CHECK(savedAnimator.autoplay == animator.autoplay);
    CHECK(savedAnimator.frameTime == 0.0f);
    CHECK(savedAnimator.currentFrame == 0);
    CHECK_FALSE(savedAnimator.playing);
    CHECK_FALSE(savedAnimator.initialized);

    World loaded = MakeWorld();
    const std::vector<Entity> applied = ApplyScene(*parsed, loaded, ApplySceneDeps{});
    REQUIRE(applied.size() == 1);
    CHECK(loaded.GetSceneKind() == SceneKind::Scene2D);
    CHECK(loaded.GetSceneFeatures() == SceneFeatureFlags::Sprites);
    REQUIRE(loaded.TryGet<SpriteRendererComponent>(applied[0]) != nullptr);
    REQUIRE(loaded.TryGet<SpriteAnimatorComponent>(applied[0]) != nullptr);
    CHECK(loaded.Get<SpriteRendererComponent>(applied[0]).blendMode == SpriteBlendMode::Multiply);
    CHECK(loaded.Get<SpriteAnimatorComponent>(applied[0]).animationPath == animator.animationPath);
}

TEST_CASE("Physics2D components survive a scene save and load round trip") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);

    World world = MakeWorld();
    world.SetSceneKind(SceneKind::Scene2D);
    world.SetSceneFeatures(SceneFeatureFlags::Sprites | SceneFeatureFlags::Physics2D);

    const Entity anchor = world.Create();
    world.Emplace<NameComponent>(anchor, NameComponent{.name = "Anchor"});
    world.Emplace<TransformComponent>(anchor, TransformComponent{});
    world.Emplace<RigidBody2DComponent>(anchor, RigidBody2DComponent{.bodyType = Body2DType::Static});
    world.Emplace<Collider2DComponent>(anchor, Collider2DComponent{.size = {10.0f, 0.5f}});

    const Entity entity = world.Create();
    world.Emplace<NameComponent>(entity, NameComponent{.name = "Crate"});
    world.Emplace<TransformComponent>(entity, TransformComponent{});

    RigidBody2DComponent rigid;
    rigid.body.value = 0xdeadbeefu; // runtime handle must never become authored state
    rigid.bodyType = Body2DType::Kinematic;
    rigid.gravityScale = 0.5f;
    rigid.linearDamping = 0.25f;
    rigid.angularDamping = 0.75f;
    rigid.fixedRotation = true;
    rigid.continuousCollision = true;
    rigid.allowSleeping = false;
    rigid.startAwake = false;
    world.Emplace<RigidBody2DComponent>(entity, rigid);

    Collider2DComponent collider;
    collider.shape = Collider2DShape::Polygon;
    collider.size = {2.0f, 3.0f};
    collider.radius = 0.25f;
    collider.capsuleHeight = 1.5f;
    collider.offset = {0.1f, -0.2f};
    collider.density = 2.5f;
    collider.friction = 0.9f;
    collider.restitution = 0.3f;
    collider.isTrigger = true;
    collider.categoryBits = 0x00000004u;
    collider.maskBits = 0x0000ff00u;
    collider.groupIndex = -3;
    collider.points = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.0f, 0.75f}};
    collider.shapes = {1u, 2u}; // runtime shape ids must be stripped
    world.Emplace<Collider2DComponent>(entity, collider);

    Joint2DComponent joint;
    joint.type = Joint2DType::Prismatic;
    joint.target = anchor;
    joint.anchor = {0.25f, 0.5f};
    joint.connectedAnchor = {-0.25f, -0.5f};
    joint.axis = {0.0f, 1.0f};
    joint.minLimit = -1.5f;
    joint.maxLimit = 2.5f;
    joint.length = 3.0f;
    joint.motorSpeed = 4.0f;
    joint.maxMotorForce = 50.0f;
    joint.enableLimit = true;
    joint.enableMotor = true;
    joint.collideConnected = true;
    joint.jointId = 77; // runtime handle
    world.Emplace<Joint2DComponent>(entity, joint);

    const SceneDescription captured = CaptureScene(world, mreg, treg);
    const auto parsed = ParseToml(WriteToml(captured));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 2);
    const EntityRecord& rec = parsed->entities[1];
    REQUIRE(rec.joint2D.has_value());
    // Rigid Body 2D and Collider 2D serialize generically; their values (including the
    // polygon points flat List) are verified on the applied components below.
    CHECK(HasGeneric(rec, "Rigid Body 2D"));
    CHECK(HasGeneric(rec, "Collider 2D"));

    const Joint2DComponent& savedJoint = *rec.joint2D;
    CHECK(savedJoint.type == joint.type);
    CHECK(savedJoint.anchor == joint.anchor);
    CHECK(savedJoint.connectedAnchor == joint.connectedAnchor);
    CHECK(savedJoint.axis == joint.axis);
    CHECK(savedJoint.minLimit == doctest::Approx(joint.minLimit));
    CHECK(savedJoint.maxLimit == doctest::Approx(joint.maxLimit));
    CHECK(savedJoint.length == doctest::Approx(joint.length));
    CHECK(savedJoint.motorSpeed == doctest::Approx(joint.motorSpeed));
    CHECK(savedJoint.maxMotorForce == doctest::Approx(joint.maxMotorForce));
    CHECK(savedJoint.enableLimit == joint.enableLimit);
    CHECK(savedJoint.enableMotor == joint.enableMotor);
    CHECK(savedJoint.collideConnected == joint.collideConnected);
    CHECK(savedJoint.jointId == 0);
    CHECK(rec.joint2DTargetIndex == 0);

    World loaded = MakeWorld();
    const std::vector<Entity> applied = ApplyScene(*parsed, loaded, ApplySceneDeps{});
    REQUIRE(applied.size() == 2);
    REQUIRE(loaded.TryGet<RigidBody2DComponent>(applied[1]) != nullptr);
    REQUIRE(loaded.TryGet<Collider2DComponent>(applied[1]) != nullptr);
    REQUIRE(loaded.TryGet<Joint2DComponent>(applied[1]) != nullptr);
    const RigidBody2DComponent& outRigid = loaded.Get<RigidBody2DComponent>(applied[1]);
    CHECK(outRigid.bodyType == rigid.bodyType);
    CHECK(outRigid.gravityScale == doctest::Approx(rigid.gravityScale));
    CHECK(outRigid.fixedRotation == rigid.fixedRotation);
    CHECK(outRigid.startAwake == rigid.startAwake);
    const Collider2DComponent& outCollider = loaded.Get<Collider2DComponent>(applied[1]);
    CHECK(outCollider.shape == collider.shape);
    CHECK(outCollider.size == collider.size);
    CHECK(outCollider.density == doctest::Approx(collider.density));
    CHECK(outCollider.categoryBits == collider.categoryBits);
    CHECK(outCollider.groupIndex == collider.groupIndex);
    REQUIRE(outCollider.points.size() == 3);
    CHECK(outCollider.points[2] == glm::vec2{0.0f, 0.75f});
    // Cold load resolves the joint target through the scene-local index.
    CHECK(loaded.Get<Joint2DComponent>(applied[1]).target == applied[0]);
}

TEST_CASE("Day Night component and camera background survive a scene round trip") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);

    World world = MakeWorld();
    const Entity sun = world.Create();
    world.Emplace<NameComponent>(sun, NameComponent{.name = "Sun"});
    world.Emplace<TransformComponent>(sun, TransformComponent{});
    world.Emplace<DayNightComponent>(sun, DayNightComponent{.animate = false, .timeOfDayHours = 17.5f, .timeSpeedSecondsPerSecond = 120.0f});

    const Entity cam = world.Create();
    world.Emplace<NameComponent>(cam, NameComponent{.name = "Cam"});
    world.Emplace<TransformComponent>(cam, TransformComponent{});
    CameraComponent camera{};
    camera.background = CameraBackground::SolidColour;
    camera.clearColor = {0.2f, 0.3f, 0.4f};
    world.Emplace<CameraComponent>(cam, camera);

    const SceneDescription captured = CaptureScene(world, mreg, treg);
    const auto parsed = ParseToml(WriteToml(captured));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 2);
    // Day Night serializes generically now; read its authored fields from `reflected`.
    const EntityRecord& sunRec = parsed->entities[0];
    const reflect::FieldValue* dnAnimate = GenericVal(sunRec, "Day Night", "animate");
    const reflect::FieldValue* dnTimeOfDay = GenericVal(sunRec, "Day Night", "time_of_day");
    const reflect::FieldValue* dnTimeSpeed = GenericVal(sunRec, "Day Night", "time_speed");
    REQUIRE(dnAnimate != nullptr);
    REQUIRE(dnTimeOfDay != nullptr);
    REQUIRE(dnTimeSpeed != nullptr);
    CHECK_FALSE(dnAnimate->boolean);
    CHECK(dnTimeOfDay->num == doctest::Approx(17.5f));
    CHECK(dnTimeSpeed->num == doctest::Approx(120.0f));
    REQUIRE(parsed->entities[1].camera.has_value());
    CHECK(parsed->entities[1].camera->background == CameraBackground::SolidColour);
    CHECK(parsed->entities[1].camera->clearColor.g == doctest::Approx(0.3f));

    World loaded = MakeWorld();
    const std::vector<Entity> applied = ApplyScene(*parsed, loaded, ApplySceneDeps{});
    REQUIRE(applied.size() == 2);
    REQUIRE(loaded.TryGet<DayNightComponent>(applied[0]) != nullptr);
    CHECK(loaded.Get<DayNightComponent>(applied[0]).timeOfDayHours == doctest::Approx(17.5f));
    // Day Night implies 3D lighting for the loaded scene.
    CHECK(HasSceneFeature(loaded.GetSceneFeatures(), SceneFeatureFlags::Lighting3D));
}

TEST_CASE("Scene apply keeps cross-domain physics records (Unity-style) but never both on one entity") {
    // 2D physics in a 3D scene is legal now - hybrid games depend on it. The
    // records apply and imply the feature flag.
    const char* toml3D = "[scene]\nname = 'hybrid'\nkind = '3d'\nversion = 14\nfeatures = ['physics_3d']\n\n"
                         "[[entities]]\nname = 'Sprite body'\nposition = [0.0, 1.0, 0.0]\neuler = [0.0, 0.0, 0.0]\nscale = [1.0, 1.0, 1.0]\n"
                         "[entities.rigid_body_2d]\nbody_type = 'dynamic'\n"
                         "[entities.collider_2d]\nshape = 'box'\n";
    const auto parsed = ParseToml(toml3D);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    CHECK(HasGeneric(parsed->entities[0], "Rigid Body 2D"));

    World world = MakeWorld();
    const auto created = ApplyScene(*parsed, world, ApplySceneDeps{});
    REQUIRE(created.size() == 1);
    CHECK(world.Has<RigidBody2DComponent>(created[0]));
    CHECK(world.Has<Collider2DComponent>(created[0]));
    CHECK(HasSceneFeature(world.GetSceneFeatures(), SceneFeatureFlags::Physics2D));

    // The one rule that remains: an entity carrying BOTH domains keeps only
    // the set matching the scene kind.
    const char* tomlBoth = "[scene]\nname = 'both'\nkind = '2d'\nversion = 14\nfeatures = ['sprites', 'physics_2d']\n\n"
                           "[[entities]]\nname = 'Contested'\nposition = [0.0, 1.0, 0.0]\neuler = [0.0, 0.0, 0.0]\nscale = [1.0, 1.0, 1.0]\n"
                           "[entities.rigid_body_2d]\nbody_type = 'dynamic'\n"
                           "[entities.collider_2d]\nshape = 'box'\n"
                           "[entities.physics]\nshape = 'box'\nmotion = 'dynamic'\n";
    const auto parsedBoth = ParseToml(tomlBoth);
    REQUIRE(parsedBoth.has_value());
    REQUIRE(parsedBoth->entities.size() == 1);
    CHECK(parsedBoth->entities[0].physics.has_value());
    CHECK(HasGeneric(parsedBoth->entities[0], "Rigid Body 2D"));

    World world2D = MakeWorld();
    const auto created2D = ApplyScene(*parsedBoth, world2D, ApplySceneDeps{});
    REQUIRE(created2D.size() == 1);
    CHECK(world2D.Has<RigidBody2DComponent>(created2D[0]));
    CHECK_FALSE(world2D.Has<RigidBodyComponent>(created2D[0]));
}

TEST_CASE("Collider 2D polygon points are reachable through reflection as a flat List") {
    // Previously points were hand-parsed and invisible to MCP; now a flat List field.
    const reflect::ComponentType* rt = reflect::FindComponentType("Collider 2D");
    REQUIRE(rt != nullptr);
    const reflect::FieldDesc* field = rt->FindField("points");
    REQUIRE(field != nullptr);
    CHECK(field->type == reflect::FieldType::List);

    Collider2DComponent col{};
    col.points = {{0.0f, 0.5f}, {0.5f, -0.5f}, {-0.5f, -0.5f}};

    // get -> JSON is a flat array of [x, y] pairs (not an array of objects).
    const nlohmann::json j = editor::FieldValueToJson(field->get(&col), field);
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 3);
    REQUIRE(j[1].is_array());
    CHECK(j[1][0].get<double>() == doctest::Approx(0.5));

    Collider2DComponent restored{};
    field->set(&restored, editor::JsonToFieldValue(j, *field));
    REQUIRE(restored.points.size() == 3);
    CHECK(restored.points[2] == glm::vec2{-0.5f, -0.5f});
}

TEST_CASE("Tile Map component survives a scene round trip and implies the feature") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);

    World world = MakeWorld();
    world.SetSceneKind(SceneKind::Scene2D);
    world.SetSceneFeatures(SceneFeatureFlags::Sprites); // deliberately missing Tilemaps

    const Entity entity = world.Create();
    world.Emplace<NameComponent>(entity, NameComponent{.name = "Map"});
    world.Emplace<TransformComponent>(entity, TransformComponent{});
    TileMapComponent tileMap;
    tileMap.tilemapPath = "project://assets/tilemaps/room.tilemap";
    tileMap.tint = {0.5f, 0.6f, 0.7f, 1.0f};
    tileMap.sortingLayer = -2;
    tileMap.orderInLayer = 4;
    tileMap.visibleLayerMask = 0x5u;
    tileMap.visible = false;
    world.Emplace<TileMapComponent>(entity, tileMap);

    const SceneDescription captured = CaptureScene(world, mreg, treg);
    const auto parsed = ParseToml(WriteToml(captured));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    // Tile Map serializes generically now; verify the authored values on the applied
    // component (a full capture -> TOML -> parse -> apply round-trip).
    CHECK(HasGeneric(parsed->entities[0], "Tile Map"));

    World loaded = MakeWorld();
    const auto applied = ApplyScene(*parsed, loaded, ApplySceneDeps{});
    REQUIRE(applied.size() == 1);
    REQUIRE(loaded.TryGet<TileMapComponent>(applied[0]) != nullptr);
    const TileMapComponent& saved = loaded.Get<TileMapComponent>(applied[0]);
    CHECK(saved.tilemapPath == tileMap.tilemapPath);
    CHECK(saved.tint.g == doctest::Approx(0.6f));
    CHECK(saved.sortingLayer == -2);
    CHECK(saved.orderInLayer == 4);
    CHECK(saved.visibleLayerMask == 0x5u);
    CHECK_FALSE(saved.visible);
    // The Tile Map component implies the Tilemaps feature via reflection requiredFeatures.
    CHECK(HasSceneFeature(loaded.GetSceneFeatures(), SceneFeatureFlags::Tilemaps));
}

TEST_CASE("v14 2D scenes gain the tilemaps feature via the v15 migration") {
    const auto migrated = ParseToml("[scene]\nname = 'v14 2d'\nkind = '2d'\nversion = 14\nfeatures = ['sprites', 'physics_2d']\n");
    REQUIRE(migrated.has_value());
    CHECK(HasSceneFeature(migrated->features, SceneFeatureFlags::Tilemaps));

    const auto untouched3D = ParseToml("[scene]\nname = 'v14 3d'\nkind = '3d'\nversion = 14\nfeatures = ['physics_3d']\n");
    REQUIRE(untouched3D.has_value());
    CHECK_FALSE(HasSceneFeature(untouched3D->features, SceneFeatureFlags::Tilemaps));

    const auto modern2D = ParseToml("[scene]\nname = 'v15 2d'\nkind = '2d'\nversion = 15\nfeatures = ['sprites']\n");
    REQUIRE(modern2D.has_value());
    CHECK_FALSE(HasSceneFeature(modern2D->features, SceneFeatureFlags::Tilemaps));
}

TEST_CASE("v9 scene fixtures migrate feature defaults without changing source version") {
    const auto readFixture = [](std::string_view name)
    {
        return io::file_util::ReadText(std::filesystem::path{AETHER_TESTS_SOURCE_DIR} / "fixtures" / "scenes" / name);
    };

    const auto legacy2DText = readFixture("v9-2d.scene.toml");
    REQUIRE(legacy2DText.has_value());
    const auto legacy2D = ParseToml(*legacy2DText);
    REQUIRE(legacy2D.has_value());
    CHECK(legacy2D->version == 9);
    CHECK(legacy2D->kind == SceneKind::Scene2D);
    CHECK(legacy2D->features == (SceneFeatureFlags::Sprites | SceneFeatureFlags::Physics2D | SceneFeatureFlags::Tilemaps));

    const auto legacy3DText = readFixture("v9-3d.scene.toml");
    REQUIRE(legacy3DText.has_value());
    const auto legacy3D = ParseToml(*legacy3DText);
    REQUIRE(legacy3D.has_value());
    CHECK(legacy3D->version == 9);
    CHECK(legacy3D->kind == SceneKind::Scene3D);
    CHECK(legacy3D->features == DefaultSceneFeatures(SceneKind::Scene3D));
}

TEST_CASE("Moving a prefab instance root does not override its children's transforms") {
    // Records are world-space, so every child's captured transform moves with the
    // instance root. Diffing that directly against the prefab wrote a transform
    // override for every child of every instance the user had ever dragged, and those
    // overrides then pinned the children against later edits to the prefab itself.
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry mreg(sink, treg);

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_prefab_override_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    SetProjectSceneDirectories(dir / "scenes", dir);

    // A prefab whose child sits offset from its root.
    World authoring = MakeWorld();
    Entity prefabRoot = authoring.Create();
    authoring.Emplace<NameComponent>(prefabRoot, NameComponent{.name = "Turret"});
    authoring.Emplace<TransformComponent>(prefabRoot, TransformComponent{});
    Entity prefabBarrel = authoring.Create();
    authoring.Emplace<NameComponent>(prefabBarrel, NameComponent{.name = "Barrel"});
    authoring.Emplace<TransformComponent>(prefabBarrel, TransformComponent{.localToWorld = ComposeTransform({0, 2, 0}, {0, 0, 0}, {1, 1, 1})});
    ecs::SetParent(authoring, prefabBarrel, prefabRoot);
    REQUIRE(SavePrefabFile("Turret", CapturePrefab(authoring, prefabRoot, mreg, treg)));

    const auto prefab = ReadPrefabFile("Turret");
    REQUIRE(prefab.has_value());

    // Place an instance, then drag it somewhere else entirely.
    World live = MakeWorld();
    const Entity instance = InstantiatePrefabInstance("Turret", *prefab, live, ApplySceneDeps{}, glm::mat4(1.0f));
    REQUIRE(instance.IsValid());
    ecs::SetWorldTransform(live, instance, ComposeTransform({10, 0, -4}, {0, 90, 0}, {1, 1, 1}));

    const SceneDescription captured = CaptureScene(live, mreg, treg);
    REQUIRE(captured.prefabInstances.size() == 1);
    // The move belongs to the instance record itself, not to per-child overrides.
    CHECK(captured.prefabInstances[0].position.x == doctest::Approx(10.0f));
    CHECK(captured.prefabInstances[0].overrides.empty());

    ClearProjectSceneDirectories();
    std::filesystem::remove_all(dir);
}

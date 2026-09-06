using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// One entry in the spawnable-prop catalogue: either a built-in primitive (cube/sphere,
/// <see cref="ModelPath"/> null) or a real glTF model with a hand-fitted collider
/// (<see cref="ModelPath"/> set). Adding a prop is one line in
/// <see cref="PropSpawner.Catalog"/>, not a new code path - <see cref="SpawnMenu"/>
/// builds its buttons straight off this array and every button calls the same
/// <see cref="PropSpawner.SpawnProp"/>.
///
/// Model entries' collider params come from measuring each source glTF's own accessor
/// bounds (see `projects/Sandbox/assets/PropCatalogExtension.md`) - primitives fit
/// exactly by construction (Size IS the collider size), so they need none of the
/// collider fields below and leave them at their defaults.
/// </summary>
public enum PropColliderShape { Box, Sphere, Capsule, Cylinder }

public readonly record struct PropDef(
    string Name,
    bool IsSphere,
    float Size,
    float Mass,
    Vector3 Color,
    string? ModelPath = null,
    PropColliderShape ColliderShape = PropColliderShape.Box,
    Vector3 ColliderHalfExtents = default,
    float ColliderRadius = 0f,
    float ColliderHalfHeight = 0f,
    Vector3 ColliderCenter = default);

/// <summary>
/// Owns the actual spawn logic - creating the entity, its mesh/material/body,
/// the launch, and the live-prop cap - shared by the F-key hotkey and
/// <see cref="SpawnMenu"/>'s catalogue buttons so there is exactly one way a
/// prop comes into the world.
///
/// Pressing F cycles through <see cref="Catalog"/> in order; the menu (Q)
/// picks a specific entry by index via <see cref="SpawnProp(int)"/>.
///
/// <see cref="_spawned"/> is a plain instance field on this script, not a
/// static - each PropSpawner only tracks props it created itself. That
/// matters once a networked version of this exists: a static registry would
/// have to be reconciled across peers, while an instance field is naturally
/// per-authority and just needs the spawn call itself replicated (e.g. as a
/// [NetRpc(Server)] that the owner calls) - not attempted here, this script
/// is single-player only for now.
///
/// FIXED UPSTREAM (was a KNOWN SDK LIMITATION here): Physics.SetLinearVelocity
/// called the same frame as AddBoxBody/AddSphereBody used to be silently dropped
/// on X/Z - the body did not exist yet, so the velocity write had nothing to
/// land on and only gravity (applied later, once the body baked) ever moved the
/// prop. PhysicsExports.cpp now seeds RigidBodyComponent::initialVelocity when
/// the body is not yet baked, and FlushPendingBodies applies it at creation -
/// confirmed live below: a spawned prop now visibly drifts in SpawnSpeed's
/// direction instead of falling straight down. No script-side change was needed;
/// this call was always the documented, correct way to use the API.
///
/// Grabbable by <see cref="PhysicsGun"/>: every spawned prop is tagged the same
/// "grabbable" as the scene-authored arena props (tagged once by PhysicsGun.OnAttach
/// from its "Props" group) - one shared tag, not two lists to keep in sync.
/// </summary>
public sealed class PropSpawner : EntityScript
{
    /// <summary>The spawnable catalogue - one row per prop the player can drop.
    /// Both the F key and the spawn menu read this same array.
    ///
    /// 30 entries: 6 built-in primitives (unchanged shape/mass/colour, renamed to
    /// "Block" so they read as deliberately distinct from the modelled crates below,
    /// not near-duplicates), 16 real CC0 models from Kenney's Factory/Furniture Kits
    /// and Quaternius (via Poly Pizza), and 6 self-authored construction-stock props.
    /// Every model entry's collider is hand-fitted from that model's own glTF
    /// accessor bounds - see `projects/Sandbox/assets/PropCatalogExtension.md` for the
    /// full derivation and the primitives-poorly-served list (Cone/Table/Chair/
    /// Hopper/ThrusterBody - flagged there as convex-hull candidates, not bugs here).
    /// Model paths are relative to `project://assets/models/`.</summary>
    public static readonly PropDef[] Catalog =
    {
        // ── Primitives (6) - exact-collider-by-construction baseline ──────────────
        new("Small Block", IsSphere: false, Size: 0.35f, Mass: 4f, Color: new Vector3(0.55f, 0.42f, 0.25f)),
        new("Medium Block", IsSphere: false, Size: 0.6f, Mass: 12f, Color: new Vector3(0.65f, 0.5f, 0.3f)),
        new("Heavy Block", IsSphere: false, Size: 0.9f, Mass: 40f, Color: new Vector3(0.35f, 0.3f, 0.28f)),
        new("Marble", IsSphere: true, Size: 0.25f, Mass: 2f, Color: new Vector3(0.85f, 0.35f, 0.2f)),
        new("Ball", IsSphere: true, Size: 0.5f, Mass: 8f, Color: new Vector3(0.2f, 0.6f, 0.85f)),
        new("Heavy Ball", IsSphere: true, Size: 0.8f, Mass: 60f, Color: new Vector3(0.3f, 0.3f, 0.32f)),

        // ── Crates (6) - Kenney Factory/Furniture Kit + Quaternius, CC0 ───────────
        new("Small Wooden Crate", IsSphere: false, Size: 1f, Mass: 4f, Color: default,
            ModelPath: "project://assets/models/Props/CrateSmall.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.298f, 0.275f, 0.250f), ColliderCenter: new Vector3(0f, 0.275f, 0f)),
        new("Large Wooden Crate", IsSphere: false, Size: 1f, Mass: 25f, Color: default,
            ModelPath: "project://assets/models/Props/CrateLarge.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.550f, 0.275f, 0.500f), ColliderCenter: new Vector3(0f, 0.275f, 0f)),
        new("Long Wooden Crate", IsSphere: false, Size: 1f, Mass: 15f, Color: default,
            ModelPath: "project://assets/models/Props/CrateLong.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.298f, 0.275f, 0.500f), ColliderCenter: new Vector3(0f, 0.275f, 0f)),
        new("Cardboard Box", IsSphere: false, Size: 1f, Mass: 2f, Color: default,
            ModelPath: "project://assets/models/Props/CardboardBox.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.106f, 0.1405f, 0.106f), ColliderCenter: new Vector3(0.106f, 0.1405f, -0.106f)),
        new("Barrel", IsSphere: false, Size: 1f, Mass: 20f, Color: default,
            ModelPath: "project://assets/models/Props/Barrel.glb",
            ColliderShape: PropColliderShape.Cylinder, ColliderRadius: 0.352f, ColliderHalfHeight: 0.2205f, ColliderCenter: new Vector3(0f, 0.5725f, 0f)),
        new("Trash Can", IsSphere: false, Size: 1f, Mass: 5f, Color: default,
            ModelPath: "project://assets/models/Props/TrashCan.glb",
            ColliderShape: PropColliderShape.Cylinder, ColliderRadius: 0.122f, ColliderHalfHeight: 0.092f, ColliderCenter: new Vector3(-0.005f, 0.214f, 0f)),

        // ── Furniture (3) - Kenney Furniture Kit, CC0. Box colliders fill the leg
        // gap (documented poor fit for Chair/Table; Bench is a solid slab so its box
        // tracks the silhouette closely) ─────────────────────────────────────────
        new("Chair", IsSphere: false, Size: 1f, Mass: 6f, Color: default,
            ModelPath: "project://assets/models/Props/Chair.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.1f, 0.235f, 0.1f), ColliderCenter: new Vector3(0.1f, 0.235f, -0.1f)),
        new("Table", IsSphere: false, Size: 1f, Mass: 12f, Color: default,
            ModelPath: "project://assets/models/Props/Table.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.4205f, 0.1635f, 0.2235f), ColliderCenter: new Vector3(0.4207f, 0.1634f, -0.2237f)),
        new("Bench", IsSphere: false, Size: 1f, Mass: 10f, Color: default,
            ModelPath: "project://assets/models/Props/Bench.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.2f, 0.235f, 0.1f), ColliderCenter: new Vector3(0.2f, 0.235f, -0.1f)),

        // ── Misc (1) ───────────────────────────────────────────────────────────
        new("Traffic Cone", IsSphere: false, Size: 1f, Mass: 1f, Color: default,
            ModelPath: "project://assets/models/Props/Cone.glb",
            ColliderShape: PropColliderShape.Capsule, ColliderRadius: 0.15f, ColliderHalfHeight: 0.075f, ColliderCenter: new Vector3(0f, 0.225f, 0f)),

        // ── Machinery (8, new) - Kenney Factory Kit, CC0. Large/Medium Cog are the
        // one entry pair in this whole catalogue NOT base-pivoted at y=0 (raw mesh
        // spans y:[-0.15,0.075]) - center.y is genuinely negative below, not the usual
        // half_height, confirmed from source bounds, not assumed symmetric with the
        // rest ──────────────────────────────────────────────────────────────────
        new("Large Cog", IsSphere: false, Size: 1f, Mass: 30f, Color: default,
            ModelPath: "project://assets/models/Props/CogLarge.glb",
            ColliderShape: PropColliderShape.Cylinder, ColliderRadius: 0.5f, ColliderHalfHeight: 0.1125f, ColliderCenter: new Vector3(0f, -0.0375f, 0f)),
        new("Medium Cog", IsSphere: false, Size: 1f, Mass: 20f, Color: default,
            ModelPath: "project://assets/models/Props/CogMedium.glb",
            ColliderShape: PropColliderShape.Cylinder, ColliderRadius: 0.5f, ColliderHalfHeight: 0.1125f, ColliderCenter: new Vector3(0f, -0.0375f, 0f)),
        new("Piston", IsSphere: false, Size: 1f, Mass: 35f, Color: default,
            ModelPath: "project://assets/models/Props/Piston.glb",
            ColliderShape: PropColliderShape.Cylinder, ColliderRadius: 0.5f, ColliderHalfHeight: 0.5f, ColliderCenter: new Vector3(0f, 0.5f, 0f)),
        new("Machine Block", IsSphere: false, Size: 1f, Mass: 60f, Color: default,
            ModelPath: "project://assets/models/Props/MachineBlock.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.6f, 0.65f, 0.75f), ColliderCenter: new Vector3(0f, 0.65f, 0f)),
        new("Hopper", IsSphere: false, Size: 1f, Mass: 15f, Color: default,
            ModelPath: "project://assets/models/Props/Hopper.glb",
            ColliderShape: PropColliderShape.Cylinder, ColliderRadius: 0.559f, ColliderHalfHeight: 0.5f, ColliderCenter: new Vector3(0f, 0.5f, 0f)),
        new("Pipe Segment", IsSphere: false, Size: 1f, Mass: 12f, Color: default,
            ModelPath: "project://assets/models/Props/PipeSegment.glb",
            ColliderShape: PropColliderShape.Cylinder, ColliderRadius: 0.5f, ColliderHalfHeight: 0.5f, ColliderCenter: new Vector3(0f, 0.5f, 0f)),
        new("Arrow Sign", IsSphere: false, Size: 1f, Mass: 3f, Color: default,
            ModelPath: "project://assets/models/Props/ArrowSign.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.283f, 0.1f, 0.4245f), ColliderCenter: new Vector3(0f, 0.1f, 0.0005f)),
        new("Warning Sign", IsSphere: false, Size: 1f, Mass: 8f, Color: default,
            ModelPath: "project://assets/models/Props/WarningSign.glb",
            ColliderShape: PropColliderShape.Cylinder, ColliderRadius: 0.2f, ColliderHalfHeight: 0.7755f, ColliderCenter: new Vector3(0f, 0.7755f, 0f)),

        // ── Construction (6, new) - self-authored, NOT downloaded; the "genuinely
        // unique" half of the 30. Exact-collider-by-construction, same as the
        // primitives, since these meshes were generated to these exact dimensions ──
        new("Steel Plate", IsSphere: false, Size: 1f, Mass: 15f, Color: default,
            ModelPath: "project://assets/models/Props/SteelPlate.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.5f, 0.025f, 0.5f), ColliderCenter: new Vector3(0f, 0.025f, 0f)),
        new("Beam", IsSphere: false, Size: 1f, Mass: 10f, Color: default,
            ModelPath: "project://assets/models/Props/Beam.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.05f, 0.05f, 0.75f), ColliderCenter: new Vector3(0f, 0.05f, 0f)),
        new("Wheel", IsSphere: false, Size: 1f, Mass: 12f, Color: default,
            ModelPath: "project://assets/models/Props/Wheel.glb",
            ColliderShape: PropColliderShape.Cylinder, ColliderRadius: 0.3f, ColliderHalfHeight: 0.09f, ColliderCenter: new Vector3(0f, 0.09f, 0f)),
        new("Hinge Plate", IsSphere: false, Size: 1f, Mass: 3f, Color: default,
            ModelPath: "project://assets/models/Props/HingePlate.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.2f, 0.02f, 0.2f), ColliderCenter: new Vector3(0f, 0.02f, 0f)),
        new("Ball Joint", IsSphere: false, Size: 1f, Mass: 5f, Color: default,
            ModelPath: "project://assets/models/Props/BallJoint.glb",
            ColliderShape: PropColliderShape.Sphere, ColliderRadius: 0.15f, ColliderCenter: new Vector3(0f, 0.15f, 0f)),
        new("Thruster Body", IsSphere: false, Size: 1f, Mass: 18f, Color: default,
            ModelPath: "project://assets/models/Props/ThrusterBody.glb",
            ColliderShape: PropColliderShape.Cylinder, ColliderRadius: 0.32f, ColliderHalfHeight: 0.2f, ColliderCenter: new Vector3(0f, 0.2f, 0f)),
        // ── Kenney import (native, via the AssetPacker `kenney` tool / MCP / Editor Kenney Browser) ──
        new("Machine Fortified", IsSphere: false, Size: 1f, Mass: 60.0000f, Color: default,
            ModelPath: "project://assets/models/Props/MachineFortified.glb",
            ColliderShape: PropColliderShape.Box, ColliderHalfExtents: new Vector3(0.6000f, 0.6752f, 0.8000f), ColliderCenter: new Vector3(0.0000f, 0.6752f, 0.0000f)),
    };

    public int MaxProps = 24;
    public float SpawnDistance = 2.5f;
    public float SpawnSpeed = 6.0f;

    private Entity _player;
    private Entity _pauseMenuEntity;
    private MeshHandle _cube;
    private MeshHandle _sphere;
    private readonly Queue<Entity> _spawned = new();
    private int _nextCatalogIndex;

    public override void OnAttach()
    {
        _player = Self.Parent;
        _cube = World.CreateMesh("cube");
        _sphere = World.CreateMesh("sphere");
        // Bug found while building: this used to hardcode Key.F unconditionally, so a
        // player who rebinds Spawn Prop in Settings has that saved to
        // SandboxSettings.SpawnPropKey, but the NEXT attach (every reconnect, every new
        // session - PropSpawner is added fresh per connection) silently re-registered
        // Key.F and undid the rebind.
        SandboxSettings.EnsureLoaded();
        InputActions.Register("spawn_prop", SandboxSettings.SpawnPropKey);
        _pauseMenuEntity = Scene.Find("NetSession");
    }

    public override void OnUpdate(float deltaTime)
    {
        // Owner-only, same as PhysicsGun: _player, not Self (the camera), because Self
        // carries no NetworkIdentity and would read Net.HasAuthority as unconditionally
        // true (see PhysicsGun.OnUpdate's own comment on ResolveOwnershipEntity). True
        // offline, so single-player is unaffected.
        if (!Net.HasAuthority(_player))
        {
            return;
        }

        // The menu suppresses gameplay input while it is up; its catalogue
        // buttons are the F key's replacement while browsing.
        if (GetScript<SpawnMenu>() is { IsOpen: true })
        {
            return;
        }
        if (_pauseMenuEntity.GetScript<UiPauseMenu>() is { IsOpen: true })
        {
            return;
        }

        if (InputActions.IsPressed("spawn_prop"))
        {
            SpawnProp(_nextCatalogIndex);
            _nextCatalogIndex = (_nextCatalogIndex + 1) % Catalog.Length;
        }
    }

    /// <summary>
    /// The one spawn path: create <see cref="Catalog"/>[<paramref name="catalogIndex"/>]
    /// a couple of metres in front of the camera. Shared by the F-key hotkey and every
    /// <see cref="SpawnMenu"/> button; recycles the oldest live prop past <see cref="MaxProps"/>
    /// so neither caller can grow the world unbounded.
    /// </summary>
    public void SpawnProp(int catalogIndex)
    {
        if (catalogIndex < 0 || catalogIndex >= Catalog.Length)
        {
            return;
        }
        PropDef def = Catalog[catalogIndex];

        // Drop anything already gone (destroyed some other way) so the cap
        // reflects props actually alive, not just ones we once created.
        // ponytail: only reclaims from the FRONT of the queue, so a prop that dies in
        // the middle (thrown off the map, deleted by another script) stays counted
        // against MaxProps until it ages to the front - fewer live props than the cap
        // allows, never more, so the failure direction is safe. Upgrade path: sweep the
        // whole queue each spawn, or keep a running count and reconcile against
        // World.IsValid lazily. Emitter.cs copies this exact pattern; fix both together.
        while (_spawned.Count > 0 && !World.IsValid(_spawned.Peek()))
        {
            _spawned.Dequeue();
        }

        if (_spawned.Count >= MaxProps)
        {
            Entity oldest = _spawned.Dequeue();
            if (World.IsValid(oldest))
            {
                oldest.Destroy();
            }
        }

        Vector3 forward = Camera.GetForward(Self);
        Vector3 spawnPos = Self.Position + forward * SpawnDistance;

        Entity prop = World.Create();
        prop.Name = $"Spawned {def.Name}";
        prop.AddTransform();
        prop.SetTransform(spawnPos, Vector3.Zero, def.ModelPath != null ? Vector3.One : new Vector3(def.Size, def.Size, def.Size));
        prop.MarkTransient(); // never pollute the saved scene
        // Grouped under one Hierarchy panel entry instead of flooding the scene root -
        // SetParent never moves a child's world position in this engine (confirmed by
        // reading ecs::InsertChildAt - see RuntimeContainers' own file comment), so this
        // is purely organizational and never fights the SetTransform call above it.
        prop.SetParent(RuntimeContainers.Get("Spawned Props"));

        if (def.ModelPath != null)
        {
            // Real glTF model, native scale - the catalogue's collider fields already
            // encode the model's own measured bounds, not a Size-derived guess.
            prop.LoadModel(def.ModelPath);
            switch (def.ColliderShape)
            {
                case PropColliderShape.Sphere:
                    Physics.AddSphereBody(prop, def.ColliderRadius, dynamic: true);
                    break;
                case PropColliderShape.Capsule:
                    Physics.AddCapsuleBody(prop, def.ColliderHalfHeight, def.ColliderRadius, dynamic: true);
                    break;
                case PropColliderShape.Cylinder:
                    Physics.AddCylinderBody(prop, def.ColliderHalfHeight, def.ColliderRadius, dynamic: true);
                    break;
                default:
                    Physics.AddBoxBody(prop, def.ColliderHalfExtents, dynamic: true);
                    break;
            }
            if (def.ColliderCenter != Vector3.Zero)
            {
                prop.Component("Collider").SetVector3("center", def.ColliderCenter);
            }
        }
        else if (def.IsSphere)
        {
            prop.AddMesh(_sphere);
            prop.SetMaterialColor(def.Color);
            Physics.AddSphereBody(prop, def.Size * 0.5f, dynamic: true);
        }
        else
        {
            prop.AddMesh(_cube);
            prop.SetMaterialColor(def.Color);
            Physics.AddBoxBody(prop, new Vector3(def.Size * 0.5f, def.Size * 0.5f, def.Size * 0.5f), dynamic: true);
        }

        // Explicit mass per catalogue entry, in place of the shape's auto-computed
        // default - this is what actually makes "Heavy Ball" heavier than "Marble"
        // rather than the two only differing in radius.
        prop.Component("Rigid Body").SetFloat("mass", def.Mass);

        Physics.SetLinearVelocity(prop, forward * SpawnSpeed);
        Tags.Add(prop, Tags.Create("grabbable"));
    }
}

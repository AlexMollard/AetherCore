using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Wiremod's third sink: spawns something into the world on a rising edge, rather than
/// driving an existing object's colour (Lamp) or pose (Door) - a wire signal creating
/// new state, not just steering it.
///
/// REUSES PropSpawner's queue pattern rather than reinventing a weaker one: a bounded
/// Queue&lt;Entity&gt;, oldest destroyed once MaxLive is hit, and World.IsValid (not
/// entity.IsValid - see WireHub's own file comment on why that distinction matters) to
/// notice an instance that died some OTHER way - picked up and thrown off the map,
/// destroyed by another script - before counting it against the cap. Cooldown is the
/// second half of the rate limit, independent of the cap: without it a Latch looped
/// back into its own Emitter (or just a very twitchy wire) would still spawn one
/// instance per Propagate() call with no minimum time between them.
///
/// SPAWNS STRAIGHT UP, NOT "FORWARD" - FOR NOW. Entity.Forward (Entity.cs/
/// WorldExports.cpp's aether_get_forward, sharing InteropCommon.hpp's ForwardOf with
/// Camera.GetForward's own non-orbit branch) now exists in the SDK - added alongside
/// this file - closing the engine gap that used to force this choice (there was no
/// generic entity forward-direction export, and hand-deriving one from
/// Self.EulerDegrees is exactly the sin/cos-yaw bug FirstPersonPlayer's own file
/// history already documents here). NOT WIRED IN YET, deliberately: AetherGame.csproj
/// is a shared assembly six workers build against, and every live Editor process has
/// its OWN already-loaded copy of AetherCore.dll that a running process can never swap
/// - only a fresh Editor launch after a full `cmake --build --preset default --target
/// Editor` picks up a new SDK member. Switching this call to Self.Forward while
/// another worker's Editor was live broke their Play (CS1061, their loaded copy
/// predates Entity.Forward) even though every build I ran locally was green - a
/// working straight-up launch beats a half-landed API dependency. Switch this to
/// Self.Forward once no live Editor is holding the staged copy and the full Editor
/// build has actually restaged it (see this repo's own file comments on why a green
/// dotnet build alone never proves that).
/// </summary>
public sealed class Emitter : EntityScript
{
    /// <summary>INPUT. A rising edge (crossing above 0.5) spawns one instance, subject
    /// to Cooldown/MaxLive - a level like Lamp/Door's Enable would spawn once per
    /// Propagate() call for as long as it stayed high, which is not what "emit" means;
    /// tracking the edge here makes wiring a level source (a Gate) into this behave the
    /// same as wiring a genuine pulse source (a Button).</summary>
    public float Enable;

    public float Size = 0.4f;
    public float Mass = 4.0f;
    public Vector3 Color = new(0.7f, 0.3f, 0.75f);

    /// <summary>Straight-up launch speed in units/second.</summary>
    public float LaunchSpeed = 4.0f;

    /// <summary>Minimum seconds between spawns, regardless of how fast Enable pulses -
    /// the rate-limit half of the cap.</summary>
    public float Cooldown = 0.5f;

    /// <summary>Live instances this Emitter allows at once before it recycles its own
    /// oldest - the count half of the cap, independent of Cooldown.</summary>
    public int MaxLive = 8;

    private readonly Queue<Entity> _spawned = new();
    private MeshHandle _sphere;
    private bool _wasEnabled;
    private float _cooldownRemaining;

    public override void OnAttach()
    {
        _sphere = World.CreateMesh("sphere");
    }

    public override void OnUpdate(float deltaTime)
    {
        if (_cooldownRemaining > 0.0f)
        {
            _cooldownRemaining -= deltaTime;
        }

        bool enabled = Enable > 0.5f;
        bool risingEdge = enabled && !_wasEnabled;
        _wasEnabled = enabled;

        if (risingEdge && _cooldownRemaining <= 0.0f)
        {
            Spawn();
            _cooldownRemaining = Cooldown;
        }
    }

    private void Spawn()
    {
        // Drop anything already gone (destroyed some other way) so the cap reflects
        // instances actually alive, not just ones this Emitter once created - same
        // liveness sweep as PropSpawner.SpawnProp.
        // ponytail: only reclaims from the FRONT of the queue, so an instance that dies
        // in the middle (thrown off the map, destroyed by another script) stays counted
        // against MaxLive until it ages to the front - fewer live instances than the
        // cap allows, never more, so the failure direction is safe. Upgrade path: sweep
        // the whole queue each spawn, or keep a running count and reconcile against
        // World.IsValid lazily. Left alone here to match PropSpawner's exact convention
        // rather than introduce a second one.
        while (_spawned.Count > 0 && !World.IsValid(_spawned.Peek()))
        {
            _spawned.Dequeue();
        }

        if (_spawned.Count >= MaxLive)
        {
            Entity oldest = _spawned.Dequeue();
            if (World.IsValid(oldest))
            {
                oldest.Destroy();
            }
        }

        Vector3 spawnPos = Self.Position + Vector3.UnitY * (Size + 0.1f);

        Entity instance = World.Create();
        instance.Name = "Emitted Ball";
        instance.AddTransform();
        instance.SetTransform(spawnPos, Vector3.Zero, new Vector3(Size, Size, Size));
        instance.MarkTransient(); // never pollute the saved scene
        // Grouped under one Hierarchy panel entry instead of flooding the scene root -
        // see RuntimeContainers' own file comment on why SetParent never moves this.
        instance.SetParent(RuntimeContainers.Get("Emitted Balls"));
        instance.AddMesh(_sphere);
        instance.SetMaterialColor(Color);
        Physics.AddSphereBody(instance, Size * 0.5f, dynamic: true);
        instance.Component("Rigid Body").SetFloat("mass", Mass);
        Physics.SetLinearVelocity(instance, Vector3.UnitY * LaunchSpeed);
        Tags.Add(instance, Tags.Create("grabbable"));

        _spawned.Enqueue(instance);
    }
}

// Probes for the 3D native-vs-browser gameplay parity fixtures (resources/web/reference/scenes/
// Browser*3D*.scene.toml). Every observation is a published public field, so the parity harness
// compares it at each checkpoint tick. projects/AgentPipelineRust/fixtures3d/gameplay3d.rs publishes
// the same properties with the same behaviour. These live outside scripts/ on purpose: the 2D
// production publisher scans every script under scripts/, and the project's 2D reference must keep
// publishing; tools/web-runtime/gameplay-parity-checks.py injects this file into its staged project.
using System;
using System.Numerics;
using AetherCore;

internal static class Probe3D
{
    public const string Human = "project://assets/models/Human/Human.gltf";
}

// Dynamic bodies driven by script velocity, force and impulse; hinge and fixed joints.
public sealed class Bodies3DProbe : EntityScript
{
    public int ticks;
    public Vector3 slider_set;
    public Vector3 slider_v30;
    public Vector3 pusher_v30;
    public Vector3 anchor_v10;
    public Vector3 anchor_v12;
    public Vector3 pendulum_v30;
    public Vector3 welded_v60;

    public override void OnUpdate(float deltaTime)
    {
        int tick = ticks++;
        Entity slider = Scene.Find("Slider");
        Entity pusher = Scene.Find("Pusher");
        Entity anchor = Scene.Find("Anchor box");
        switch (tick)
        {
            case 0:
                Physics.SetLinearVelocity(slider, new Vector3(3, 0, 0));
                slider_set = Physics.GetLinearVelocity(slider);
                break;
            case 10:
                anchor_v10 = Physics.GetLinearVelocity(anchor);
                Physics.AddImpulse(anchor, new Vector3(0, 30, 0));
                break;
            case 12:
                anchor_v12 = Physics.GetLinearVelocity(anchor);
                break;
            case 30:
                slider_v30 = Physics.GetLinearVelocity(slider);
                pusher_v30 = Physics.GetLinearVelocity(pusher);
                pendulum_v30 = Physics.GetLinearVelocity(Scene.Find("Pendulum"));
                break;
            case 60:
                welded_v60 = Physics.GetLinearVelocity(Scene.Find("Welded box"));
                break;
        }
        if (tick < 20) Physics.AddForce(pusher, new Vector3(100, 0, 0));
    }
}

// Script raycasts against static and dynamic bodies. Distance is -1 on a miss; *_ok says whether the
// hit entity (or the absence of one) is the expected one.
public sealed class Raycast3DProbe : EntityScript
{
    private const float MaxDistance = 20f;

    public int ticks;
    public float floor_distance = -2f;
    public Vector3 floor_point;
    public Vector3 floor_normal;
    public bool floor_ok;
    public float wall_distance = -2f;
    public Vector3 wall_point;
    public Vector3 wall_normal;
    public bool wall_ok;
    public float ball_distance = -2f;
    public Vector3 ball_point;
    public Vector3 ball_normal;
    public bool ball_ok;
    public float diamond_distance = -2f;
    public Vector3 diamond_point;
    public Vector3 diamond_normal;
    public bool diamond_ok;
    public float oblique_distance = -2f;
    public Vector3 oblique_point;
    public Vector3 oblique_normal;
    public bool oblique_ok;
    public float miss_distance = -2f;
    public bool miss_ok;
    public float dropper_distance = -2f;
    public Vector3 dropper_point;
    public Vector3 dropper_normal;
    public bool dropper_ok;
    public float rest_distance = -2f;
    public bool rest_ok;

    private static void Cast(Vector3 origin, Vector3 direction, string expect,
        out float distance, out Vector3 point, out Vector3 normal, out bool ok)
    {
        RaycastHit hit = Physics.Raycast(origin, direction, MaxDistance);
        if (hit.DidHit)
        {
            distance = hit.Fraction * MaxDistance;
            point = hit.Position;
            normal = hit.Normal;
            ok = expect.Length > 0 && hit.Entity == Scene.Find(expect);
        }
        else
        {
            distance = -1f;
            point = default;
            normal = default;
            ok = expect.Length == 0;
        }
    }

    public override void OnUpdate(float deltaTime)
    {
        int tick = ticks++;
        switch (tick)
        {
            case 1:
                Cast(new Vector3(2, 5, 2), new Vector3(0, -1, 0), "Floor",
                    out floor_distance, out floor_point, out floor_normal, out floor_ok);
                Cast(new Vector3(0, 1, 0), new Vector3(1, 0, 0), "Wall",
                    out wall_distance, out wall_point, out wall_normal, out wall_ok);
                Cast(new Vector3(0, 1.5f, 0.3f), new Vector3(-1, 0, 0), "Ball",
                    out ball_distance, out ball_point, out ball_normal, out ball_ok);
                Cast(new Vector3(0.5f, 1, 0), new Vector3(0, 0, -1), "Diamond",
                    out diamond_distance, out diamond_point, out diamond_normal, out diamond_ok);
                Cast(new Vector3(-3, 3, 0), new Vector3(0.70710677f, -0.70710677f, 0), "Floor",
                    out oblique_distance, out oblique_point, out oblique_normal, out oblique_ok);
                Cast(new Vector3(0, 10, 0), new Vector3(0, 1, 0), "",
                    out miss_distance, out _, out _, out miss_ok);
                break;
            case 31:
                Cast(new Vector3(8, 10, 0), new Vector3(0, -1, 0), "Dropper",
                    out dropper_distance, out dropper_point, out dropper_normal, out dropper_ok);
                break;
            case 119:
                Cast(new Vector3(8, 10, 0), new Vector3(0, -1, 0), "Dropper",
                    out rest_distance, out _, out _, out rest_ok);
                break;
        }
    }
}

// Counts the 3D contact events delivered to the entity carrying it.
public sealed class Contacts3DProbe : EntityScript
{
    public int ticks;
    public int collision_enter;
    public int collision_exit;
    public int trigger_enter;
    public int trigger_exit;
    public bool touched_floor;

    public override void OnUpdate(float deltaTime) => ticks++;

    public override void OnCollisionEnter(Entity other)
    {
        collision_enter++;
        touched_floor |= other == Scene.Find("Floor");
    }

    public override void OnCollisionExit(Entity other) => collision_exit++;
    public override void OnTriggerEnter(Entity other) => trigger_enter++;
    public override void OnTriggerExit(Entity other) => trigger_exit++;
}

// Character controller driven by arrow keys (move) and space (jump).
public sealed class Walker3DProbe : EntityScript
{
    public int ticks;
    public bool grounded;
    public int grounded_ticks;
    public int airborne_ticks;
    public int jumps;
    public float max_y = -100f;
    public bool space_was_down;

    public override void OnUpdate(float deltaTime)
    {
        ticks++;
        Vector3 direction = Vector3.Zero;
        if (Input.IsKeyDown(Key.Right)) direction.X += 1;
        if (Input.IsKeyDown(Key.Left)) direction.X -= 1;
        if (Input.IsKeyDown(Key.Up)) direction.Z -= 1;
        if (Input.IsKeyDown(Key.Down)) direction.Z += 1;
        CharacterController.Move(Self, direction, 3f);
        bool space = Input.IsKeyDown(Key.Space);
        if (space && !space_was_down)
        {
            CharacterController.Jump(Self, 5f);
            jumps++;
        }
        space_was_down = space;
        grounded = CharacterController.IsGrounded(Self);
        if (grounded) grounded_ticks++;
        else airborne_ticks++;
        max_y = MathF.Max(max_y, Self.Position.Y);
    }
}

// Builds a ragdoll on its entity at tick 0 and checks a second spawn is refused.
public sealed class Ragdoll3DProbe : EntityScript
{
    public int ticks;
    public bool spawned;
    public bool root_is_self;
    public bool respawn_refused;

    public override void OnUpdate(float deltaTime)
    {
        int tick = ticks++;
        if (tick != 0) return;
        Entity[] bones = Ragdoll.Spawn(Self, Probe3D.Human);
        spawned = bones.Length > 0;
        root_is_self = bones.Length > 0 && bones[0] == Self;
        respawn_refused = Ragdoll.Spawn(Self, Probe3D.Human).Length == 0;
    }
}

// Skinned clip switch, cross-fade and speed changes on the Human; publishes the live clip and time.
public sealed class Skin3DProbe : EntityScript
{
    public int ticks;
    public int ready_tick = -1;
    public int clip = -1;
    public float clip_time = -1f;
    public int walk_index = -1;

    public override void OnUpdate(float deltaTime)
    {
        int tick = ticks++;
        if (ready_tick < 0)
        {
            // The model's clips become available once its asynchronous load completes.
            if (Animation.ClipCount(Self) <= 0) return;
            Animation.SetClip(Self, 0);
            ready_tick = tick;
        }
        switch (tick - ready_tick)
        {
            case 10:
                walk_index = Animation.Find(Self, "Walk");
                Animation.SetClip(Self, walk_index);
                break;
            case 25:
                Animation.CrossFade(Self, Animation.Find(Self, "mixamo.com"), 0.5f);
                break;
            case 40:
                Animation.SetPlaybackSpeed(Self, 2f);
                break;
            case 70:
                Animation.SetPlaybackSpeed(Self, 0.5f);
                break;
        }
        clip = Animation.CurrentClip(Self);
        clip_time = Animation.GetTime(Self);
    }
}

// Instantiates 3D prefabs from script, places them, destroys one and instantiates another.
public sealed class Spawner3DProbe : EntityScript
{
    public int ticks;
    public int spawned_count;
    public int destroyed_count;
    public Entity first;
    public Entity second;
    public Entity third;

    private static Entity Spawn(string prefab, Vector3 position)
    {
        Entity spawned = Scene.Instantiate(prefab);
        if (!spawned.IsValid) throw new InvalidOperationException("Prefab failed to spawn: " + prefab);
        spawned.Position = position;
        return spawned;
    }

    public override void OnUpdate(float deltaTime)
    {
        ticks++;
        switch (ticks)
        {
            case 5:
                first = Spawn("Crate3D", new Vector3(-1, 4, 0));
                spawned_count++;
                break;
            case 15:
                second = Spawn("Barrel3D", new Vector3(1.5f, 6, 0));
                spawned_count++;
                break;
            case 25:
                first.Destroy();
                destroyed_count++;
                break;
            case 40:
                third = Spawn("Crate3D", new Vector3(0, 3, 0));
                spawned_count++;
                break;
        }
    }
}

// Lets a body fall for 20 ticks, saves a marker and loads the second 3D scene.
public sealed class Transition3DProbe : EntityScript
{
    public int ticks;

    public override void OnUpdate(float deltaTime)
    {
        ticks++;
        if (ticks != 20) return;
        SaveData.Write("transition3d_marker", $"left after {ticks} ticks");
        Scene.Load("BrowserTransition3DResult");
    }
}

// Runs in the destination scene: reads the pre-transition save and tracks its sphere.
public sealed class TransitionResult3DProbe : EntityScript
{
    public int ticks;
    public string marker = "";
    public float arrival_y = -100f;

    public override void OnAttach()
    {
        if (!SaveData.TryRead("transition3d_marker", out marker))
            throw new InvalidOperationException("Transition marker is unavailable.");
    }

    public override void OnUpdate(float deltaTime)
    {
        ticks++;
        arrival_y = Self.Position.Y;
    }
}

// Packaged VFS probe: results are published for the native/browser checkpoint harness.
public sealed class PackagedGlobProbe : EntityScript
{
    public string simple = "";
    public string recursive = "";
    public int missing;
    public int invalid;
    public int traversal;
    public bool engine;
    public bool package_read_only;

    public override void OnAttach()
    {
        simple = string.Join("|", Assets.List("project://assets/glob-probe/*.txt"));
        recursive = string.Join("|", Assets.List("project://assets/glob-probe/**/*.txt"));
        missing = Assets.List("project://assets/glob-probe/*.missing").Length;
        invalid = Assets.List("invalid://assets/glob-probe/*.txt").Length;
        traversal = Assets.List("project://assets/glob-probe/../*.txt").Length;
        engine = Assets.List("engine://**").Length > 0;
        package_read_only = !Assets.WriteText("project://assets/glob-probe/root.txt", "changed");
    }
}

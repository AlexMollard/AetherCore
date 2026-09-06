using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The visible humanoid mesh every player entity gets, attached UNCONDITIONALLY -
/// unlike <see cref="FirstPersonPlayer"/>/<see cref="PhysicsGun"/>/<see cref="SpawnMenu"/>/
/// <see cref="PropSpawner"/>, this is not gated on ownership. <see cref="NetPlayerRig"/>'s
/// own OnAttach adds it to every networked player copy (owner and remote alike, see that
/// class's own file comment on being "the one thing... NOT conditional on ownership"), and
/// <see cref="FirstPersonPlayer"/>'s OnAttach adds it for the scene-placed solo 'Player'
/// entity too (which carries no NetPlayerRig at all - see that entity's own scripts list in
/// Sandbox.scene.toml). Both call sites guard on <c>GetScript&lt;PlayerBody&gt;() == null</c>
/// first, so whichever of the two runs first on a given entity wins and the other is a no-op.
///
/// ASSET: <see cref="ModelPath"/> is the same rigged humanoid <see cref="Ragdoll.Spawn"/>
/// already reads for its bind pose (project://assets/models/Human/Human.gltf) - confirmed by
/// reading the glTF's own JSON directly, not assumed: skin 0/1 share one 65-joint mixamorig
/// hierarchy, and it ships two primitives (root child 0 = "Beta_Surface", the skinned body;
/// child 1 = "Beta_Joints", a separate skinned mesh of ball-joint overlays at every hinge) -
/// both spawned as children of a fresh "Body" entity by <see cref="Entity.LoadModel"/>, which
/// is why this script drives BOTH mesh entities' clips together rather than one.
///
/// CLIPS: the source glTF originally shipped exactly one baked clip, Mixamo's own unrenamed
/// take name "mixamo.com" (idle - it does move a little, but returns to itself in a loop, per
/// a frame-by-frame read of the Hips translation channel). A second clip, "Walk", has since
/// been grafted in from this project's HumanDemo sibling (same rig, byte-identical shared
/// buffer.bin, additional walk.bin holding just the new keyframes) - see this project's own
/// Human.gltf for the merged JSON. THERE IS NO RUN CYCLE in this asset; sprinting still plays
/// "Walk" (see <see cref="ApplyClip"/>) rather than faking a run by time-scaling it, which
/// would visibly slide the feet. That is a real sourcing gap, not something this script papers
/// over.
///
/// BAKE CAVEAT, findable only by reading the runtime loader end to end: script-driven
/// <see cref="Entity.LoadModel"/> calls AssetManager::LoadModel, which goes straight to
/// GltfAsset::LoadFromVfsPath - and THAT never parses raw .gltf/.glb at all, it requires an
/// already-baked companion .mesh/.skel/.animset next to it (EnsureModelBaked in the editor's
/// own ModelBake.cpp is the only thing that produces those, via drag-drop/Inspector re-import/
/// Publish - the editor was off-limits this session). Sandbox's existing bake predates the
/// "Walk" clip addition above, so <see cref="Animation.Find"/> will not find it until someone
/// with editor access re-imports this model once. Until then this script logs that specific
/// gap once and simply never leaves the idle clip - it does not crash or spin.
///
/// FACING: a player entity's own rotation is otherwise never written (see
/// <see cref="FirstPersonPlayer"/>'s own file comment on why), so nothing turns this mesh to
/// face anywhere unless this script does it - every frame, by an explicit absolute
/// <see cref="Entity.EulerDegrees"/> write on the body root (the SAME technique
/// <see cref="FirstPersonPlayer"/> already uses for the camera, for the same reason: a world
/// SetWorldTransform write, not parent-local composition, so it costs nothing extra whether
/// the physics-driven parent delta already dragged this entity along or not). Two sources,
/// in preference order:
/// <list type="number">
/// <item>An owner's actual look yaw, fed in every active frame by
/// <see cref="FirstPersonPlayer.ApplyMovement"/> via <see cref="SetLocomotionState"/> - once
/// that has been called even once, this script trusts it permanently (an owner calls it every
/// frame it is not menu-gated, so "permanently" in practice means "for this entity's entire
/// life", and freezing the last known yaw for the few frames a menu is open is imperceptible).
/// This is the only signal an observer looking down at their OWN legs should ever see: it
/// matches the camera exactly, so the body always reads as facing "forward" from your own
/// point of view, at the cost of legs that do not individually turn to face a sideways strafe -
/// an accepted simplification given this asset has only one non-idle clip to show regardless
/// of strafe direction.</item>
/// <item>Otherwise (every remote peer's replicated copy - nobody local ever drives its input,
/// so nothing ever calls SetLocomotionState on it), this script derives a facing direction
/// from the entity's OWN world-position delta between frames - the only signal available
/// without adding rotation replication to NetworkTransform, which is a real networking feature,
/// not a script-layer fix. The yaw-from-direction formula is not hand-derived: it is copied
/// from the engine's own aether_camera_get_yaw (CameraExports.cpp) - atan2(-dir.x, -dir.z) -
/// so a body that ever does get a real look-yaw fed in later lines up with this fallback by
/// construction instead of by two independently-guessed conventions agreeing.</item>
/// </list>
/// </summary>
public sealed class PlayerBody : EntityScript
{
    public string ModelPath = "project://assets/models/Human/Human.gltf";

    /// <summary>Uniform scale applied to the spawned "Body" root after LoadModel.
    /// REQUIRED, not cosmetic: Entity.LoadModel calls AssetManager::SpawnModel with its
    /// scale parameter defaulting to 1.0 - there is no way to pass a different one
    /// through the script API (checked Entity.cs and aether_load_model directly). This
    /// rig's raw glTF units are centimetres (its Hips bone sits at translation.y ~104
    /// in the raw buffer - confirmed by reading the accessor bytes - which only reads
    /// as a real adult hip height once scaled by 0.01 to metres). HumanDemo's own scene
    /// only looks right because its mesh entities were hand-scaled 0.01 through the
    /// editor's add-to-scene flow, a path this script never goes through - confirmed
    /// live: without this, the body rendered roughly 100x too large.</summary>
    public float ModelScale = 0.01f;

    /// <summary>Extra yaw added on top of the computed facing (owner camera yaw or the
    /// position-delta fallback) to correct for whichever way this rig's own bind pose
    /// happens to face - Beta_Surface/Beta_Joints/FBX_Root all carry an identity
    /// rotation in the source glTF (checked directly), so nothing said which axis
    /// "forward" was for this specific asset until it was actually seen on screen.
    /// 180 confirmed correct by live feedback: the body faced directly away from the
    /// camera at 0.</summary>
    public float ModelForwardOffsetDegrees = 180.0f;

    public const string IdleClipName = "mixamo.com";
    public const string WalkClipName = "Walk";

    /// <summary>Horizontal speed, in m/s, above which the walk clip plays instead of
    /// idle. Below <see cref="FirstPersonPlayer.WalkSpeed"/> so an owner's own gentle
    /// acceleration out of a standstill crosses it well before reaching a "walk", not
    /// right at top speed.</summary>
    public float WalkSpeedThreshold = 0.3f;

    private Entity _bodyRoot;
    private Entity _mesh0;
    private Entity _mesh1;
    private int _idleClip0 = -1;
    private int _walkClip0 = -1;
    private int _idleClip1 = -1;
    private int _walkClip1 = -1;
    private bool _clipsUsable;
    private bool _isWalking;

    private bool _hasExternalState;
    private float _externalYawDegrees;
    private float _externalSpeed;

    private Vector3 _lastPosition;
    private bool _hasLastPosition;
    private float _fallbackFacingYawDegrees;

    public override void OnAttach()
    {
        _bodyRoot = World.Create();
        _bodyRoot.Name = "Body";
        _bodyRoot.AddTransform();
        // Seed the correct world position BEFORE LoadModel: it reads this entity's
        // CURRENT transform once to place the mesh children, so an identity-transform
        // "Body" would spawn its meshes at the world origin instead of at the player.
        _bodyRoot.Position = Self.Position;
        _bodyRoot.SetParent(Self);
        _bodyRoot.LoadModel(ModelPath);
        _bodyRoot.Scale = new Vector3(ModelScale, ModelScale, ModelScale);

        if (_bodyRoot.ChildCount < 2)
        {
            Log.Warn($"[Sandbox] PlayerBody ({Self.Name}): '{ModelPath}' spawned {_bodyRoot.ChildCount} mesh child(ren), expected 2 (Human.gltf's body + joint-overlay primitives) - no visible player model this run.");
            return;
        }

        _mesh0 = _bodyRoot.GetChild(0);
        _mesh1 = _bodyRoot.GetChild(1);

        _idleClip0 = Animation.Find(_mesh0, IdleClipName);
        _idleClip1 = Animation.Find(_mesh1, IdleClipName);
        _walkClip0 = Animation.Find(_mesh0, WalkClipName);
        _walkClip1 = Animation.Find(_mesh1, WalkClipName);

        _clipsUsable = _idleClip0 >= 0 && _idleClip1 >= 0;
        if (!_clipsUsable)
        {
            Log.Warn($"[Sandbox] PlayerBody ({Self.Name}): idle clip '{IdleClipName}' not found on '{ModelPath}' - the model will render in its default bind pose with no locomotion animation.");
        }
        else if (_walkClip0 < 0 || _walkClip1 < 0)
        {
            // The expected state right after grafting "Walk" into the source glTF but
            // before anyone with editor access re-imports it - see this class's own
            // BAKE CAVEAT. Not a code bug: Animation.Find reads the baked .animset, and
            // the stale one predates the new clip.
            Log.Warn($"[Sandbox] PlayerBody ({Self.Name}): walk clip '{WalkClipName}' not found on '{ModelPath}' - it exists in the source glTF but Sandbox's baked .mesh/.animset predates it (Entity.LoadModel never parses raw glTF, only its baked cache). Re-import this model once (Inspector re-import, or drag it into the Hierarchy again) to pick it up; until then this body stays on idle regardless of movement.");
        }

        ApplyClip(walking: false);
    }

    public override void OnUpdate(float deltaTime)
    {
        if (!_bodyRoot.IsValid || deltaTime <= 0.0f)
        {
            return;
        }

        Vector3 position = Self.Position;
        float fallbackSpeed = 0.0f;
        if (_hasLastPosition)
        {
            Vector3 delta = position - _lastPosition;
            delta.Y = 0.0f;
            float distance = delta.Length();
            fallbackSpeed = distance / deltaTime;
            if (distance > 1e-4f)
            {
                Vector3 dir = delta / distance;
                // Same convention as aether_camera_get_yaw (CameraExports.cpp) - see this
                // class's own file comment on why this is copied, not re-derived.
                _fallbackFacingYawDegrees = MathF.Atan2(-dir.X, -dir.Z) * (180.0f / MathF.PI);
            }
        }
        _lastPosition = position;
        _hasLastPosition = true;

        float speed = _hasExternalState ? _externalSpeed : fallbackSpeed;
        float yaw = _hasExternalState ? _externalYawDegrees : _fallbackFacingYawDegrees;

        bool walking = speed > WalkSpeedThreshold;
        if (walking != _isWalking)
        {
            _isWalking = walking;
            ApplyClip(walking);
        }

        if (_bodyRoot.IsValid)
        {
            _bodyRoot.EulerDegrees = new Vector3(0.0f, yaw + ModelForwardOffsetDegrees, 0.0f);
        }
    }

    /// <summary>Fed by <see cref="FirstPersonPlayer.ApplyMovement"/>, owner-only, every
    /// active frame - see this class's own file comment on why an owner's real camera
    /// yaw and character-controller speed beat this script's own position-delta guess.
    /// </summary>
    public void SetLocomotionState(float yawDegrees, float speed)
    {
        _hasExternalState = true;
        _externalYawDegrees = yawDegrees;
        _externalSpeed = speed;
    }

    private void ApplyClip(bool walking)
    {
        if (!_clipsUsable)
        {
            return;
        }
        // No run cycle exists in this asset (see this class's own file comment) - a
        // sprinting owner still gets the walk clip rather than a time-scaled fake.
        int clip0 = walking && _walkClip0 >= 0 ? _walkClip0 : _idleClip0;
        int clip1 = walking && _walkClip1 >= 0 ? _walkClip1 : _idleClip1;
        Animation.SetClip(_mesh0, clip0);
        Animation.SetClip(_mesh1, clip1);
    }
}

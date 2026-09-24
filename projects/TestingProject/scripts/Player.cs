using System.Numerics;
using AetherCore;
using AetherGame.Systems;

namespace AetherGame;

/// <summary>
/// Player controller. The rig (mesh children, transform) is authored scene
/// content; this script tops up what the scene can't store - the external walk
/// clip and input bindings - drives movement each tick, and steers the scene's
/// main camera to follow.
///
/// It resolves the camera at runtime via <see cref="Camera.Main"/> (the scene's
/// authored main camera) rather than a wired reference, and tags itself "player"
/// so the GameManager can find it without any inspector plumbing.
/// </summary>
public sealed class Player : EntityScript
{
    // Inspector-exposed tunables (persist per-entity in the scene).
    public float WalkSpeed = 9.0f;
    public float SprintSpeed = 15.0f;

    private Entity _cam;
    private int _idleClip;
    private int _walkClip = -1;
    // AetherCore.CharacterController (SDK static helper) makes the bare name ambiguous
    // under `using AetherCore;` - qualify the project's component explicitly.
    private readonly AetherGame.Systems.CharacterController _controller = new();

    public override void OnAttach()
    {
        Tags.Add(Self, Tags.Create("player"));

        Animation.SetPlaybackSpeed(Self, 1.0f);

        // The walk clip lives in a separate .anim file the scene can't author -
        // append it once per session (clip 0 is the model idle).
        if (Animation.ClipCount(Self) < 2)
        {
            int walk = Animation.AddClip(Self, "project://assets/animations/Walking/Walking_mixamo.com.anim", lockRoot: true);
            if (walk >= 0)
            {
                Animation.Compile(Self);
            }
        }

        int clipCount = Animation.ClipCount(Self);
        _idleClip = 0;
        _walkClip = clipCount > 1 ? clipCount - 1 : -1;

        // Follow the scene's main camera. If the scene didn't author one, spawn a
        // third-person orbit camera so the game is still playable.
        _cam = Camera.Main;
        if (!_cam.IsValid)
        {
            _cam = Camera.CreateOrbit(new Vector3(0.0f, 6.0f, 13.0f), new Vector3(0.0f, 2.5f, 0.0f), 60.0f);
            Camera.SetMain(_cam);
        }

        InputActions.RegisterDefaults();
        InputActions.Register("sprint_key", Key.LeftShift);
    }

    public override void OnUpdate(float dt)
    {
        // Keep the controller tunables live so inspector edits apply immediately.
        _controller.WalkSpeed = WalkSpeed;
        _controller.SprintSpeed = SprintSpeed;

        float inputX = 0.0f;
        float inputZ = 0.0f;
        if (InputActions.IsDown("move_forward")) { inputZ += 1.0f; }
        if (InputActions.IsDown("move_backward")) { inputZ -= 1.0f; }
        if (InputActions.IsDown("move_left")) { inputX -= 1.0f; }
        if (InputActions.IsDown("move_right")) { inputX += 1.0f; }

        bool isSprinting = InputActions.IsDown("sprint_key") && inputZ > 0.0f;
        _controller.Update(Self, dt, _idleClip, _walkClip, inputX, inputZ, _cam, isSprinting, runClip: -1);

        // Third-person follow: keep the camera framed on the player.
        if (_cam.IsValid)
        {
            Camera.SetTarget(_cam, Self.Position);
        }
    }
}

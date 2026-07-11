using System.Numerics;
using AetherCore;
using AetherGame.Systems;

namespace AetherGame;

/// <summary>
/// Player controller. The rig (mesh children, transform) is authored scene
/// content; this script tops up what the scene can't store - the external walk
/// clip, the orbit camera, and input bindings - then drives movement each tick.
///
/// The single-camera-per-session value is a static field; movement state lives
/// on the CharacterController instance.
/// </summary>
public sealed class Player : EntityScript
{
    // Inspector-exposed tunables (persist per-entity in the scene).
    public float WalkSpeed = 10.0f;
    public float SprintSpeed = 15.0f;

    // One orbit camera per session (matches das g_cam; reset on assembly reload).
    private static CameraId s_camera;

    private int _idleClip;
    private int _walkClip = -1;
    private readonly CharacterController _controller = new();

    public override void OnAttach()
    {
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

        if (s_camera.Value == 0)
        {
            // Third-person: sit ~13 units back and ~6 up, looking at the rig's
            // upper body (was 0,30,50 - far too zoomed out).
            s_camera = Camera.CreateOrbit(new Vector3(0.0f, 6.0f, 13.0f), new Vector3(0.0f, 2.5f, 0.0f), 60.0f);
        }
        Camera.SetMain(s_camera);

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
        _controller.Update(Self, dt, _idleClip, _walkClip, inputX, inputZ, s_camera, isSprinting, runClip: -1);
        ThirdPersonCamera.Update(s_camera, Self);

        if (++_dbgFrame % 60 == 0)
        {
            Log.Info($"[playerdbg] f={_dbgFrame} pos={Self.Position} inZ={inputZ} wDown={Input.IsKeyDown(Key.W)} fwdAction={InputActions.IsDown("move_forward")}");
        }
    }

    private int _dbgFrame;
}

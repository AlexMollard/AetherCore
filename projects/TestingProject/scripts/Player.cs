using System.Numerics;
using AetherCore;
using AetherGame.Systems;

namespace AetherGame;

/// <summary>
/// Player controller. The rig (mesh children, transform) is authored scene
/// content; this script tops up what the scene can't store - the external walk
/// clip and input bindings - drives movement each tick, steers the referenced
/// camera to follow, and writes a live HUD label.
///
/// Everything the script touches on OTHER entities is a typed component
/// reference, assigned in the inspector by dropping an entity that carries the
/// matching component (drop-validated). Access flows through the wrapper's
/// members, e.g. <c>Cam.SetTarget(...)</c>, <c>Hud.Text = ...</c>.
/// </summary>
public sealed class Player : EntityScript
{
    // Inspector-exposed tunables (persist per-entity in the scene).
    public float WalkSpeed = 10.0f;
    public float SprintSpeed = 15.0f;

    // Typed component references. The scene wires these to the authored Camera
    // entity, this player's own Rigid Body, and a Canvas text element; each slot
    // only accepts an entity that has the matching component.
    public CameraRef Cam;      // third-person camera this player drives
    public RigidBodyRef Body;  // this player's physics body (read its motion)
    public UiTextRef Hud;      // on-screen label this script updates live

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

        if (!Cam.IsValid)
        {
            // No camera linked in the inspector - spawn a third-person one so the
            // game is still playable (sits ~13 back / ~6 up, framing the rig).
            Cam = new CameraRef(Camera.CreateOrbit(new Vector3(0.0f, 6.0f, 13.0f), new Vector3(0.0f, 2.5f, 0.0f), 60.0f));
        }
        Cam.MakeMain();

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
        _controller.Update(Self, dt, _idleClip, _walkClip, inputX, inputZ, Cam.Owner, isSprinting, runClip: -1);

        // Third-person follow: the camera (a component reference) tracks the player.
        if (Cam.IsValid)
        {
            Cam.SetTarget(Self.Position);
        }

        // Drive an in-game UI label straight from the script through a component
        // reference. Reads the body's speed through another reference. The label
        // updates on screen every frame as the player moves.
        if (Hud.IsValid)
        {
            Vector3 p = Self.Position;
            float speed = Body.IsValid ? Body.LinearVelocity.Length() : 0.0f;
            Hud.Text = $"pos ({p.X:0.0}, {p.Z:0.0})   speed {speed:0.0}";
        }
    }
}

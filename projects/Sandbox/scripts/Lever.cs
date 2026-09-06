using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Wiremod's toggle source: the piece Button cannot be, on purpose - Button.Pressed is
/// a one-frame pulse (see its own file comment), so "wire a switch that stays on" always
/// needed a Latch downstream of it, costing an extra entity and wire for the single most
/// obvious thing anyone builds first. Lever.Interact() instead flips a PERSISTENT on/off
/// state, and Out is a level (1.0f/0.0f) for as long as that state holds - wiring it
/// straight into a Door's Enable holds the door open with nothing else in between.
///
/// Follows Button.cs's own conventions exactly, because this is the same shape of
/// device with a different memory rule: no idea a wire, or WireHub, exists (see WireHub's
/// own file comment on why device scripts stay ignorant of the graph on top of them);
/// Hub is resolved lazily through a property rather than in OnAttach, for the identical
/// WireHub-attach-order race Button.cs's own file comment documents; Interact() pushes
/// (calls Propagate() itself) rather than only waiting on WireHub's next OnUpdate.
///
/// VISIBLE STATE: tints its own material on flip - the same Material.SetEmissive idiom
/// ToolGun's pending/reject tints and PhysicsGun's held-prop tint already use, so a
/// lever's position is never a guess and this is not a second visual convention for
/// "this entity's state just changed." Assumes emissive black as the authored baseline
/// (true for every interactable in this sandbox), the same assumption PhysicsGun's own
/// HeldEmissiveTint documents.
///
/// HANDLE MOTION: lever-single.glb ships three baked clips ("toggle-on"/"toggle-off"/
/// "toggle"). Interact() plays whichever of the first two matches the state it just
/// entered via the ordinary <see cref="Animation"/> API - no hand-rolled transform lerp
/// on the handle, which would silently diverge from the authored keyframes and be a
/// second animation convention beside the engine's actual animation system. Both clip
/// lookups are cached once: <see cref="Animation.Find"/> is a named lookup and the clip
/// set is fixed for this entity's mesh, and -1 (not found) is an expected, non-error case
/// for a Lever whose model has not been placed with animation yet - PlayToggleClip below
/// checks for it every time instead of assuming the lookup succeeded.
///
/// HOLD AT END, DON'T LOOP: "Skinned Mesh" has no dedicated looping setter on <see
/// cref="Animation"/>, so it is set through the generic reflected-component path (AE_COMPONENT
/// in CoreComponents.reflect.cpp, its "looping" bool field) instead - but that path is NOT
/// interchangeable with <see cref="Animation"/>'s own exports here. <see cref="Animation.SetClip"/>
/// walks Self AND every child with an animator natively (AnimationExports.cpp's
/// ForEachSpawnedSmc) because a SKINNED model - even a single-primitive one - always spawns as
/// a root plus one child per primitive (ModelSpawn.cpp: the single-entity shortcut is gated on
/// `skinIndex &lt; 0`, so it never applies here); "Skinned Mesh" itself lives on that child, not
/// the root a script is attached to. The generic component reflection <see cref="Component"/>
/// resolves only the exact entity id it is given - no child fallback - so setting "looping" has
/// to walk Self plus its children itself rather than assuming Self carries it, unlike Lamp's
/// "Point Light" or Door's "Rigid Body", which really do live on Self.
/// </summary>
public sealed class Lever : EntityScript
{
    /// <summary>OUTPUT. A level, not a pulse: 1.0 while on, 0.0 while off - unlike
    /// Button.Pressed, this never decays on its own.</summary>
    public float Out;

    public Vector3 OnTint = new(0.2f, 0.85f, 0.35f);

    private WireHub? _hub;
    private bool _on;
    private int _clipOn = -1;
    private int _clipOff = -1;

    /// <summary>Resolved lazily for the same reason Button.Hub is - see Button.cs's own
    /// field comment.</summary>
    private WireHub? Hub => _hub ??= Scene.Find("WireHub").GetScript<WireHub>();

    public override void OnAttach()
    {
        _clipOn = Animation.Find(Self, "toggle-on");
        _clipOff = Animation.Find(Self, "toggle-off");
        ApplyTint();
    }

    public void Interact()
    {
        _on = !_on;
        Out = _on ? 1.0f : 0.0f;
        ApplyTint();
        PlayToggleClip();
        // Push, not just poll: propagate right now rather than waiting for WireHub's own
        // OnUpdate to happen to run this frame (see WireHub's own file comment on why).
        Hub?.Propagate();
    }

    private void PlayToggleClip()
    {
        int clip = _on ? _clipOn : _clipOff;
        if (clip < 0)
        {
            return;
        }
        Animation.SetClip(Self, clip);
        StopLooping(Self);
        for (int i = 0; i < Self.ChildCount; i++)
        {
            StopLooping(Self.GetChild(i));
        }
    }

    private static void StopLooping(Entity entity)
    {
        ComponentAccess skinnedMesh = entity.Component("Skinned Mesh");
        if (skinnedMesh.Exists)
        {
            skinnedMesh.SetBool("looping", false);
        }
    }

    private void ApplyTint() => Self.Material.SetEmissive(_on ? OnTint : Vector3.Zero);
}

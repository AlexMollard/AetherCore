using AetherCore;

namespace AetherGame;

/// <summary>
/// Networking glue for the Whisper player prefab. Two jobs:
/// <list type="bullet">
/// <item>Pushes the inspector-tunable correction feel into this entity's
/// NetworkTransform component on attach, so it can be tuned without a
/// rebuild.</item>
/// <item>Replicates which animation clip is playing, so remote copies of a
/// player animate instead of sliding around in whatever pose they spawned
/// in.</item>
/// </list>
/// </summary>
/// <remarks>
/// <para>
/// Split from <see cref="PlayerController"/> on purpose. PlayerController owns
/// DECIDING the animation - it already has the grounded/movement state that
/// drives the decision, exposed read-only as <see cref="PlayerController.AnimIndex"/>.
/// This script owns REPLICATING that decision and APPLYING it to the sprite.
/// Keeping the apply side in exactly one place means the owner's own sprite and
/// every remote copy's sprite go through the identical "only switch when it
/// actually changed" guard, instead of two scripts independently recomputing the
/// same clip and risking one restarting it a frame out of step with the other.
/// </para>
/// <para>
/// PlayerController.OnUpdate returns immediately for anything this peer does not
/// own (<c>!Net.IsOwner(Self)</c>), so on a remote copy AnimIndex is never
/// updated locally - the whole point of AnimState existing is to carry the
/// owner's decision across the wire so this script can apply it anyway.
/// </para>
/// </remarks>
public sealed class NetPlayerSync : EntityScript
{
    /// <summary>How fast the locally-owned entity's predicted position eases
    /// toward the host's authoritative one. Pushed into this entity's
    /// NetworkTransform.correctionRate on attach - see NetComponents.hpp for the
    /// full field docs.</summary>
    public float CorrectionRate = 12.0f;

    /// <summary>Position error beyond which correction snaps instead of easing,
    /// so a big desync (e.g. after a stall) cuts to the right place instead of
    /// visibly sliding back to it. Pushed into NetworkTransform.snapDistance on
    /// attach.</summary>
    public float SnapDistance = 4.0f;

    /// <summary>
    /// Which clip is playing, host-authoritative and replicated to every client -
    /// one of <see cref="PlayerController.AnimIndexIdle"/>,
    /// <see cref="PlayerController.AnimIndexRun"/> or
    /// <see cref="PlayerController.AnimIndexJump"/>. The owner writes this every
    /// frame from its own PlayerController; every peer, including the owner,
    /// drives the sprite from this value.
    /// </summary>
    [Replicated] public int AnimState;

    // Guards SpriteAnimator.SetAnimation the same way PlayerController's old
    // per-instance `_anim` field did: re-issuing the same clip every frame
    // restarts it from frame 0, which reads as a stutter.
    private int _appliedAnim = -1;

    public override void OnAttach()
    {
        Net.SetTransformTuning(Self, CorrectionRate, SnapDistance);
        // Apply whatever AnimState already holds (idle, by default) immediately
        // rather than waiting a frame for OnUpdate.
        ApplyAnim(AnimState);
    }

    public override void OnUpdate(float deltaTime)
    {
        // Looked up per frame rather than cached in OnAttach: scripts on an entity
        // attach one at a time in list order, so a sibling is only guaranteed to be
        // live from OnUpdate onward (see Entity.GetScript).
        // HasAuthority, not IsOwner: the host now simulates every player from the
        // input its owner submits, so the host is where a CLIENT's animation is
        // decided too. Gated on ownership, a client's AnimState was written nowhere
        // the host could replicate it, and every remote copy of that player animated
        // from a value that never left idle.
        if (Net.HasAuthority(Self) && GetScript<PlayerController>() is { } controller)
        {
            AnimState = controller.AnimIndex;
        }
        ApplyAnim(AnimState);
    }

    private void ApplyAnim(int index)
    {
        if (index == _appliedAnim)
        {
            return;
        }
        _appliedAnim = index;
        SpriteAnimator.SetAnimation(Self, ClipFor(index));
        SpriteAnimator.Play(Self);
    }

    private static string ClipFor(int index) => index switch
    {
        PlayerController.AnimIndexRun => PlayerController.AnimRun,
        PlayerController.AnimIndexJump => PlayerController.AnimJump,
        _ => PlayerController.AnimIdle,
    };
}

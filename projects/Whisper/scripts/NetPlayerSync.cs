using AetherCore;

namespace AetherGame;

/// <summary>
/// Networking glue for the Whisper player prefab: replicates the presentation
/// decisions <see cref="PlayerController"/> makes - which animation clip is
/// playing and which way the character faces - so remote copies of a player
/// animate and face correctly instead of sliding around in whatever pose they
/// spawned in.
/// </summary>
/// <remarks>
/// <para>
/// Split from <see cref="PlayerController"/> on purpose. PlayerController owns
/// DECIDING these - it already has the grounded/movement state that drives both,
/// exposed read-only as <see cref="PlayerController.AnimIndex"/> and
/// <see cref="PlayerController.FacingLeft"/>. This script owns REPLICATING those
/// decisions and APPLYING them to the sprite. Keeping the apply side in exactly one
/// place means the owner's own sprite and every remote copy's sprite go through the
/// identical path, instead of two scripts independently recomputing the same thing
/// and risking one landing a frame out of step with the other.
/// </para>
/// <para>
/// PlayerController.OnUpdate returns immediately for anything this peer does not own
/// (<c>!Net.HasAuthority(Self)</c>), so on a remote copy neither value is ever
/// updated locally - carrying the owner's decision across the wire so this script
/// can apply it anyway is the whole point of the replicated fields below.
/// </para>
/// </remarks>
public sealed class NetPlayerSync : EntityScript
{
    /// <summary>
    /// Which clip is playing - one of <see cref="PlayerController.AnimIndexIdle"/>,
    /// <see cref="PlayerController.AnimIndexRun"/> or
    /// <see cref="PlayerController.AnimIndexJump"/>. The owner writes it every frame
    /// from its own PlayerController and the framework replicates it; every peer,
    /// including the owner, drives the sprite from this value.
    /// </summary>
    [Replicated] public int AnimState;

    /// <summary>
    /// Whether the character faces left. Replicated exactly as
    /// <see cref="AnimState"/> is, and deliberately through the same mechanism
    /// rather than a second one: both are presentation decisions the owner makes and
    /// every peer must reproduce, and a facing that travelled some other way would be
    /// one more thing that can fall out of step with the animation it belongs to.
    /// </summary>
    [Replicated] public bool FacingLeft;

    // Guard SpriteAnimator.SetAnimation the same way PlayerController's old
    // per-instance `_anim` field did: re-issuing the same clip every frame
    // restarts it from frame 0, which reads as a stutter. FacingLeft is guarded
    // alongside it for symmetry and to keep the flip off the per-frame path.
    private int _appliedAnim = -1;
    private bool _appliedFacing;
    private bool _facingApplied;

    public override void OnAttach()
    {
        // Apply whatever the fields already hold (idle, facing right, by default)
        // immediately rather than waiting a frame for OnUpdate.
        ApplyAnim(AnimState);
        ApplyFacing(FacingLeft);
    }

    public override void OnUpdate(float deltaTime)
    {
        // Looked up per frame rather than cached in OnAttach: scripts on an entity
        // attach one at a time in list order, so a sibling is only guaranteed to be
        // live from OnUpdate onward (see Entity.GetScript).
        //
        // HasAuthority is the gate the controller itself uses, and it must be the
        // same one: the owner is the only peer whose PlayerController runs, so it is
        // the only peer whose AnimIndex/FacingLeft mean anything. On every other peer
        // these fields arrive from the wire and are simply applied.
        if (Net.HasAuthority(Self) && GetScript<PlayerController>() is { } controller)
        {
            AnimState = controller.AnimIndex;
            FacingLeft = controller.FacingLeft;
        }
        ApplyAnim(AnimState);
        ApplyFacing(FacingLeft);
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

    /// <summary>Push the facing onto the sprite. The <c>_facingApplied</c> latch is
    /// what makes the very first apply happen even when the value is the default
    /// <c>false</c>, which a bare equality guard would skip.</summary>
    private void ApplyFacing(bool left)
    {
        if (_facingApplied && left == _appliedFacing)
        {
            return;
        }
        _facingApplied = true;
        _appliedFacing = left;
        // Negative transform scale is clamped away by the 2D physics transform sync,
        // so the sprite flag is the route.
        SpriteRenderer.SetFlipX(Self, left);
    }

    private static string ClipFor(int index) => index switch
    {
        PlayerController.AnimIndexRun => PlayerController.AnimRun,
        PlayerController.AnimIndexJump => PlayerController.AnimJump,
        _ => PlayerController.AnimIdle,
    };
}

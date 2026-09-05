using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Networking glue for the Whisper player prefab: replicates the presentation
/// decisions <see cref="PlayerController"/> makes - which animation clip is
/// playing, which way the character faces, and what colour it is - so remote
/// copies of a player animate, face and read correctly instead of sliding around
/// in whatever pose they spawned in.
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

    /// <summary>
    /// Which <see cref="PlayerPalette"/> entry this player wears - its character tint,
    /// its name tag, and its roster row all read this one number.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Replicated, and authored by the owner, through the same mechanism as
    /// <see cref="AnimState"/> and <see cref="FacingLeft"/> rather than a third one. It
    /// could be DERIVED on each peer instead - the owning connection id is replicated on
    /// the entity, so every peer could compute the same index from it without a byte
    /// crossing the wire - and that is exactly the shortcut worth not taking: the moment
    /// a player is allowed to CHOOSE a colour, a derived value has nowhere to put the
    /// choice, and the change is a new replication path rather than a different value in
    /// this field. Which player wears which colour is a decision, and decisions belong to
    /// the peer that owns the thing being decided.
    /// </para>
    /// <para>
    /// Seeded from the owning connection so a session that nobody customises still comes
    /// up with everyone in a different colour.
    /// </para>
    /// </remarks>
    [Replicated] public int ColorIndex;

    /// <summary>This player's colour, for anything drawing alongside it - the name tag,
    /// the roster. One lookup, so a tag can never disagree with the character under it.</summary>
    public Vector4 Color => PlayerPalette.At(ColorIndex);

    // Guard SpriteAnimator.SetAnimation the same way PlayerController's old
    // per-instance `_anim` field did: re-issuing the same clip every frame
    // restarts it from frame 0, which reads as a stutter. FacingLeft is guarded
    // alongside it for symmetry and to keep the flip off the per-frame path.
    private int _appliedAnim = -1;
    private bool _appliedFacing;
    private bool _facingApplied;
    private int _appliedColor = -1;

    public override void OnAttach()
    {
        // Apply whatever the fields already hold (idle, facing right, by default)
        // immediately rather than waiting a frame for OnUpdate.
        ApplyAnim(AnimState);
        ApplyFacing(FacingLeft);
        ApplyColor(ColorIndex);
    }

    /// <summary>
    /// Seeds <see cref="ColorIndex"/> from the owning connection exactly once, the
    /// moment this peer is confirmed to own the player. Replaces re-deriving it
    /// every frame in <see cref="OnUpdate"/>: that used to be necessary only because
    /// on a client <see cref="Net.IsOwner"/> answers false for everything until the
    /// host's Welcome lands, so ownership was not knowable in <see cref="OnAttach"/> -
    /// this hook IS that knowable moment, offline, on the host, and on a welcomed
    /// client alike, so there is nothing left to re-derive on the frames after it.
    /// </summary>
    public override void OnOwnershipChanged(uint owner, bool isOwner)
    {
        if (!isOwner)
        {
            return;
        }
        ColorIndex = (int) (owner % (uint) PlayerPalette.Count);
        ApplyColor(ColorIndex);
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
        ApplyColor(ColorIndex);
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

    /// <summary>Tint the sprite. Latched on the index rather than the colour so the
    /// per-frame path is an integer compare, and so the first apply happens even for the
    /// default index 0.</summary>
    private void ApplyColor(int index)
    {
        if (index == _appliedColor)
        {
            return;
        }
        _appliedColor = index;
        SpriteRenderer.SetTint(Self, PlayerPalette.At(index));
    }

    private static string ClipFor(int index) => index switch
    {
        PlayerController.AnimIndexRun => PlayerController.AnimRun,
        PlayerController.AnimIndexJump => PlayerController.AnimJump,
        _ => PlayerController.AnimIdle,
    };
}

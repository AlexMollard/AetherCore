using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The ONE emissive-tint highlight mechanism, shared by <see cref="PhysicsGun"/> (the held
/// prop) and <see cref="ToolGun"/> (Wire/Weld/Rope's pending-source tint, the live
/// hover-validity indicator, and the reject flash) - the task that added the last two of
/// those explicitly rejected letting each script hand-roll its own apply/clear pair a
/// second time, since two conventions for "what a highlighted entity looks like" is worse
/// than one shared one.
/// </summary>
/// <remarks>
/// <para>
/// <b>Captures the REAL baseline, not an assumed one.</b> PhysicsGun's original
/// HeldEmissiveTint comment documented a known simplification: "assumes every grabbable
/// prop is authored at emissive black... a prop with its own baseline emissive would need
/// that value remembered instead of assumed." <see cref="Apply"/> reads
/// <see cref="MaterialEditor.GetEmissive"/> before tinting and hands the caller that exact
/// value back, so <see cref="Clear"/> restores what was really there - correct for
/// future non-black content, and also correct TODAY for a case the old assumed-zero code
/// got wrong: aiming Weld/Rope mode at a prop the physgun is already holding tints it a
/// SECOND time (see ToolGun's own pending-source apply) - clearing that second tint used
/// to reset it straight to Vector3.Zero, wiping the physgun's still-active held tint
/// early. Capturing the actual pre-tint value composes correctly instead.
/// </para>
/// <para>
/// <b>Never a save hazard under normal use.</b> Every applied tint always writes into a
/// real, reflected <c>MaterialInstanceComponent</c> the scene serializer's own
/// <c>CaptureMaterial</c> can and does pick up (confirmed by reading
/// <c>MaterialSerde.cpp</c> directly, not assumed) - so a highlight IS visible to a save
/// for as long as it is active, exactly like any other live material edit. What keeps it
/// out of a committed save under NORMAL use is the engine's own Play/Stop boundary
/// (<c>play</c> snapshots the scene, <c>stop</c> restores that snapshot - see their own
/// doc comments), not anything this file does: every caller here clears its tint on
/// release/throw/cancel/reject-timeout before Stop ever runs, so the live scene is back
/// at baseline well before a subsequent Save could see it. The one path this cannot
/// cover is a scene.save issued WHILE still in Play with something actively tinted (an
/// operator/tooling action, not gameplay) - already possible today independent of this
/// file (an earlier incident baked a duplicate crosshair canvas into Sandbox.scene.toml
/// exactly that way - see PhysicsGun.EnsureHud's own comment), and out of a gameplay
/// script's reach to prevent without new engine-side save/Play-state coupling that
/// nothing here asked for. Named explicitly rather than silently accepted.
/// </para>
/// <para>
/// <b>World.IsValid-gated on both ends</b>, independent of the native
/// aether_entity_material_* exports' own EntityAlive guard (added alongside this file) -
/// belt and braces, not redundant: this makes the C# call sites correct on their own
/// terms (every one of Release/Throw/CancelPending/FlashReject-timeout/Remove-tool
/// deletion/mid-hold destruction can call <see cref="Clear"/> unconditionally, with no
/// per-call-site IsValid check of its own to remember), rather than depending on a native
/// guard three layers away to make an unguarded call site merely harmless instead of
/// correct.
/// </para>
/// </remarks>
public static class Highlight
{
    /// <summary>Tints <paramref name="entity"/> and returns its emissive value from just
    /// before the tint - the caller stores this alongside whatever field already tracks
    /// the highlighted entity (e.g. PhysicsGun's own <c>_held</c>) and hands it back to
    /// <see cref="Clear"/> later. Vector3.Zero (a no-op-safe value) on an already-dead
    /// entity, so a caller never has to guard the call itself.</summary>
    public static Vector3 Apply(Entity entity, Vector3 tint)
    {
        if (!World.IsValid(entity))
        {
            return Vector3.Zero;
        }
        Vector3 baseline = entity.Material.GetEmissive();
        entity.Material.SetEmissive(tint);
        return baseline;
    }

    /// <summary>Restores whatever the matching <see cref="Apply"/> call captured. Safe to
    /// call on an entity that no longer exists (mid-hold/mid-pending destruction, or the
    /// tool's own Remove mode deleting the very entity another mode had pending) - a
    /// no-op, not an error.</summary>
    public static void Clear(Entity entity, Vector3 baseline)
    {
        if (!World.IsValid(entity))
        {
            return;
        }
        entity.Material.SetEmissive(baseline);
    }
}

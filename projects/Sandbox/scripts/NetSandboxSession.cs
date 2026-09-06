using AetherCore;

namespace AetherGame;

/// <summary>
/// The Sandbox arena's session. An empty body over <see cref="NetSessionDirector"/> is a
/// complete, working session per that class's own doc comment - room-code join, hosting,
/// rostering, reconnect and the offline solo fallback are all handled there. This type
/// exists only because the base class is abstract and the scene needs a concrete script
/// name to attach; what actually makes a freshly spawned player start owning and driving
/// its own body (rather than every peer's copy of it fighting over the same keyboard) is
/// <see cref="NetPlayerRig"/> on the player prefab, not this class.
/// </summary>
/// <remarks>
/// <see cref="NetSessionDirector.PlayerPrefab"/> is left at its base default of "player" -
/// see <see cref="NetPlayerRig"/>'s own file comment for what that prefab must (and must
/// not) bake in.
///
/// <see cref="NetSessionDirector.ReturnScene"/> is changed from the base's empty default
/// ("stay here") to "SandboxMenu": this project uses a two-scene design - a lobby that
/// loads into this arena via <c>NetSandboxConnectMenu.ArenaScene</c> - so leaving the
/// session (via <see cref="Leave"/>, or after the host vanishes) needs somewhere to send
/// the player back to, or they would be stranded in a scene with no menu left to host or
/// join again from. Set here, in the constructor, rather than in <c>OnAttach</c>, so a
/// scene-authored override of this field still wins - a subclass constructor runs before
/// the scene loader applies any authored field values, exactly like changing the base
/// class's own field initializer would.
///
/// <see cref="WantsToLeave"/> is overridden to false, same as Whisper's and Hollowtide's
/// own sessions - the base default binds it straight to Escape
/// (<c>Api.InputIsKeyPressed(Key.Escape)</c>), which without this override ends the
/// session immediately on one keystroke: no confirmation, no pause menu, just gone.
/// Escape is freed up here for a pause menu to use instead; leaving the session becomes
/// something a menu action calls explicitly, not something a bare keypress does behind
/// the player's back.
/// </remarks>
public sealed class NetSandboxSession : NetSessionDirector
{
    public NetSandboxSession()
    {
        ReturnScene = "SandboxMenu";
    }

    protected override bool WantsToLeave() => false;
}

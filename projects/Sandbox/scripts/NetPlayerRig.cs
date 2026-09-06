using AetherCore;

namespace AetherGame;

/// <summary>
/// Lives on the player prefab's root and is the one thing about a spawned player that is
/// NOT conditional on ownership - because it is what decides what else is.
/// </summary>
/// <remarks>
/// <para>
/// <see cref="FirstPersonPlayer"/> (on the root) and, on the Main Camera child,
/// <see cref="PhysicsGun"/>/<see cref="SpawnMenu"/>/<see cref="PropSpawner"/>/
/// <see cref="ToolGun"/> must NOT be
/// baked directly into the "player" prefab the way they are on the scene-placed solo
/// 'Player' entity. Every one of them reads real OS input (mouse delta, WASD, Q, F,
/// scroll) with no per-entity isolation - the SAME input a script instance on ANY player
/// entity in this peer's world would see, and there is no ownership check anywhere in the
/// engine's own dispatch of <c>EntityScript.OnUpdate</c> to stop that. Baked onto a shared
/// prefab, every remote player's copy would fight the local one for movement (see
/// <see cref="FirstPersonPlayer"/>'s own CharacterController.Move call - the character
/// controller's native implementation has no owner check either, unlike the freeze this
/// engine already applies to a non-owned RigidBody2D/3D, see
/// NetworkContext::SyncSimulationAuthority's own comment) and spawn its own duplicate
/// crosshair/spawn-menu UI on top of the real one for every OTHER player standing in the
/// arena.
/// </para>
/// <para>
/// <see cref="EntityScript.OnOwnershipChanged"/> is the framework's own answer to "when is
/// ownership known" - fired once, the first frame the answer is decided (immediately
/// offline and for the host, the first frame after the host's Welcome lands on a client),
/// and again only if it ever changes. That is exactly the one moment this needs: attach
/// the input-driving scripts once, only on the copy that should ever move under THIS
/// peer's own hands. A remote player's copy never becomes the owner, so it never gets
/// them - that entity is replication's to move, exactly like a non-owned physics body.
/// </para>
/// <para>
/// The same reasoning applies to the camera itself: the prefab's Main Camera child must
/// NOT be authored with its own <c>main = true</c> - <see cref="Camera.SetMain"/>'s own
/// doc comment says there is exactly one main camera scene-wide, so every remote player's
/// camera claiming it on spawn would leave whichever connection's player was created LAST
/// rendering the view for everyone. This claims it explicitly, and only once ownership is
/// actually confirmed - never for a remote player's copy.
/// </para>
/// <para>
/// The <c>GetScript</c> guards make this idempotent rather than one-shot: a player entity
/// is not expected to change owner while it is still connected (see
/// <c>NetworkIdentity::owner</c>'s own comment - it is set at spawn and released on
/// disconnect, and PhysicsGun's self-hit guard stops a physics gun from ever claiming a
/// player entity by mistake), but a second <see cref="OnOwnershipChanged"/> call with
/// <paramref name="isOwner"/> still true must never double-attach the camera's crosshair
/// UI.
/// </para>
/// </remarks>
public sealed class NetPlayerRig : EntityScript
{
    /// <summary>The one piece of this class that really is unconditional (see the
    /// class's own file comment) - a visible body for every player copy, owner and
    /// remote alike, fired at spawn before ownership is even known. FirstPersonPlayer's
    /// own OnAttach carries the identical guarded call for the scene-placed solo
    /// 'Player' entity, which carries no NetPlayerRig at all; whichever of the two
    /// scripts is present on a given entity is the one that actually adds it.</summary>
    public override void OnAttach()
    {
        if (Self.GetScript<PlayerBody>() == null)
        {
            Self.AddScript(nameof(PlayerBody));
        }
    }

    public override void OnOwnershipChanged(uint owner, bool isOwner)
    {
        if (!isOwner)
        {
            return;
        }

        if (Self.GetScript<FirstPersonPlayer>() == null)
        {
            Self.AddScript(nameof(FirstPersonPlayer));
        }

        Entity camera = Self.ChildCount > 0 ? Self.GetChild(0) : default;
        if (!camera.IsValid)
        {
            Log.Warn("[Sandbox] NetPlayerRig: player prefab has no camera child; gun/menu/spawner/tool gun not attached.");
            return;
        }

        Camera.SetMain(camera);

        if (camera.GetScript<PhysicsGun>() == null)
        {
            camera.AddScript(nameof(PhysicsGun));
        }
        if (camera.GetScript<SpawnMenu>() == null)
        {
            camera.AddScript(nameof(SpawnMenu));
        }
        if (camera.GetScript<PropSpawner>() == null)
        {
            camera.AddScript(nameof(PropSpawner));
        }
        if (camera.GetScript<ToolGun>() == null)
        {
            camera.AddScript(nameof(ToolGun));
        }
        if (camera.GetScript<UiSpawnCatalog>() == null)
        {
            camera.AddScript(nameof(UiSpawnCatalog));
        }
    }
}

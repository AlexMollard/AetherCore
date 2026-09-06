using AetherCore;

namespace AetherGame;

/// <summary>
/// GMod-style spawn menu toggle: the "open_menu" InputActions binding (Q by default,
/// rebindable in Settings) opens/closes it. The panel itself - category tabs,
/// the prop grid, click-to-spawn - is owned by <see cref="UiSpawnCatalog"/>, a sibling
/// script on this same camera entity that reads <see cref="IsOpen"/> below and calls
/// <see cref="PropSpawner.SpawnProp"/> directly, so there is still exactly one way a
/// prop comes into the world.
///
/// <see cref="IsOpen"/> is the gate every other gameplay script checks (FirstPersonPlayer,
/// PhysicsGun, PropSpawner, DebugFlyCam all read it via <c>GetScript&lt;SpawnMenu&gt;()</c>),
/// never <see cref="Ui.HasFocus"/> - see this class's own prior file comment for why
/// (kept from the original, still true): HasFocus is a global query several unrelated
/// screens could each flip, and IsOpen is this menu's own boolean, owned by this script
/// alone.
/// </summary>
public sealed class SpawnMenu : EntityScript
{
    /// <summary>True while the menu is up - gameplay input is suppressed for as
    /// long as this is true, and only for that long.</summary>
    public bool IsOpen { get; private set; }

    private Entity _player;

    public override void OnAttach()
    {
        _player = Self.Parent;
        // Register every attach, not just the first - an idempotent upsert (InputActions'
        // own doc comment), and the fix for the exact bug PropSpawner's own file comment
        // documents for "spawn_prop": a fresh per-connection script re-hardcoding the
        // compiled-in default would silently undo a saved rebind on every reconnect.
        SandboxSettings.EnsureLoaded();
        InputActions.Register("open_menu", SandboxSettings.BoundKeys["open_menu"]);
    }

    public override void OnDetach()
    {
        // Focus is process-wide, not per scene - leaving a button focused across a
        // scene teardown points the next screen's Enter/Space at an entity that no
        // longer exists.
        if (IsOpen)
        {
            Ui.ClearFocus();
        }
    }

    public override void OnUpdate(float deltaTime)
    {
        // Owner-only, same reasoning as PhysicsGun/PropSpawner: _player, not Self (the
        // camera), because Self carries no NetworkIdentity of its own and would read
        // Net.HasAuthority as unconditionally true otherwise. True offline, so
        // single-player is unaffected.
        if (!Net.HasAuthority(_player))
        {
            return;
        }

        if (IsOpen)
        {
            if (InputActions.IsPressed("open_menu"))
            {
                Close();
            }
            return;
        }

        if (InputActions.IsPressed("open_menu"))
        {
            Open();
        }
    }

    private void Open()
    {
        IsOpen = true;
        // Release FirstPersonPlayer's pointer lock (it re-requests it every active
        // frame, so simply not being active while the menu is open would be enough on
        // its own, but releasing explicitly here means the cursor is free to click a
        // tile the very same frame the menu opens, not one frame later). UiSpawnCatalog
        // owns focusing its own first tile once it shows itself.
        Input.CursorLockRequested = false;
        Input.OsCursorVisible = true;
    }

    private void Close()
    {
        IsOpen = false;
        Ui.ClearFocus();
    }
}

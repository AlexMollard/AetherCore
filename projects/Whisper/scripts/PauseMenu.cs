using AetherCore;

namespace AetherGame;

/// <summary>
/// The arena's pause menu: Escape puts it up, and leaving the session is behind a
/// Disconnect button on it rather than on the key itself.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why a menu at all.</b> Escape used to leave the session outright, which is a
/// single keypress between a player and the end of everybody's game - and it collided
/// with the chat box, which uses Escape to cancel a half-typed message. One press both
/// discarded the draft and quit to the title screen.
/// </para>
/// <para>
/// <b>How Escape has exactly one owner now.</b> The engine's UI pass runs before any
/// script and CONSUMES a key it acted on, so an Escape that cancelled a chat edit is
/// already spent by the time this runs and this sees nothing. When the chat is not
/// editing, nothing else claims the key and this is its only reader. There is no flag
/// published by one script for another to read, and therefore no dependence on which
/// script updates first - which is what the old <c>ChatBox.LocalIsTyping</c> static got
/// wrong, and could not have got right.
/// </para>
/// <para>
/// <b>The menu does not pause the world</b>, and the subtitle on it says so. This is a
/// live multiplayer session: freezing the local simulation would not freeze anybody
/// else's, so a "pause" that stopped time would only mean this player stops being able
/// to react while everyone else keeps playing. What it does instead is take the
/// keyboard, which <see cref="PlayerController"/> already stands still for.
/// </para>
/// <para>
/// The screen itself is authored in <c>Arena.scene.toml</c> as a disabled subtree of the
/// HUD canvas, not built here. Two reasons: real <c>UIButton</c>s with <c>UISelectable</c>
/// are what make the menu keyboard-navigable and what make <see cref="Ui.HasFocus"/> true
/// while it is up, and there is no script-side button constructor to build them with.
/// </para>
/// </remarks>
public sealed class PauseMenu : EntityScript
{
    /// <summary>Name of the scene entity holding the menu's subtree.</summary>
    public string RootName = "PauseRoot";

    /// <summary>Name of the button that closes the menu.</summary>
    public string ResumeName = "ResumeButton";

    /// <summary>Name of the button that ends the session.</summary>
    public string DisconnectName = "DisconnectButton";

    private Entity _root;
    private Entity _resume;
    private Entity _disconnect;
    private bool _open;

    /// <summary>True while the menu is up. Read by <see cref="WhisperSession"/>, which
    /// must not also be listening for Escape while this is.</summary>
    public bool IsOpen => _open;

    /// <inheritdoc/>
    public override void OnAttach()
    {
        _root = Scene.Find(RootName);
        _resume = Scene.Find(ResumeName);
        _disconnect = Scene.Find(DisconnectName);
        if (!_root.IsValid)
        {
            Log.Warn($"[Whisper] PauseMenu: the scene has no '{RootName}' entity; Escape will do nothing");
            return;
        }
        // Authored disabled, and closed again here anyway: a scene edited in the editor
        // can be saved with the menu left showing, and a level that starts paused is a
        // confusing thing to ship by accident.
        Close();
    }

    /// <inheritdoc/>
    /// <remarks>Focus is process-wide rather than per scene, so a menu that took the
    /// keyboard and then had its scene torn down under it would leave the next screen's
    /// Enter and Space pointing at an element that no longer exists.</remarks>
    public override void OnDetach()
    {
        if (_open)
        {
            Ui.ClearFocus();
        }
    }

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        if (!_root.IsValid)
        {
            return;
        }

        if (_open)
        {
            TickOpen();
            return;
        }

        if (Input.IsKeyPressed(Key.Escape))
        {
            Open();
        }
    }

    private void TickOpen()
    {
        // Escape closes it as well as opening it. Symmetry matters here: a menu that can
        // only be dismissed by finding the right button is one more thing between the
        // player and the game they were playing.
        if (Input.IsKeyPressed(Key.Escape) || (_resume.IsValid && Ui.WasActivated(_resume)))
        {
            Close();
            return;
        }

        if (_disconnect.IsValid && Ui.WasActivated(_disconnect))
        {
            // Through the director, not through Net.Disconnect: leaving is a session
            // decision - hand the 2D bodies back, tell the other peers this was
            // deliberate, clear the reconnect machinery, load the menu - and all of that
            // already lives in one place.
            if (GetScript<WhisperSession>() is { } session)
            {
                Close();
                session.Leave(string.Empty);
                return;
            }
            Log.Error("[Whisper] PauseMenu: no WhisperSession on this entity, so Disconnect has nothing to leave");
        }
    }

    private void Open()
    {
        _open = true;
        _root.SetActive(true);
        // Explicitly, because nothing focuses itself: the engine never invents a focus,
        // which is what keeps Enter and Space with the game while the HUD is idle. A menu
        // that wants the keyboard has to ask, and this is where the asking is.
        if (_resume.IsValid)
        {
            Ui.SetFocus(_resume);
        }
    }

    private void Close()
    {
        _open = false;
        _root.SetActive(false);
        // And hand the keyboard straight back, or the next Space is "press Resume again"
        // instead of a jump. The menu owns the keyboard for exactly as long as it is up.
        Ui.ClearFocus();
    }
}

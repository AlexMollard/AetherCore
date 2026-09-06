using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The room-code lobby screen a player sees before the Sandbox arena loads: a code field,
/// Host/Join buttons, a dedicated room-code display with a Copy button, a status line, and
/// a way back to the Main Menu. All of the actual connect ladder - minting or validating a
/// code, traversal state, timeouts, narrating progress - lives in the base
/// <see cref="NetConnectMenu"/>; this only builds the on-screen controls and calls into
/// it, which is exactly what that class's own file comment warns not to duplicate.
/// </summary>
/// <remarks>
/// <para>
/// Lives in its own scene ("SandboxMenu"), not the arena ("Sandbox") - <see cref="ArenaScene"/>
/// is set to "Sandbox" below so a successful host or join really does <c>Api.SceneLoad</c>
/// into a clean arena rather than merely hiding this canvas over a scene that would
/// otherwise have already spawned an offline solo player before either button was ever
/// pressed (see <see cref="NetSessionDirector"/>'s own offline fallback). The scene load
/// tears this whole canvas down with it, so there is no hide-on-success bookkeeping to get
/// wrong here.
/// </para>
/// <para>
/// <b>Every non-success state gets a status line too, not just success.</b> StatusText
/// (base class) already narrates hosting, every rung of a join's traversal, a failure, a
/// timeout and a cancel - this only had to make sure the label showing it is actually
/// legible (it had no font size or colour set at all before) and that a failed or timed-out
/// join leaves the player able to try again rather than stranded: it does, for free -
/// CancelJoin (called by the base class on failure/timeout) clears IsJoining, which is the
/// same flag this class's own IsBusy gate below already reads, so Host/Join/the code box
/// re-enable themselves the instant the base class reports the attempt is over.
/// </para>
/// <para>
/// <b>Buttons now SAY they are disabled while busy, instead of silently swallowing the
/// click.</b> The old code already refused a Host/Join press while <see cref="NetConnectMenu.IsBusy"/>
/// (the `if (!IsBusy)` gate below), but nothing told the player that - a click while hosting
/// or joining looked identical to a click that simply did not register. <c>Ui.SetInteractable</c>
/// now reflects the same gate the click handler already enforces, which is exactly the "the
/// screen SAY so instead of swallowing a click" idiom the engine's own generated multiplayer
/// lobby template (<c>ProjectCommon.cpp</c>'s <c>MakeLobbyScriptText</c>) already uses -
/// copied here rather than reinvented.
/// </para>
/// <para>
/// <b>The room code gets its own large, dedicated line plus a Copy button</b> (also lifted
/// from that same generated template, which solved this exact problem already): the minted
/// code is the entire join credential, and it used to live only inside a sentence in the
/// general status line ("Hosting ABC123. Press Enter to start.") at whatever the default
/// text size is - easy to misread, and nothing to hand it to a second player with except
/// reading it aloud. <see cref="Input.Clipboard"/> is a real SDK primitive (checked
/// <c>Input.cs</c>/<c>Native.cs</c> before assuming it existed) - <c>Copy</c> writes the
/// code there and the button's own label flips to "Copied" for a beat, because a clipboard
/// write nobody can see happen is indistinguishable from one that silently failed.
/// </para>
/// <para>
/// <b>A Back button returns to "MainMenu"</b> (the exact scene name <see cref="UiMainMenu"/>'s
/// own Play button already uses to reach this one - <c>Scene.Load("SandboxMenu")</c> there,
/// <c>Scene.Load("MainMenu")</c> here), the one gap this screen had with no way to close it
/// short of quitting the process. Deliberately never disabled, unlike every other control
/// here: a player who clicked Host by mistake, or whose join is timing out, must always have
/// an exit that is not "wait" or "alt-F4". It cleans up whichever attempt is in flight before
/// leaving - <see cref="NetConnectMenu.CancelJoin"/> for a join (the one call that also puts
/// <see cref="Net.ReplicationReady"/> back to true, which a bare <see cref="Net.Disconnect"/>
/// would not) or a plain <see cref="Net.Disconnect"/> for a host (never touched
/// ReplicationReady in the first place - see <see cref="NetConnectMenu.Host"/>'s own remarks) -
/// so a session never keeps listening or connecting in the background after the player has
/// already left the screen that started it.
/// </para>
/// </remarks>
public sealed class NetSandboxConnectMenu : NetConnectMenu
{
    private const int RoomCodeLength = 6; // matches RoomCode.hpp's kRoomCodeLength

    private Entity _canvas;
    private Entity _codeBox;
    private Entity _hostButton;
    private Entity _joinButton;
    private Entity _statusLabel;
    private Entity _roomCodeLabel;
    private Entity _copyButton;
    private Entity _backButton;

    private float _copyFeedbackSecondsLeft;

    public override void OnAttach()
    {
        ArenaScene = "Sandbox";

        _canvas = Ui.CreateCanvas();
        // Missing before this pass - every other runtime canvas in this project already
        // calls this (see PhysicsGun.EnsureHud's own comment: an accidental save during
        // Play previously baked a duplicate crosshair canvas permanently into
        // Sandbox.scene.toml). Grouped under a shared container for the same reason
        // PropSpawner/Emitter/ToolGun now are - one Hierarchy panel entry instead of
        // a loose root entity - safe to combine with a transient container since this
        // canvas is transient too (see RuntimeContainers' own file comment on why that
        // pairing matters and the Wires case does not use it).
        _canvas.MarkTransient();
        _canvas.SetParent(RuntimeContainers.Get("Runtime UI"));

        Entity title = Ui.CreateText(_canvas, "AETHERCORE SANDBOX");
        Ui.SetAnchors(title, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(title, new Vector2(0.5f, 1.0f));
        Ui.SetRect(title, 0.0f, -150.0f, 420.0f, 40.0f);
        Ui.SetTextAlign(title, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(title, 28.0f);

        // The room code's own line, above the code box in the previously-empty space
        // between it and the title - a status sentence alone is easy to misread, so the
        // minted code gets a large, dedicated display the instant Host() succeeds.
        _roomCodeLabel = Ui.CreateText(_canvas, string.Empty);
        Ui.SetAnchors(_roomCodeLabel, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_roomCodeLabel, new Vector2(1.0f, 1.0f));
        Ui.SetRect(_roomCodeLabel, -6.0f, -120.0f, 200.0f, 28.0f);
        Ui.SetTextAlign(_roomCodeLabel, UiHAlign.Right, UiVAlign.Middle);
        Ui.SetFontSize(_roomCodeLabel, 24.0f);
        Ui.SetTextColor(_roomCodeLabel, UiTheme.Accent);

        _copyButton = Ui.CreateButton(_canvas);
        Ui.SetAnchors(_copyButton, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_copyButton, new Vector2(0.0f, 1.0f));
        Ui.SetRect(_copyButton, 6.0f, -120.0f, 90.0f, 28.0f);
        Ui.SetButtonLabel(_copyButton, "Copy");
        _copyButton.SetActive(false); // nothing to copy until a Host() succeeds below

        _codeBox = Ui.CreateTextBox(_canvas);
        Ui.SetAnchors(_codeBox, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_codeBox, new Vector2(0.5f, 1.0f));
        Ui.SetRect(_codeBox, 0.0f, -60.0f, 240.0f, 36.0f);
        Ui.SetPlaceholder(_codeBox, "ROOM CODE");
        Ui.SetContentType(_codeBox, UiContentType.Alphanumeric);
        Ui.SetMaxLength(_codeBox, RoomCodeLength);
        Ui.SetFontSize(_codeBox, 22.0f);
        Ui.SetTextAlign(_codeBox, UiHAlign.Center, UiVAlign.Middle);

        _hostButton = Ui.CreateButton(_canvas);
        Ui.SetAnchors(_hostButton, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_hostButton, new Vector2(1.0f, 1.0f));
        Ui.SetRect(_hostButton, -6.0f, 0.0f, 117.0f, 36.0f);
        Ui.SetButtonLabel(_hostButton, "Host");

        _joinButton = Ui.CreateButton(_canvas);
        Ui.SetAnchors(_joinButton, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_joinButton, new Vector2(0.0f, 1.0f));
        Ui.SetRect(_joinButton, 6.0f, 0.0f, 117.0f, 36.0f);
        Ui.SetButtonLabel(_joinButton, "Join");

        _statusLabel = Ui.CreateText(_canvas, string.Empty);
        Ui.SetAnchors(_statusLabel, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_statusLabel, new Vector2(0.5f, 1.0f));
        Ui.SetRect(_statusLabel, 0.0f, 100.0f, 420.0f, 90.0f);
        Ui.SetTextAlign(_statusLabel, UiHAlign.Center, UiVAlign.Top);
        Ui.SetFontSize(_statusLabel, UiTheme.FontSizeBody);
        Ui.SetTextColor(_statusLabel, UiTheme.TextColor);

        // Anchored to the screen's own top-left corner, independent of the centred cluster
        // above - the one control on this screen that must never end up disabled or hidden
        // behind another element, since it is every dead end's only exit.
        _backButton = Ui.CreateButton(_canvas);
        Ui.SetAnchors(_backButton, Vector2.Zero, Vector2.Zero);
        Ui.SetPivot(_backButton, Vector2.Zero);
        Ui.SetRect(_backButton, UiTheme.SpacingLarge, UiTheme.SpacingLarge, 100.0f, 36.0f);
        Ui.SetButtonLabel(_backButton, "Back");

        // Typing a code and clicking buttons, not looking around - nothing here should
        // fight the OS cursor away from the UI the way a gameplay scene's FirstPersonPlayer
        // would (this scene never spawns one).
        Input.CursorLockRequested = false;
    }

    public override void OnUpdate(float deltaTime)
    {
        base.OnUpdate(deltaTime);

        if (Ui.WasActivated(_backButton))
        {
            GoToMainMenu();
            return;
        }

        Ui.SetText(_statusLabel, StatusText);

        // The room code and its Copy button live outside the busy gate below on purpose:
        // IsBusy is true for the entire time this screen is showing a freshly hosted code
        // (IsHosting is one of the two things IsBusy means), which is exactly when
        // re-copying it is most useful - a host who missed the first read, or wants to hand
        // the code to a second friend, still has a working button.
        bool hasCode = IsHosting && HostedRoomCode.Length > 0;
        Ui.SetText(_roomCodeLabel, hasCode ? HostedRoomCode : string.Empty);
        _copyButton.SetActive(hasCode);
        if (hasCode)
        {
            if (_copyFeedbackSecondsLeft > 0.0f)
            {
                _copyFeedbackSecondsLeft -= deltaTime;
                if (_copyFeedbackSecondsLeft <= 0.0f)
                {
                    Ui.SetButtonLabel(_copyButton, "Copy");
                }
            }
            if (Ui.WasActivated(_copyButton))
            {
                Input.Clipboard = HostedRoomCode;
                _copyFeedbackSecondsLeft = 1.5f;
                Ui.SetButtonLabel(_copyButton, "Copied");
                // See the class doc's own remark on why the generated template does this
                // too: Enter/Space activating this button while it holds focus would
                // otherwise be the only thing left listening once IsHosting disables every
                // other selectable, silently eating the very key WantsToEnterArena polls
                // for. Clearing focus hands that key straight back.
                Ui.ClearFocus();
            }
        }

        // NetConnectMenu already refuses a second Host/Join while one is in flight (the
        // gate below) - this just makes the screen SAY so instead of swallowing a click.
        bool idle = !IsBusy;
        Ui.SetInteractable(_hostButton, idle);
        Ui.SetInteractable(_joinButton, idle);
        Ui.SetInteractable(_codeBox, idle);

        if (!idle)
        {
            return;
        }

        if (Ui.WasActivated(_hostButton))
        {
            Host();
        }
        else if (Ui.WasActivated(_joinButton))
        {
            Join(Ui.GetTextBoxText(_codeBox));
        }
    }

    private void GoToMainMenu()
    {
        // Clean up whichever attempt is in flight before leaving, so the session does not
        // keep listening or connecting in the background once the player is looking at a
        // different screen entirely - see this class's own file comment for why these two
        // calls are not interchangeable (CancelJoin also restores Net.ReplicationReady,
        // which a bare Net.Disconnect would leave stuck false).
        if (IsJoining)
        {
            CancelJoin("Cancelled.");
        }
        else if (IsHosting)
        {
            Net.Disconnect();
        }
        Scene.Load("MainMenu");
    }
}

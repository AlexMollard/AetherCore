using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The Sandbox HUD: a "Holding: &lt;name&gt;" line and a crosshair tint while the physgun
/// has something, a static F/Q hint row, and the live application of
/// <see cref="SandboxSettings"/> (look sensitivity, invert-Y, field of view) onto the local
/// player. Deliberately minimal - nothing here is more than the design called genuinely
/// needed, and the crosshair dot itself already exists (<c>PhysicsGun.EnsureHud</c>).
/// </summary>
/// <remarks>
/// <para>
/// <b>Lives on a plain scene entity, not a per-connection prefab</b> ("NetSession" in
/// Sandbox.scene.toml, alongside <see cref="UiPauseMenu"/>) - unlike <c>PhysicsGun</c>/
/// <c>SpawnMenu</c>, this never touches local input or builds its own canvas up front. It
/// only ever reaches the local player through <see cref="Camera.Main"/>, which
/// <c>NetPlayerRig</c> guarantees is always the LOCAL player's own camera (a remote
/// player's camera is never claimed as main) - so polling it every frame needs no
/// <see cref="Net.HasAuthority"/> check or ownership-change callback of its own.
/// </para>
/// <para>
/// <b>Attaches its own elements as children of PhysicsGun's existing HUD canvas</b>
/// (<c>PhysicsGun.Hud</c>) rather than building a second, competing canvas. PhysicsGun
/// already hides that canvas while the spawn menu (and, once <c>UiPauseMenu</c>'s gate
/// lands there, the pause menu) is open - children inherit that the moment
/// <see cref="Entity.SetActive"/> is called on their parent (it disables "this entity
/// AND its subtree"), so this needs no menu-suppression logic of its own.
/// </para>
/// <para>
/// <b>Depends on two small accessors PhysicsGun.cs does not have yet</b> - <c>Hud</c> (the
/// canvas entity, currently a private <c>_hud</c> field) and <c>Held</c> (the held prop
/// entity, currently a private <c>_held</c> field). Until those land, <see cref="EnsureHudChildren"/>/
/// <see cref="RefreshHeldDisplay"/> below cannot compile - see the routed diff notes.
/// </para>
/// <para>
/// <b>Settings application timing.</b> <c>FirstPersonPlayer</c>/the camera are recreated
/// fresh per connection (<c>NetPlayerRig.OnOwnershipChanged</c>), so a persisted setting
/// does not survive that by itself - it resets to the script's own compiled-in default
/// every time a new instance appears (new session, reconnect, host restart). This polls
/// <see cref="Camera.Main"/> every frame and re-applies whenever the camera instance is
/// new OR the persisted value itself changed since it was last applied (e.g. the player
/// tweaked it in Pause -&gt; Settings mid-session) - either alone is not enough.
/// </para>
/// </remarks>
public sealed class UiHud : EntityScript
{
    private Entity _appliedCamera;
    private float _appliedSensitivity = float.NaN;
    private bool _appliedInvertY;
    private float _appliedFov = float.NaN;

    private Entity _hudRoot;
    private Entity _heldLabel;
    private Entity _hintLabel;
    private Entity _interactLabel;
    private Entity _toolModeLabel;

    public override void OnUpdate(float deltaTime)
    {
        SandboxSettings.EnsureLoaded();

        Entity camera = Camera.Main;
        if (!camera.IsValid)
        {
            return;
        }

        ApplySettings(camera);

        PhysicsGun? gun = camera.GetScript<PhysicsGun>();
        if (gun == null)
        {
            return;
        }
        EnsureHudChildren(gun.Hud);
        RefreshHeldDisplay(gun);
        RefreshInteractPrompt(camera);
    }

    private void ApplySettings(Entity camera)
    {
        bool freshCamera = camera != _appliedCamera;

        if (freshCamera || !NearlyEqual(_appliedFov, SandboxSettings.FovDegrees))
        {
            Camera.SetPerspective(camera, SandboxSettings.FovDegrees);
            _appliedFov = SandboxSettings.FovDegrees;
        }

        FirstPersonPlayer? player = camera.Parent.GetScript<FirstPersonPlayer>();
        if (player != null)
        {
            if (freshCamera || !NearlyEqual(_appliedSensitivity, SandboxSettings.LookSensitivity))
            {
                player.LookSensitivity = SandboxSettings.LookSensitivity;
                _appliedSensitivity = SandboxSettings.LookSensitivity;
            }
            if (freshCamera || _appliedInvertY != SandboxSettings.InvertY)
            {
                player.InvertY = SandboxSettings.InvertY;
                _appliedInvertY = SandboxSettings.InvertY;
            }
        }

        _appliedCamera = camera;
    }

    /// <summary>Builds the held-item label and hint row once per HUD canvas instance
    /// (a fresh canvas appears each time PhysicsGun.EnsureHud runs on a newly owned
    /// camera - a new session, a reconnect). <c>_hudRoot</c> tracks which canvas these
    /// children already belong to, so a repeat call after the same canvas is a no-op.</summary>
    private void EnsureHudChildren(Entity hud)
    {
        if (!hud.IsValid || _hudRoot == hud)
        {
            return;
        }

        _heldLabel = Ui.CreateText(hud, string.Empty);
        Ui.SetAnchors(_heldLabel, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_heldLabel, new Vector2(0.5f, 1.0f));
        Ui.SetRect(_heldLabel, 0.0f, -34.0f, 320.0f, 24.0f);
        Ui.SetTextAlign(_heldLabel, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(_heldLabel, UiTheme.FontSizeHint);
        Ui.SetTextColor(_heldLabel, UiTheme.TextColor);

        // Below the crosshair rather than sharing the "Holding" label's spot above it -
        // the two are mutually exclusive in practice (nothing wired the physgun's own
        // interact key to fire while it also has a Button under the reticle) but each
        // gets its own slot regardless, so one never has to yield to or overwrite the
        // other's text. Pivot/offset signs: this engine's UI is Y-down at every layer
        // (see Ui.SetRect's own doc remarks) - pivot (0.5, 0.0) with a POSITIVE offset
        // is what actually renders below the anchor; a screenshot caught this and
        // _heldLabel swapped the two before this comment matched the code.
        _interactLabel = Ui.CreateText(hud, string.Empty);
        Ui.SetAnchors(_interactLabel, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_interactLabel, new Vector2(0.5f, 0.0f));
        Ui.SetRect(_interactLabel, 0.0f, 40.0f, 260.0f, 24.0f);
        Ui.SetTextAlign(_interactLabel, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(_interactLabel, UiTheme.FontSizeHint);
        Ui.SetTextColor(_interactLabel, UiTheme.TextColor);

        // Anchored to the canvas's own BOTTOM edge (anchor.y = 1.0 is bottom, not 0.0 -
        // same Y-down convention as above), pivot at that same edge, negative offset
        // pulling it up from the edge - the previous anchor.y = 0.0 put this at the TOP
        // of the canvas instead of "a low hint row", never actually rendering near the
        // bottom it was meant for.
        _hintLabel = Ui.CreateText(hud, "F spawn \u00b7 Q menu");
        Ui.SetAnchors(_hintLabel, new Vector2(0.5f, 1.0f), new Vector2(0.5f, 1.0f));
        Ui.SetPivot(_hintLabel, new Vector2(0.5f, 1.0f));
        Ui.SetRect(_hintLabel, 0.0f, -24.0f, 320.0f, 20.0f);
        Ui.SetTextAlign(_hintLabel, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(_hintLabel, UiTheme.FontSizeHint);
        Ui.SetTextColor(_hintLabel, UiTheme.TextMuted);

        // Directly above the F/Q hint row, same anchor/pivot convention - one more
        // static-position readout, always visible while the gun is present rather than
        // show/hide like _interactLabel, so scrolling to a new mode is confirmed
        // immediately without needing to fire it first.
        _toolModeLabel = Ui.CreateText(hud, string.Empty);
        Ui.SetAnchors(_toolModeLabel, new Vector2(0.5f, 1.0f), new Vector2(0.5f, 1.0f));
        Ui.SetPivot(_toolModeLabel, new Vector2(0.5f, 1.0f));
        Ui.SetRect(_toolModeLabel, 0.0f, -48.0f, 320.0f, 18.0f);
        Ui.SetTextAlign(_toolModeLabel, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(_toolModeLabel, UiTheme.FontSizeHint);
        Ui.SetTextColor(_toolModeLabel, UiTheme.TextMuted);

        _hudRoot = hud;
    }

    /// <summary>Updates the held-item label text. The crosshair dot itself stays
    /// PhysicsGun's own to draw and tint (it already exists and already reads <c>_held</c>
    /// internally) - this only adds the text line PhysicsGun does not have a slot for.</summary>
    private void RefreshHeldDisplay(PhysicsGun gun)
    {
        if (!_heldLabel.IsValid)
        {
            return;
        }
        Entity held = gun.Held;
        Ui.SetText(_heldLabel, held.IsValid ? $"Holding: {held.Name}" : string.Empty);
    }

    /// <summary>Shows "[key] Use" while ToolGun's own aim ray is over something
    /// Interact() would actually press, and clears it the instant the player looks
    /// away or steps out of range - both collapse to the one InteractTarget check,
    /// since ToolGun bounds its raycast to MaxRange itself (see InteractTarget's
    /// own field comment on ToolGun). Also shows the tool gun's current mode
    /// (Wire/Light/Colour/Remove) on its own line whenever the gun is present, so a
    /// scroll-cycled mode never has to be guessed from the last click's effect.</summary>
    private void RefreshInteractPrompt(Entity camera)
    {
        ToolGun? tool = camera.GetScript<ToolGun>();
        if (_interactLabel.IsValid)
        {
            Ui.SetText(_interactLabel, tool != null && tool.InteractTarget.IsValid ? tool.InteractPromptText : string.Empty);
        }
        if (_toolModeLabel.IsValid)
        {
            Ui.SetText(_toolModeLabel, tool != null ? tool.ModeLabel : string.Empty);
        }
    }

    private static bool NearlyEqual(float a, float b) => Math.Abs(a - b) < 0.0001f;
}

using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Starts a conversation from the world. Attach to an entity that has a 2D trigger collider.
/// Zone mode (RequireInteract=false) fires as the player enters. Interact mode shows a "> read" prompt
/// while the player is inside and fires on the interact key. Script-driven dialogue needs no trigger -
/// any script can call Dialogue.Play("id") directly (e.g. on level start).</summary>
public sealed class DialogueTrigger : EntityScript
{
    public string DialogueId = "";
    public bool Once = true;
    public bool RequireInteract = false;
    /// <summary>Interact-prompt text shown while the player is in range (e.g. "> read", "> talk").</summary>
    public string PromptLabel = "> read";
    /// <summary>Trigger box size in world units (self-provisioned sensor; drop the script and go).</summary>
    public Vector2 Size = new(2.0f, 3.0f);


    private bool _fired;       // has ever fired (gates Once)
    private bool _armed = true; // can fire on the current visit; consumed on fire, re-armed on exit
    private bool _playerInside;
    private float _promptT;
    private Entity _prompt;

    public override void OnAttach()
    {
        // Self-provision a static sensor box so a dialogue zone is just "add the script + set the id".
        Physics2D.AddBoxBody(Self, Size, Body2DType.Static);
        Physics2D.SetTrigger(Self, true);
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        if (!IsPlayer(other)) { return; }
        _playerInside = true;
        if (RequireInteract) { _promptT = 0f; ShowPrompt(true); }
        else { TryFire(); }
    }

    public override void OnTriggerExit2D(Entity other)
    {
        if (!IsPlayer(other)) { return; }
        _playerInside = false;
        _armed = true; // re-arm for the next approach (so "talk again" needs leaving + returning)
        ShowPrompt(false);
    }

    public override void OnUpdate(float dt)
    {
        if (!RequireInteract || !_playerInside) { return; }

        // Fire only while armed and idle. Consuming the arm on fire means the same in-range session
        // can't restart the conversation the moment it ends - the player must step out and back in.
        if (_armed && !Dialogue.IsActive && Controls.InteractPressed)
        {
            TryFire();
            ShowPrompt(false);
            return;
        }

        // Breathe the prompt while it is actually offering an interaction, so it reads as live.
        if (_armed && !Dialogue.IsActive && _prompt.IsValid)
        {
            _promptT += dt;
            Vector4 c = GameSettings.Accent;
            c.W = 0.72f + 0.28f * MathF.Sin(_promptT * 4.5f);
            Ui.SetTextColor(_prompt, c);
        }
    }

    private void TryFire()
    {
        if (!_armed) { return; }
        if (_fired && Once) { return; }
        if (string.IsNullOrEmpty(DialogueId)) { Log.Warn("[INKBOUND] DialogueTrigger has no DialogueId"); return; }
        Dialogue.Play(DialogueId);
        _fired = true;
        _armed = false;
    }

    private static bool IsPlayer(Entity e)
    {
        PlayerController? p = PlayerController.Instance;
        if (p != null) { return e.Id == p.Self.Id; }
        return e.Name == "Player";
    }

    private void ShowPrompt(bool on)
    {
        if (on && !_prompt.IsValid)
        {
            _prompt = Ui.CreateText(default, PromptLabel);
            Ui.SetFont(_prompt, "PixelStorm");
            Ui.SetFontSize(_prompt, 38f);
            Ui.SetTextColor(_prompt, GameSettings.Accent);
            Ui.SetTextAlign(_prompt, UiHAlign.Center, UiVAlign.Middle);
            Ui.SetAnchors(_prompt, new Vector2(0.5f, 1f), new Vector2(0.5f, 1f)); // bottom-centre
            Ui.SetPivot(_prompt, new Vector2(0.5f, 1f));
            Ui.SetRect(_prompt, 0f, -180f, 420f, 60f);
        }
        if (_prompt.IsValid) { _prompt.SetActive(on); }
    }

    public override void OnDetach()
    {
        if (_prompt.IsValid) { _prompt.Destroy(); }
    }
}

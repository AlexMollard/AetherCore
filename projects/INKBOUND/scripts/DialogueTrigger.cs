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

    private const Key InteractKey = Key.E; // free of the player's move/jump bindings (A/D/arrows, Space/W/Up)

    private bool _fired;
    private bool _playerInside;
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
        if (RequireInteract) { ShowPrompt(true); }
        else { TryFire(); }
    }

    public override void OnTriggerExit2D(Entity other)
    {
        if (!IsPlayer(other)) { return; }
        _playerInside = false;
        ShowPrompt(false);
    }

    public override void OnUpdate(float dt)
    {
        if (RequireInteract && _playerInside && !Dialogue.IsActive && Input.IsKeyPressed(InteractKey))
        {
            TryFire();
            ShowPrompt(false);
        }
    }

    private void TryFire()
    {
        if (_fired && Once) { return; }
        if (string.IsNullOrEmpty(DialogueId)) { Log.Warn("[INKBOUND] DialogueTrigger has no DialogueId"); return; }
        Dialogue.Play(DialogueId);
        _fired = true;
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
            Ui.SetFontSize(_prompt, 20f);
            Ui.SetTextColor(_prompt, GameSettings.Accent);
            Ui.SetTextAlign(_prompt, UiHAlign.Center, UiVAlign.Middle);
            Ui.SetAnchors(_prompt, new Vector2(0.5f, 1f), new Vector2(0.5f, 1f)); // bottom-centre
            Ui.SetPivot(_prompt, new Vector2(0.5f, 1f));
            Ui.SetRect(_prompt, 0f, -150f, 220f, 34f);
        }
        if (_prompt.IsValid) { _prompt.SetActive(on); }
    }

    public override void OnDetach()
    {
        if (_prompt.IsValid) { _prompt.Destroy(); }
    }
}

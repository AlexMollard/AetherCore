using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>A checkpoint flag. First pass-through becomes the respawn point and lights up. Its Index
/// (authored, ascending along the level) is written to the active save slot so 'return' can resume
/// here.</summary>
public sealed class Checkpoint : EntityScript
{
    public float SpawnYOffset = 0.5f;
    /// <summary>Ascending checkpoint ordinal within the level (1, 2, 3...). 0 = not resumable.</summary>
    public int Index = 1;

    private bool _active;
    private bool _resumeChecked;

    public override void OnAttach()
    {
        Physics2D.EnableEvents(Self);
        SpriteRenderer.SetTint(Self, new Vector4(0.6f, 0.6f, 0.6f, 1.0f)); // dim until reached
    }

    public override void OnUpdate(float deltaTime)
    {
        // Resume lands here on the first frame the player exists. Wait a frame if the player has not
        // attached yet (ConsumeResume only fires for the matching level+index, exactly once).
        if (_resumeChecked) return;
        PlayerController? p = PlayerController.Instance;
        if (p == null) return;
        _resumeChecked = true;
        if (GameState.ConsumeResume(GameState.CurrentLevel, Index))
        {
            Vector3 here = Self.Position;
            p.TeleportTo(new Vector3(here.X, here.Y + SpawnYOffset, here.Z));
            Light();
        }
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (_active || player == null || other.Id != player.Self.Id) return;
        Vector3 here = Self.Position;
        player.SetCheckpoint(new Vector3(here.X, here.Y + SpawnYOffset, here.Z));
        Light();
        if (GameState.CurrentLevel.Length > 0 && SaveSystem.Active != null)
        {
            SaveSystem.Active.RecordCheckpoint(GameState.CurrentLevel, Index);
            SaveSystem.SaveActive();
        }
        Log.Info($"[INKBOUND] Checkpoint {Index} reached!");
    }

    private void Light()
    {
        _active = true;
        SpriteRenderer.SetTint(Self, new Vector4(0.4f, 1.0f, 0.5f, 1.0f)); // lit green
    }
}

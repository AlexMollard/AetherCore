using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A checkpoint flag. The first time the player passes through, it becomes their
/// new respawn point (so a later hazard or pit doesn't send them all the way
/// back to the start) and lights up to show it's active.
/// </summary>
public sealed class Checkpoint : EntityScript
{
    /// <summary>Respawn slightly above the flag base so the player drops in cleanly.</summary>
    public float SpawnYOffset = 0.5f;

    private bool _active;

    public override void OnAttach()
    {
        Physics2D.EnableEvents(Self);
        // Dim until reached.
        SpriteRenderer.SetTint(Self, new Vector4(0.6f, 0.6f, 0.6f, 1.0f));
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (_active || player == null || other.Id != player.Self.Id)
        {
            return;
        }
        _active = true;
        Vector3 here = Self.Position;
        player.SetCheckpoint(new Vector3(here.X, here.Y + SpawnYOffset, here.Z));
        SpriteRenderer.SetTint(Self, new Vector4(0.4f, 1.0f, 0.5f, 1.0f)); // lit green
        Log.Info("[CoinDash] Checkpoint reached!");
    }
}

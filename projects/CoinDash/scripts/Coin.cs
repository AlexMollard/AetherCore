using AetherCore;

namespace AetherGame;

/// <summary>Spinning pickup: the player touching the trigger collects it.</summary>
public sealed class Coin : EntityScript
{
    public override void OnAttach()
    {
        Physics2D.EnableEvents(Self);
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (player == null || other.Id != player.Self.Id)
        {
            return;
        }
        GameState.CollectCoin();
        Self.Destroy();
    }
}

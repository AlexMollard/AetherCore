using AetherCore;

namespace AetherGame;

/// <summary>Level-end trigger at the flag pole.</summary>
public sealed class GoalFlag : EntityScript
{
    public override void OnAttach()
    {
        Physics2D.EnableEvents(Self);
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (player != null && other.Id == player.Self.Id)
        {
            GameState.Win();
        }
    }
}

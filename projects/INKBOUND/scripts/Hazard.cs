using AetherCore;

namespace AetherGame;

/// <summary>
/// A damage zone - spikes, lava, a saw. Touching it sends the player back to
/// their last checkpoint (or the level start), with a screen shake to sell the
/// hit. A trigger, so it never blocks movement - it just punishes contact.
/// </summary>
public sealed class Hazard : EntityScript
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
        Log.Info("[INKBOUND] Ouch - hazard!");
        player.Die();
    }
}

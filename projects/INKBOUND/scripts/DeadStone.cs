using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A stretch of rock that will not take ink. Nothing you draw inside it holds - the stroke comes out
/// red and crumbles, however solid the wall behind it looks.
///
/// This is the level's way of saying "not here" without building a wall. The route stays visible and
/// the geometry stays open, but you cannot simply bridge across it, so the puzzle becomes finding the
/// one place you ARE allowed to build from. Pair it with a conduit or a gate and you get problems
/// that are about planning a line rather than executing a jump.
/// </summary>
public sealed class DeadStone : EntityScript
{
    /// <summary>Half-size of the dead region around this entity, in world units.</summary>
    public float HalfWidth = 4.0f;
    public float HalfHeight = 3.0f;

    public override void OnAttach()
    {
        Vector3 p = Self.Position;
        AetherInk.AddDeadZone(Self.Id, new Vector4(p.X - HalfWidth, p.Y - HalfHeight, p.X + HalfWidth, p.Y + HalfHeight));
        Log.Info($"[INKBOUND] Dead stone at ({p.X:0.#}, {p.Y:0.#}) - ink will not set here.");
    }

    public override void OnDetach() => AetherInk.RemoveDeadZone(Self.Id);
}

using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Tiny inspector example: drag an entity into Target, then this entity follows
/// that target while playing.
/// </summary>
public sealed class EntityFollowerExample : EntityScript
{
    public Entity Target;
    public Vector3 Offset = new(0.0f, 2.0f, 0.0f);
    public float FollowStrength = 12.0f;

    public override void OnUpdate(float deltaTime)
    {
        if (!Target.IsValid || !Self.HasTransform || !Target.HasTransform)
        {
            return;
        }

        Vector3 desired = Target.Position + Offset;
        float t = FollowStrength * deltaTime;
        if (t < 0.0f) { t = 0.0f; }
        if (t > 1.0f) { t = 1.0f; }
        Self.Position = Vector3.Lerp(Self.Position, desired, t);
    }
}

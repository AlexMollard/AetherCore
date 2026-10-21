using System.Numerics;
using AetherCore;

public sealed class ModelProbe : EntityScript
{
    public override void OnUpdate(float deltaTime)
    {
        Vector3 position = Self.Position;
        position.X += deltaTime;
        Self.Position = position;
    }
}

using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Spins its entity about the Y axis. The first C# entity script - proves the
/// full round trip: native runner -> managed instance -> transform exports.
/// </summary>
public sealed class Spinner : EntityScript
{
    /// <summary>Degrees per second about Y.</summary>
    public float DegreesPerSecond = 90.0f;

    public override void OnUpdate(float deltaTime)
    {
        Vector3 euler = Self.EulerDegrees;
        euler.Y += DegreesPerSecond * deltaTime;
        if (euler.Y >= 360.0f)
        {
            euler.Y -= 360.0f;
        }
        Self.EulerDegrees = euler;
    }
}

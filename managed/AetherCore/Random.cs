using System.Numerics;

namespace AetherCore;

/// <summary>Random number helpers mirroring Unity's Random.</summary>
public static class Random
{
    private static readonly System.Random s_rng = new();

    /// <summary>A random float in [0, 1).</summary>
    public static float Value => (float)s_rng.NextDouble();

    /// <summary>A random float in [min, max).</summary>
    public static float Range(float min, float max) => min + (float)s_rng.NextDouble() * (max - min);

    /// <summary>A random int in [minInclusive, maxExclusive).</summary>
    public static int Range(int minInclusive, int maxExclusive) => s_rng.Next(minInclusive, maxExclusive);

    /// <summary>A random point inside the unit sphere.</summary>
    public static Vector3 InsideUnitSphere
    {
        get
        {
            Vector3 p;
            do
            {
                p = new Vector3(Range(-1.0f, 1.0f), Range(-1.0f, 1.0f), Range(-1.0f, 1.0f));
            }
            while (p.LengthSquared() > 1.0f);
            return p;
        }
    }

    /// <summary>A random unit-length direction.</summary>
    public static Vector3 OnUnitSphere
    {
        get
        {
            Vector3 p = InsideUnitSphere;
            float len = p.Length();
            return len > 1e-6f ? p / len : new Vector3(0.0f, 1.0f, 0.0f);
        }
    }
}

using System;

namespace AetherCore;

/// <summary>Float math helpers mirroring Unity's Mathf (thin over System.MathF).</summary>
public static class Mathf
{
    public const float PI = MathF.PI;
    public const float Deg2Rad = PI / 180.0f;
    public const float Rad2Deg = 180.0f / PI;
    public const float Epsilon = 1e-6f;

    public static float Clamp(float value, float min, float max) => value < min ? min : (value > max ? max : value);
    public static float Clamp01(float value) => Clamp(value, 0.0f, 1.0f);

    public static float Lerp(float a, float b, float t) => a + (b - a) * Clamp01(t);
    public static float LerpUnclamped(float a, float b, float t) => a + (b - a) * t;
    public static float InverseLerp(float a, float b, float value) => a == b ? 0.0f : Clamp01((value - a) / (b - a));

    public static float MoveTowards(float current, float target, float maxDelta)
        => MathF.Abs(target - current) <= maxDelta ? target : current + MathF.Sign(target - current) * maxDelta;

    public static float Repeat(float t, float length) => Clamp(t - MathF.Floor(t / length) * length, 0.0f, length);
    public static float PingPong(float t, float length)
    {
        t = Repeat(t, length * 2.0f);
        return length - MathF.Abs(t - length);
    }

    public static float Abs(float v) => MathF.Abs(v);
    public static float Sign(float v) => MathF.Sign(v);
    public static float Min(float a, float b) => MathF.Min(a, b);
    public static float Max(float a, float b) => MathF.Max(a, b);
    public static float Sqrt(float v) => MathF.Sqrt(v);
    public static float Pow(float b, float e) => MathF.Pow(b, e);
    public static float Sin(float x) => MathF.Sin(x);
    public static float Cos(float x) => MathF.Cos(x);
    public static float Atan2(float y, float x) => MathF.Atan2(y, x);
    public static float Floor(float v) => MathF.Floor(v);
    public static float Ceil(float v) => MathF.Ceiling(v);
    public static float Round(float v) => MathF.Round(v);
    public static bool Approximately(float a, float b) => MathF.Abs(b - a) < Epsilon * MathF.Max(1.0f, MathF.Max(MathF.Abs(a), MathF.Abs(b)));
}

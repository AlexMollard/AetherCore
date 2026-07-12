using System.Numerics;

namespace AetherCore;

/// <summary>
/// Development helpers: logging (forwards to the engine log) and immediate-mode
/// world-space debug drawing. Debug lines show when debug rendering is enabled
/// (like Unity's Debug.DrawLine with gizmos on).
/// </summary>
public static class Debug
{
    private static readonly Vector4 White = new(1.0f, 1.0f, 1.0f, 1.0f);

    // ── Logging ───────────────────────────────────────────────────────────────────
    public static void Log(string message) => AetherCore.Log.Info(message);
    public static void LogWarning(string message) => AetherCore.Log.Warn(message);
    public static void LogError(string message) => AetherCore.Log.Error(message);

    // ── Drawing (world space) ───────────────────────────────────────────────────────

    /// <summary>Master switch for debug drawing (off by default, like gizmos).</summary>
    public static bool DrawEnabled
    {
        get => Native.aether_debug_is_enabled() != 0;
        set => Native.aether_debug_set_enabled(value ? 1 : 0);
    }

    public static void DrawLine(Vector3 from, Vector3 to) => Native.aether_debug_draw_line(from, to, White);
    public static void DrawLine(Vector3 from, Vector3 to, Vector4 color) => Native.aether_debug_draw_line(from, to, color);

    public static void DrawRay(Vector3 origin, Vector3 direction, Vector4 color) => Native.aether_debug_draw_ray(origin, direction, color);

    public static void DrawSphere(Vector3 center, float radius, Vector4 color) => Native.aether_debug_draw_sphere(center, radius, color);

    /// <summary>Draw a wireframe box (axis-aligned) centred at <paramref name="center"/>.</summary>
    public static void DrawBox(Vector3 center, Vector3 halfExtents, Vector4 color) => Native.aether_debug_draw_box(center, halfExtents, color);
}

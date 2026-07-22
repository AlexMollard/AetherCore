namespace AetherCore;

/// <summary>
/// Play-time clock for scripts. The clock only advances while the game is
/// playing (paused/edit mode does not tick it), matching Unity's Time.
/// </summary>
public static class Time
{
    /// <summary>Seconds since the previous frame.</summary>
    public static float DeltaTime => Native.aether_time_delta();

    /// <summary>Seconds of play time elapsed since the scene started playing.</summary>
    public static float TotalTime => Native.aether_time_total();

    /// <summary>Real (wall-clock) seconds since play started, ignoring <see cref="Scale"/>. Keeps
    /// advancing while the game is frozen (scale 0), so pause-menu UI can still animate. This is the
    /// Unity Time.unscaledTime model.</summary>
    public static float UnscaledTime => Native.aether_time_unscaled();

    /// <summary>Number of frames rendered since play started.</summary>
    public static long FrameCount => Native.aether_time_frame_count();

    /// <summary>
    /// Global time scale (1 = normal, &lt;1 slow-mo, &gt;1 fast-forward, 0 = frozen).
    /// Setting 0 pauses the whole simulation - physics, particles and animation stop -
    /// while scripts keep ticking with <see cref="DeltaTime"/> == 0, so a pause menu can
    /// still read input to resume. This is the Unity Time.timeScale model.
    /// </summary>
    public static float Scale
    {
        get => Native.aether_time_get_scale();
        set => Native.aether_time_set_scale(value);
    }

    /// <summary>True while the game is frozen via <see cref="Pause"/> (scale ~ 0).</summary>
    public static bool IsPaused => Native.aether_time_get_scale() <= 0.0001f;

    /// <summary>Freeze the simulation (scale = 0). Scripts still tick; gameplay stops.</summary>
    public static void Pause() => Native.aether_time_set_scale(0.0f);

    /// <summary>Resume at normal speed (scale = 1).</summary>
    public static void Resume() => Native.aether_time_set_scale(1.0f);
}

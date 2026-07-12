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

    /// <summary>Number of frames rendered since play started.</summary>
    public static long FrameCount => Native.aether_time_frame_count();
}

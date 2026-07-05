using AetherCore.Managed.Interop;

namespace AetherCore.Managed;

/// <summary>Keyboard state for the current frame.</summary>
public static class Input
{
    public static bool IsKeyDown(Key key) => Native.aether_input_key_down((int)key) != 0;

    public static bool IsKeyPressed(Key key) => Native.aether_input_key_pressed((int)key) != 0;

    public static bool IsKeyReleased(Key key) => Native.aether_input_key_released((int)key) != 0;

    /// <summary>Seconds elapsed since the previous frame.</summary>
    public static float DeltaTime => Native.aether_input_delta_time();
}

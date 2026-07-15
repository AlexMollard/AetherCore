using System;
using System.Text;

namespace AetherCore;

public enum SpriteAnimationLoopMode
{
    Loop = 0,
    Once,
    PingPong,
    Hold,
}

public readonly record struct SpriteAnimationEvent(uint FrameIndex, string Name, string Payload);

public static class SpriteAnimator
{
    public static void SetAnimation(Entity entity, string path) => Native.aether_sprite_animator_set_animation(entity.Id, path);
    public static void Play(Entity entity) => Native.aether_sprite_animator_play(entity.Id);
    public static void Pause(Entity entity) => Native.aether_sprite_animator_pause(entity.Id);
    public static void Restart(Entity entity) => Native.aether_sprite_animator_restart(entity.Id);
    public static bool IsPlaying(Entity entity) => Native.aether_sprite_animator_is_playing(entity.Id) != 0;
    public static uint CurrentFrame(Entity entity) => Native.aether_sprite_animator_get_current_frame(entity.Id);
    public static float GetSpeed(Entity entity) => Native.aether_sprite_animator_get_speed(entity.Id);
    public static void SetSpeed(Entity entity, float value) => Native.aether_sprite_animator_set_speed(entity.Id, value);
    public static SpriteAnimationLoopMode GetLoopMode(Entity entity) => (SpriteAnimationLoopMode)Native.aether_sprite_animator_get_loop_mode(entity.Id);
    public static void SetLoopMode(Entity entity, SpriteAnimationLoopMode value) => Native.aether_sprite_animator_set_loop_mode(entity.Id, (int)value);

    public static unsafe bool TryPopEvent(Entity entity, out SpriteAnimationEvent animationEvent)
    {
        Span<byte> name = stackalloc byte[256];
        Span<byte> payload = stackalloc byte[512];
        uint frame = 0;
        fixed (byte* namePtr = name)
        fixed (byte* payloadPtr = payload)
        {
            if (Native.aether_sprite_animator_pop_event(entity.Id, namePtr, name.Length, payloadPtr, payload.Length, &frame) == 0)
            {
                animationEvent = default;
                return false;
            }
        }
        animationEvent = new SpriteAnimationEvent(frame, DecodeUtf8(name), DecodeUtf8(payload));
        return true;
    }

    private static string DecodeUtf8(ReadOnlySpan<byte> bytes)
    {
        int length = bytes.IndexOf((byte)0);
        return Encoding.UTF8.GetString(length >= 0 ? bytes[..length] : bytes);
    }
}

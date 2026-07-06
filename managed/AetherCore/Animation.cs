using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

/// <summary>
/// Skeletal animation control for entities with a skinned mesh. Clip indices span
/// the compiled database clips followed by pending (added-but-not-compiled) clips.
/// </summary>
public static unsafe class Animation
{
    /// <summary>
    /// Load a .anim clip and queue it as pending. Returns its future clip index
    /// (valid after <see cref="Compile"/>), or -1 on failure. When
    /// <paramref name="lockRoot"/> is true, root translation is stripped (walk in place).
    /// </summary>
    public static int AddClip(Entity entity, string animPath, bool lockRoot = false)
        => Native.aether_anim_add(entity.Id, animPath, lockRoot ? 1 : 0);

    /// <summary>Backwards-compatible alias for <see cref="AddClip"/> without root lock.</summary>
    public static int LoadExternal(Entity entity, string animPath) => Native.aether_anim_load_external(entity.Id, animPath);

    /// <summary>Bake all pending clips into the entity's animation database.</summary>
    public static void Compile(Entity entity) => Native.aether_anim_compile(entity.Id);

    public static void ClearPending(Entity entity) => Native.aether_anim_clear_pending(entity.Id);

    public static void SetClip(Entity entity, int clipIndex) => Native.aether_anim_set_clip(entity.Id, clipIndex);

    public static int CurrentClip(Entity entity) => Native.aether_anim_get_current(entity.Id);

    public static void SetPlaybackSpeed(Entity entity, float speed) => Native.aether_anim_set_playback_speed(entity.Id, speed);

    public static float GetPlaybackSpeed(Entity entity) => Native.aether_anim_get_playback_speed(entity.Id);

    public static void SetTime(Entity entity, float seconds) => Native.aether_anim_set_time(entity.Id, seconds);

    public static float GetTime(Entity entity) => Native.aether_anim_get_time(entity.Id);

    public static int ClipCount(Entity entity) => Native.aether_anim_get_count(entity.Id);

    public static string ClipName(Entity entity, int index)
    {
        Span<byte> buffer = stackalloc byte[128];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_anim_get_name(entity.Id, index, ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }

    /// <summary>Duration of the current clip, in seconds.</summary>
    public static float ClipDuration(Entity entity) => Native.aether_anim_get_duration(entity.Id);

    /// <summary>Find a clip index by name, or -1 if not found.</summary>
    public static int Find(Entity entity, string name) => Native.aether_anim_find(entity.Id, name);

    /// <summary>Cross-fade to a secondary clip. Default transition speed if &lt;= 0.</summary>
    public static void SetBlend(Entity entity, int secondaryClipIndex, float transitionSpeed = 0f)
        => Native.aether_anim_set_blend(entity.Id, secondaryClipIndex, transitionSpeed);

    public static void SetRootMotionEnabled(Entity entity, bool enabled)
        => Native.aether_anim_set_root_motion_enabled(entity.Id, enabled ? 1 : 0);

    public static bool GetRootMotionEnabled(Entity entity) => Native.aether_anim_get_root_motion_enabled(entity.Id) != 0;

    public static Vector3 GetRootMotionDelta(Entity entity) => Native.aether_anim_get_root_motion_delta(entity.Id);

    public static int GetEntitiesWithAnimator(Span<Entity> buffer)
    {
        if (buffer.IsEmpty)
        {
            return 0;
        }
        fixed (Entity* ptr = buffer)
        {
            return Native.aether_anim_get_entities_with_animator((uint*)ptr, buffer.Length);
        }
    }
}

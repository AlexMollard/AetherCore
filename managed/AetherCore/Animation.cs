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

    /// <summary>
    /// Start <paramref name="clipIndex"/> from time 0 while the clip playing now keeps running and
    /// fades out over <paramref name="seconds"/>. seconds &lt;= 0 switches outright, like SetClip.
    /// </summary>
    public static void CrossFade(Entity entity, int clipIndex, float seconds)
        => Native.aether_anim_crossfade(entity.Id, clipIndex, seconds);

    /// <summary>
    /// Layer <paramref name="clipIndex"/> at full weight over the clip playing now: the layer
    /// overwrites exactly the nodes it animates and the base clip keeps posing the rest. Intended
    /// for partial clips (a clip that animates only a few joints). A negative index clears the
    /// layer. A later CrossFade/SetClip also clears it.
    /// </summary>
    public static void SetLayerClip(Entity entity, int clipIndex)
        => Native.aether_anim_set_layer_clip(entity.Id, clipIndex);

    /// <summary>
    /// NOT IMPLEMENTED. Pose sampling runs on the GPU and nothing reads the hips back, so no
    /// delta is ever accumulated: this flag gates nothing and <see cref="GetRootMotionDelta"/>
    /// always returns zero. Move a character from script instead.
    /// </summary>
    public static void SetRootMotionEnabled(Entity entity, bool enabled)
        => Native.aether_anim_set_root_motion_enabled(entity.Id, enabled ? 1 : 0);

    /// <summary>NOT IMPLEMENTED - see <see cref="SetRootMotionEnabled"/>.</summary>
    public static bool GetRootMotionEnabled(Entity entity) => Native.aether_anim_get_root_motion_enabled(entity.Id) != 0;

    /// <summary>
    /// NOT IMPLEMENTED: always returns zero. Nothing computes root motion, so a character
    /// driven from this will simply never move. The engine warns once if you call it.
    /// </summary>
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

using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// One piece of machinery two different visuals share: orienting a stretched, unit
/// cube mesh so it forms a segment between two world points. WireLink's single wire
/// segment and Beam's N-segment curved beam both need exactly this - kept here once
/// instead of copied twice, the same "small table, not duplicated per caller" reasoning
/// WireHub's own file comment gives for device roles.
///
/// Derived directly from TransformUtils.hpp's own ComposeTransform (R = Ry(yaw) *
/// Rx(pitch) * Rz(roll)) rather than a hand-rolled guess - at yaw=pitch=0, local +Z maps
/// to world +Z; solving that same composition for an arbitrary target direction gives
/// yaw = atan2(dx, dz), pitch = -asin(dy). Roll is left at 0 - a round-in-cross-section
/// segment reads identically at any roll, so there is nothing to solve for on that axis.
/// This is WireLink's own original derivation, moved here unchanged when Beam needed the
/// same math a second time.
/// </summary>
public static class SegmentVisual
{
    /// <summary>Repositions, rescales and reorients `segment` so its local +Z axis runs
    /// from `from` to `to`, `thickness` wide on the other two axes. A `to`-`from`
    /// distance under 1mm is treated as degenerate and left untouched, so a not-yet-
    /// resolved pair of endpoints renders nothing rather than a zero-length or
    /// NaN-scaled box.</summary>
    public static void Orient(Entity segment, Vector3 from, Vector3 to, float thickness)
    {
        Vector3 delta = to - from;
        float length = delta.Length();
        if (length < 0.001f)
        {
            return;
        }
        Vector3 direction = delta / length;
        float yaw = MathF.Atan2(direction.X, direction.Z) * Mathf.Rad2Deg;
        float pitch = -MathF.Asin(Math.Clamp(direction.Y, -1.0f, 1.0f)) * Mathf.Rad2Deg;
        Vector3 midpoint = (from + to) * 0.5f;
        segment.SetTransform(midpoint, new Vector3(pitch, yaw, 0.0f), new Vector3(thickness, thickness, length));
    }
}

using System;
using System.Numerics;

namespace AetherCore;

/// <summary>Where a custom render pass is injected into the frame graph. Both stages draw into the
/// scene HDR colour, before tonemap.</summary>
public enum CustomPassStage
{
    /// <summary>Before sprites/tiles - drawn behind the 2D scene.</summary>
    BehindScene = 0,
    /// <summary>After sprites/tiles, before the 2D light map - so the pass is lit and shadowed like
    /// the rest of the scene.</summary>
    OverScene = 1,
    /// <summary>After the 2D light map - the pass keeps its own brightness instead of being darkened
    /// by lighting. For things that emit light (a glowing stroke, a spell). Pair with
    /// <see cref="Lighting2D"/> submissions so it still lights and shadows the world around it.</summary>
    EmissiveOverLight = 2,
}

/// <summary>
/// Project hook for custom, script-driven render passes. A project registers a pass by name once
/// (its project shader + where in the graph it runs), then each frame submits a <see cref="Vector4"/>
/// data buffer plus params/colours; the engine runs the named shader over a fullscreen pass at that
/// stage with the buffer bound. The engine is entirely agnostic to what the pass draws - the geometry
/// packing and the look live wholly in the project's shader and scripts.
///
/// The shader receives a fixed push contract: frame constants (viewProj / invViewProj / time), the
/// bound float4 buffer + its element count, the viewport size, and params/color0/color1.
/// </summary>
public static class CustomPass
{
    /// <summary>Register a pass. <paramref name="shader"/> is a project shader stem
    /// (shaders://&lt;shader&gt;.spv). Call once (e.g. in OnAttach).</summary>
    public static void Register(string name, string shader, CustomPassStage stage)
        => Native.aether_custompass_register(name, shader, (int)stage);

    /// <summary>Stop drawing a registered pass. Call when the owner is torn down (OnDetach).</summary>
    public static void Unregister(string name) => Native.aether_custompass_unregister(name);

    /// <summary>Submit this frame's data + push params for a registered pass. Re-submit every frame
    /// the pass should draw; skip a frame to not draw it. <paramref name="data"/> is a raw float4
    /// buffer the pass's shader interprets.</summary>
    public static unsafe void Submit(string name, ReadOnlySpan<Vector4> data, Vector4 parameters, Vector4 color0, Vector4 color1)
    {
        fixed (Vector4* ptr = data)
        {
            Native.aether_custompass_submit(name, ptr, data.Length, parameters, color0, color1);
        }
    }
}

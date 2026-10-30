using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

public enum StreamingPriority : uint { Critical, High, Normal, Background }
public enum StreamingState : uint { Unknown, Queued, Loading, Uploading, Loaded, Active, Saving, Retiring, Cancelled, Failed, Blocked }

/// <summary>Controls region demand. Requests remain claimed until cancelled or released.</summary>
public static class Streaming
{
    public static bool SetSource(string id, Vector3 position, float loadRadius, float prefetchRadius, StreamingPriority priority = StreamingPriority.Normal)
        => Native.aether_streaming_set_source(id, position, loadRadius, prefetchRadius, (uint)priority);
    public static bool RemoveSource(string id) => Native.aether_streaming_remove_source(id);
    public static ulong Prefetch(string region, StreamingPriority priority = StreamingPriority.Normal)
        => Native.aether_streaming_request(region, (uint)priority, false);
    public static ulong Load(string region, StreamingPriority priority = StreamingPriority.Normal)
        => Native.aether_streaming_request(region, (uint)priority, true);
    public static bool Cancel(ulong request) => Native.aether_streaming_cancel(request);
    public static bool Release(ulong request) => Native.aether_streaming_release(request);
    public static StreamingState GetState(ulong request) => (StreamingState)Native.aether_streaming_get_state(request);
    public static unsafe string GetDiagnostics()
    {
        uint required = Native.aether_streaming_diagnostics(null, 0);
        while (required > 0 && required <= int.MaxValue)
        {
            byte[] bytes = new byte[(int)required];
            fixed (byte* buffer = bytes)
            {
                uint actual = Native.aether_streaming_diagnostics(buffer, required);
                if (actual == 0) return string.Empty;
                if (actual <= required) return Encoding.UTF8.GetString(bytes, 0, (int)actual - 1);
                required = actual;
            }
        }
        return string.Empty;
    }
}

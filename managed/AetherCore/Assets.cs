using System;
using System.Text;

namespace AetherCore;

/// <summary>Generic data-asset access. Reads through the VFS: the raw project directory
/// in-editor, the packed .pak when shipped. Not tied to any game.</summary>
public static class Assets
{
    /// <summary>Read a data asset as UTF-8 text (e.g. "project://dialogue/intro.json").
    /// Returns null if the asset does not exist.</summary>
    public static unsafe string? ReadText(string virtualPath)
    {
        // Fast path: most data assets fit the stack buffer in a single call. The native side
        // returns the FULL byte length even when it exceeds the buffer, so a larger asset falls
        // through to an exact-size heap read.
        Span<byte> buffer = stackalloc byte[4096];
        int full;
        fixed (byte* ptr = buffer)
        {
            full = Native.aether_assets_read_text(virtualPath, ptr, buffer.Length);
            if (full < 0)
            {
                return null; // missing / unreadable
            }
            if (full <= buffer.Length)
            {
                return Encoding.UTF8.GetString(ptr, full);
            }
        }

        byte[] heap = new byte[full];
        fixed (byte* ptr = heap)
        {
            int n = Native.aether_assets_read_text(virtualPath, ptr, heap.Length);
            return n > 0 ? Encoding.UTF8.GetString(ptr, Math.Min(n, heap.Length)) : string.Empty;
        }
    }
}

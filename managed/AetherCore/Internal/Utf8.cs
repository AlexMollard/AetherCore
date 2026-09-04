using System;
using System.Runtime.InteropServices;
using System.Text;

namespace AetherCore;

/// <summary>UTF-8 marshalling helpers for the native boundary.</summary>
internal static unsafe class Utf8
{
    public static string ToString(byte* ptr)
    {
        return ptr == null ? string.Empty : Marshal.PtrToStringUTF8((IntPtr)ptr) ?? string.Empty;
    }

    /// <summary>
    /// Writes <paramref name="value"/> as null-terminated UTF-8 into the caller's
    /// buffer, truncating if needed. Returns bytes written (excluding the null).
    /// </summary>
    public static int Write(string value, byte* buf, int bufLen)
    {
        if (buf == null || bufLen <= 0)
        {
            return 0;
        }
        // Encode straight into the caller's buffer when the value fits (the
        // common case: names and short property values) so the write allocates
        // nothing; only a genuinely truncated write pays for the full encoding.
        int byteCount = Encoding.UTF8.GetByteCount(value);
        if (byteCount < bufLen)
        {
            int n = Encoding.UTF8.GetBytes(value, new Span<byte>(buf, byteCount));
            buf[n] = 0;
            return n;
        }
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        int t = Math.Min(bytes.Length, bufLen - 1);
        new Span<byte>(bytes, 0, t).CopyTo(new Span<byte>(buf, t));
        buf[t] = 0;
        return t;
    }
}

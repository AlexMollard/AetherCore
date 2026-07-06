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
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        int n = Math.Min(bytes.Length, bufLen - 1);
        new Span<byte>(bytes, 0, n).CopyTo(new Span<byte>(buf, n));
        buf[n] = 0;
        return n;
    }
}

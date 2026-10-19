using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace AetherCore;

/// <summary>Project-local UTF-8 records shared by the C# and Rust gameplay hosts.</summary>
public static class SaveData
{
    private static readonly UTF8Encoding Encoding = new(false, true);

    private static void ValidateKey(string key)
    {
        ArgumentNullException.ThrowIfNull(key);
        if (key.Length is 0 or > 128)
            throw new ArgumentException("Save keys must contain 1 to 128 ASCII letters, digits, underscores or hyphens.", nameof(key));
        foreach (char c in key)
            if (!(c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '_' or '-'))
                throw new ArgumentException("Save keys must contain only ASCII letters, digits, underscores or hyphens.", nameof(key));
    }

    /// <summary>Returns false for a missing record or when the run isolates prior saves.</summary>
    public static unsafe bool TryRead(string key, out string value)
    {
        ValidateKey(key);
        value = string.Empty;
        int required = Native.aether_save_read(key, null, 0);
        for (int attempt = 0; attempt < 3; attempt++)
        {
            if (required == -1) return false;
            if (required <= 0) throw new IOException("Could not read gameplay save.");
            byte[] buffer = new byte[required];
            fixed (byte* pointer = buffer)
            {
                int actual = Native.aether_save_read(key, pointer, buffer.Length);
                if (actual == -1) return false;
                if (actual <= 0) throw new IOException("Could not read gameplay save.");
                if (actual <= buffer.Length)
                {
                    value = Encoding.GetString(buffer, 0, actual - 1);
                    return true;
                }
                required = actual;
            }
        }
        throw new IOException("Gameplay save kept changing during the read.");
    }

    public static unsafe void Write(string key, string value)
    {
        ValidateKey(key);
        ArgumentNullException.ThrowIfNull(value);
        byte[] bytes = Encoding.GetBytes(value);
        fixed (byte* pointer = bytes)
            if (Native.aether_save_write(key, pointer, bytes.Length) != 1)
                throw new IOException("Could not write gameplay save.");
    }
}

internal static unsafe partial class Native
{
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_save_read(string key, byte* output, int capacity);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_save_write(string key, byte* value, int length);
}

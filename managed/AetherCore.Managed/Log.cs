using System;
using System.Buffers;
using System.Text;
using AetherCore.Managed.Interop;

namespace AetherCore.Managed;

/// <summary>
/// Engine log, routed to the native <c>aether::Logger</c> via the host callback
/// table. Levels match <c>aether::LogLevel</c> (Verbose=0, Info=1, Warn=2, Error=3).
/// </summary>
public static unsafe class Log
{
    public static void Info(string message) => Emit(1, message);

    public static void Warn(string message) => Emit(2, message);

    public static void Error(string message) => Emit(3, message);

    private static void Emit(int level, string message)
    {
        delegate* unmanaged<int, byte*, void> callback = Bootstrap.Host.Log;
        if (callback == null)
        {
            return;
        }

        int byteCount = Encoding.UTF8.GetByteCount(message);
        byte[] buffer = ArrayPool<byte>.Shared.Rent(byteCount + 1);
        try
        {
            int written = Encoding.UTF8.GetBytes(message, buffer);
            buffer[written] = 0; // null-terminate for the C++ side
            fixed (byte* ptr = buffer)
            {
                callback(level, ptr);
            }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer);
        }
    }
}

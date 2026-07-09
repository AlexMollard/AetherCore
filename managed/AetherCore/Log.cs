using System;
using System.Buffers;
using System.Runtime.CompilerServices;
using System.Text;

namespace AetherCore;

/// <summary>
/// Engine log, routed to the native <c>aether::Logger</c> via the host callback
/// table. Levels match <c>aether::LogLevel</c> (Verbose=0, Info=1, Warn=2, Error=3).
/// </summary>
public static unsafe class Log
{
    public static void Info(string message, [CallerFilePath] string filePath = "", [CallerLineNumber] int line = 0) => Emit(1, message, filePath, line);

    public static void Warn(string message, [CallerFilePath] string filePath = "", [CallerLineNumber] int line = 0) => Emit(2, message, filePath, line);

    public static void Error(string message, [CallerFilePath] string filePath = "", [CallerLineNumber] int line = 0) => Emit(3, message, filePath, line);

    private static void Emit(int level, string message, string filePath, int line)
    {
        message ??= string.Empty;
        filePath ??= string.Empty;

        delegate* unmanaged<int, byte*, byte*, int, void> sourceCallback = HostBridge.LogAtSource;
        if (sourceCallback != null)
        {
            EmitAtSource(level, message, filePath, line, sourceCallback);
            return;
        }

        EmitLegacy(level, message);
    }

    private static void EmitAtSource(int level, string message, string filePath, int line, delegate* unmanaged<int, byte*, byte*, int, void> callback)
    {
        int messageByteCount = Encoding.UTF8.GetByteCount(message);
        int fileByteCount = Encoding.UTF8.GetByteCount(filePath);
        byte[] messageBuffer = ArrayPool<byte>.Shared.Rent(messageByteCount + 1);
        byte[] fileBuffer = ArrayPool<byte>.Shared.Rent(fileByteCount + 1);
        try
        {
            int messageWritten = Encoding.UTF8.GetBytes(message, messageBuffer);
            int fileWritten = Encoding.UTF8.GetBytes(filePath, fileBuffer);
            messageBuffer[messageWritten] = 0; // null-terminate for the C++ side
            fileBuffer[fileWritten] = 0;
            fixed (byte* messagePtr = messageBuffer)
            fixed (byte* filePtr = fileBuffer)
            {
                callback(level, messagePtr, filePtr, line);
            }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(fileBuffer);
            ArrayPool<byte>.Shared.Return(messageBuffer);
        }
    }

    private static void EmitLegacy(int level, string message)
    {
        delegate* unmanaged<int, byte*, void> callback = HostBridge.Log;
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

using System;
using System.Runtime;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace AetherCore.Managed.Interop;

/// <summary>
/// The single managed entry point the native host binds via
/// load_assembly_and_get_function_pointer. <see cref="Init"/> performs the ABI
/// handshake: it receives the host callback table, fills the managed API table,
/// and returns 0 on success.
/// </summary>
internal static unsafe class Bootstrap
{
    /// <summary>Host callbacks (log, error reporting). Populated by <see cref="Init"/>.</summary>
    internal static NativeHostCallbacks Host;

    /// <summary>
    /// ABI handshake. Return codes: 0 = success, 1 = null argument,
    /// 2 = struct-size mismatch, 3 = managed exception during init.
    /// </summary>
    [UnmanagedCallersOnly]
    internal static int Init(NativeHostCallbacks* callbacks, int callbacksSize, ManagedScriptApi* outApi, int apiSize)
    {
        if (callbacks == null || outApi == null)
        {
            return 1;
        }

        // Reject a host compiled against a different struct layout before we
        // dereference anything past the shared prefix.
        if (callbacksSize != sizeof(NativeHostCallbacks) || apiSize != sizeof(ManagedScriptApi))
        {
            return 2;
        }

        Host = *callbacks;

        try
        {
            *outApi = default;
            outApi->SetPlayMode = &Api_SetPlayMode;
            outApi->CollectFull = &Api_CollectFull;
            // Registry, instance-lifecycle, and property entries are wired in Phase 1.

            Log.Info($"AetherCore.Managed bootstrap OK (.NET {Environment.Version})");
            return 0;
        }
        catch (Exception ex)
        {
            ReportError(ex.ToString());
            return 3;
        }
    }

    /// <summary>Forwards a message to the host's script-error UI (falls back to log).</summary>
    internal static void ReportError(string message)
    {
        delegate* unmanaged<byte*, void> callback = Host.ReportScriptError;
        if (callback == null)
        {
            Log.Error(message);
            return;
        }

        int byteCount = System.Text.Encoding.UTF8.GetByteCount(message);
        Span<byte> buffer = byteCount < 512 ? stackalloc byte[byteCount + 1] : new byte[byteCount + 1];
        int written = System.Text.Encoding.UTF8.GetBytes(message, buffer);
        buffer[written] = 0;
        fixed (byte* ptr = buffer)
        {
            callback(ptr);
        }
    }

    [UnmanagedCallersOnly]
    private static void Api_SetPlayMode(int playing)
    {
        try
        {
            GCSettings.LatencyMode = playing != 0 ? GCLatencyMode.SustainedLowLatency : GCLatencyMode.Interactive;
        }
        catch
        {
            // Latency mode is best-effort; some GC configs reject it.
        }
    }

    [UnmanagedCallersOnly]
    private static void Api_CollectFull()
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
    }
}

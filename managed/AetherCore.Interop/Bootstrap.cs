using System;
using System.Runtime;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using AetherCore;

namespace AetherCore.Interop;

/// <summary>
/// The single managed entry point the native host binds via
/// load_assembly_and_get_function_pointer. <see cref="Init"/> performs the ABI
/// handshake: it receives the host callback table, fills the managed API table,
/// and returns 0 on success.
/// </summary>
internal static unsafe class Bootstrap
{
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

        // Publish the host callbacks into the SDK's HostBridge so game-facing
        // types (Log) and this ABI layer share one source of truth.
        HostBridge.Log = callbacks->Log;
        HostBridge.LogAtSource = callbacks->LogAtSource;
        HostBridge.ReportScriptError = callbacks->ReportScriptError;

        try
        {
            // Point the SDK's "AetherHost" P/Invokes at the running executable
            // before any script runs. Native is an SDK internal, reachable here
            // via InternalsVisibleTo.
            Native.RegisterResolver();

            *outApi = default;

            // Assembly / registry lifecycle
            outApi->LoadScripts = &ScriptRegistry.LoadScripts;
            outApi->UnloadScripts = &ScriptRegistry.UnloadScripts;
            outApi->GetScriptTypeCount = &ScriptRegistry.GetScriptTypeCount;
            outApi->GetScriptTypeName = &ScriptRegistry.GetScriptTypeName;

            // Per-entity instance lifecycle
            outApi->CreateInstance = &ScriptRegistry.CreateInstance;
            outApi->DestroyInstance = &ScriptRegistry.DestroyInstance;
            outApi->InvokeAttach = &ScriptRegistry.InvokeAttach;
            outApi->InvokeUpdate = &ScriptRegistry.InvokeUpdate;
            outApi->InvokeDetach = &ScriptRegistry.InvokeDetach;

            // Serialized script properties (inspector / scene overrides)
            outApi->GetPropertyCount = &ScriptRegistry.GetPropertyCount;
            outApi->GetPropertyInfo = &ScriptRegistry.GetPropertyInfo;
            outApi->GetProperty = &ScriptRegistry.GetProperty;
            outApi->SetProperty = &ScriptRegistry.SetProperty;

            outApi->GetDefaultProperty = &ScriptRegistry.GetDefaultProperty;

            // GC policy
            outApi->SetPlayMode = &Api_SetPlayMode;
            outApi->CollectFull = &Api_CollectFull;

            // Editor tooling
            outApi->DrawEditorWindows = &ScriptRegistry.DrawEditorWindows;
            outApi->GetEditorWindowCount = &ScriptRegistry.GetEditorWindowCount;
            outApi->GetEditorWindowTitle = &ScriptRegistry.GetEditorWindowTitle;
            outApi->GetEditorWindowVisible = &ScriptRegistry.GetEditorWindowVisible;
            outApi->SetEditorWindowVisible = &ScriptRegistry.SetEditorWindowVisible;

            // Networking
            outApi->GetReplicatedPropertyIndices = &ScriptRegistry.GetReplicatedPropertyIndices;
            outApi->GetNetRpcMethodIndex = &ScriptRegistry.GetNetRpcMethodIndex;
            outApi->InvokeNetRpc = &ScriptRegistry.InvokeNetRpc;

            Log.Info($"AetherCore bootstrap OK (.NET {Environment.Version})");
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
        delegate* unmanaged<byte*, void> callback = HostBridge.ReportScriptError;
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

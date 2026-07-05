using System;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace AetherCore.Managed.Interop;

/// <summary>
/// Source-generated P/Invokes into the engine's C exports. "AetherHost" resolves
/// to the running executable (App.exe), which exports the aether_* functions —
/// no separate native DLL is loaded.
/// </summary>
internal static unsafe partial class Native
{
    private const string Lib = "AetherHost";

    // Called once from Bootstrap.Init (before any script runs, hence before the
    // first P/Invoke) to point "AetherHost" at the running executable.
    internal static void RegisterResolver()
    {
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly,
            static (name, _, _) => name == Lib ? NativeLibrary.GetMainProgramHandle() : IntPtr.Zero);
    }

    // ── World / entity / transform ────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial uint aether_entity_create();

    [LibraryImport(Lib)]
    internal static partial void aether_entity_destroy(uint id);

    [LibraryImport(Lib)]
    internal static partial int aether_entity_valid(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_mark_transient(uint id);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_set_name(uint id, string name);

    [LibraryImport(Lib)]
    internal static partial int aether_get_name(uint id, byte* buf, int bufLen);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_get_position(uint id);

    // Setters do subtree propagation + physics teleport, so no SuppressGCTransition
    // (that is reserved for trivial, non-blocking leaf calls).
    [LibraryImport(Lib)]
    internal static partial void aether_set_position(uint id, Vector3 pos);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_get_euler(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_set_euler(uint id, Vector3 euler);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_get_scale(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_set_transform(uint id, Vector3 pos, Vector3 euler, Vector3 scale);

    // ── Input ─────────────────────────────────────────────────────────────────
    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_input_key_down(int keyCode);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_input_key_pressed(int keyCode);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_input_key_released(int keyCode);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial float aether_input_delta_time();
}

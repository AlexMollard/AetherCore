using System.Runtime.InteropServices;

namespace AetherCore.Interop;

// Managed mirrors of the native structs in src/engine/scripting/ManagedInterop.hpp.
// Field order, count, and sizes must match exactly - Bootstrap.Init validates the
// struct sizes against the values the host passes in.

/// <summary>Wire type tag for a serialized script property. Mirrors native PropertyType.</summary>
internal enum PropertyType
{
    None = 0,
    Float = 1,
    Int = 2,
    Bool = 3,
    Vector3 = 4,
    String = 5,
    Enum = 6,
    Entity = 7,
    Component = 8,
}

/// <summary>Blittable tagged union for one script property crossing the boundary.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct PropertyValue
{
    public int Type; // PropertyType
    public int Reserved;
    public fixed float F4[4]; // Float (x), Vector3 (xyz)
    public long I64; // Int, Bool (0/1), Enum, Entity id
    public byte* Str; // String (UTF-8, caller-owned)
}

/// <summary>Callbacks the native host exposes to managed code.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeHostCallbacks
{
    public delegate* unmanaged<int, byte*, void> Log;
    public delegate* unmanaged<int, byte*, byte*, int, void> LogAtSource;
    public delegate* unmanaged<byte*, void> ReportScriptError;
}

/// <summary>
/// The managed script runtime API. Bootstrap.Init fills this and hands it back to
/// the host. Entries a phase has not implemented are left null.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ManagedScriptApi
{
    // Assembly / registry lifecycle
    public delegate* unmanaged<byte*, int> LoadScripts;
    public delegate* unmanaged<void> UnloadScripts;
    public delegate* unmanaged<int> GetScriptTypeCount;
    public delegate* unmanaged<int, byte*, int, int> GetScriptTypeName;

    // Per-entity instance lifecycle
    public delegate* unmanaged<byte*, uint, ulong> CreateInstance;
    public delegate* unmanaged<ulong, void> DestroyInstance;
    public delegate* unmanaged<ulong, void> InvokeAttach;
    public delegate* unmanaged<ulong, float, void> InvokeUpdate;
    public delegate* unmanaged<ulong, void> InvokeDetach;

    // Serialized script properties
    public delegate* unmanaged<byte*, int> GetPropertyCount;
    public delegate* unmanaged<byte*, int, byte*, int, int*, int> GetPropertyInfo;
    public delegate* unmanaged<ulong, int, PropertyValue*, int> GetProperty;
    public delegate* unmanaged<ulong, int, PropertyValue*, int> SetProperty;

    // GC policy
    public delegate* unmanaged<int, void> SetPlayMode;
    public delegate* unmanaged<void> CollectFull;

    // Default field value from a cached default instance (inspector, edit mode).
    public delegate* unmanaged<byte*, int, PropertyValue*, int> GetDefaultProperty;

    // Editor tooling (editor-only; never called from a shipped GameRuntime).
    public delegate* unmanaged<void> DrawEditorWindows;

    // Project editor-window registry: enumerate + toggle IEditorWindow tools from the editor menu.
    public delegate* unmanaged<int> GetEditorWindowCount;
    public delegate* unmanaged<int, byte*, int, int> GetEditorWindowTitle;
    public delegate* unmanaged<int, int> GetEditorWindowVisible;
    public delegate* unmanaged<int, int, void> SetEditorWindowVisible;

    // Networking: which of a type's properties carry [Replicated], as indices into
    // the property table above. Values still travel through GetProperty/SetProperty.
    public delegate* unmanaged<byte*, int*, int, int> GetReplicatedPropertyIndices;

    // Networking: RPCs. GetNetRpcMethod resolves a [NetRpc] method's wire index by
    // name and writes its declared NetRpcTarget to the out parameter (encode side);
    // InvokeNetRpc runs method `methodIndex` on instance `handle` (decode side).
    // argBlob is a single value - null/empty, or a UTF-8 string for a
    // one-string-parameter method.
    public delegate* unmanaged<byte*, byte*, int*, int> GetNetRpcMethod;
    public delegate* unmanaged<ulong, int, byte*, int, void> InvokeNetRpc;

    // Compile a project's scripts in-process. Returns 0 on success and fills the diagnostics
    // buffer with compiler output; see ScriptCompiler.cs for why this is not `dotnet build`.
    public delegate* unmanaged<byte*, byte*, byte*, int, byte*, int, int> CompileScripts;
}

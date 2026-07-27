using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using AetherCore;

namespace AetherCore.Interop;

/// <summary>
/// Loads the game-scripts assembly into a collectible load context, discovers
/// EntityScript types, and manages per-entity script instances. All the
/// [UnmanagedCallersOnly] members here are wired into ManagedScriptApi by
/// Bootstrap.Init and called by the native ScriptComponentSystem.
///
/// Instances are pinned by GCHandle; the native side holds the handle as a u64
/// and hands it back for every lifecycle call. Handles must be freed (via
/// DestroyInstance / UnloadScripts) before the ALC can unload.
/// </summary>
internal static unsafe class ScriptRegistry
{
    // The engine API assembly (this one), used to bind script references back to
    // the exact AetherCore the host loaded - same type identity, and it
    // resolves regardless of which load context the host placed it in.
    private static readonly Assembly EngineAssembly = typeof(EntityScript).Assembly;

    private sealed class ScriptsLoadContext : AssemblyLoadContext
    {
        // Resolves the game assembly's NuGet dependencies from its .deps.json so
        // they load into - and unload with - this collectible context.
        private readonly AssemblyDependencyResolver _resolver;

        public ScriptsLoadContext(string mainAssemblyPath)
            : base(name: "AetherGame", isCollectible: true)
        {
            _resolver = new AssemblyDependencyResolver(mainAssemblyPath);
        }

        protected override Assembly? Load(AssemblyName name)
        {
            // Share the engine SDK by identity: return the host's already-loaded
            // AetherCore so EntityScript et al. have the same Type across ALCs.
            if (name.Name == "AetherCore")
            {
                return EngineAssembly;
            }

            // Everything else (game NuGet deps) resolves from the game's deps.json.
            string? path = _resolver.ResolveAssemblyToPath(name);
            return path != null ? LoadFromAssemblyPath(path) : null;
        }
    }

    private static ScriptsLoadContext? s_context;
    private static readonly Dictionary<string, Type> s_types = new(StringComparer.Ordinal);
    private static string[] s_typeNames = Array.Empty<string>();

    // One reflected property table per script type (built once at load).
    private sealed class Prop
    {
        public required string Name;
        public required PropertyType Type;
        public required System.Reflection.FieldInfo Field;
        // For PropertyType.Component: the ComponentCatalog name of the required
        // component (from the IComponentRef wrapper's static ComponentType). The
        // inspector reads it (via the value's Str) to validate entity drops.
        public string? ComponentType;

        /// <summary>Field carries [Replicated]; the network layer syncs it host to client.</summary>
        public bool Replicated;
    }

    private static readonly Dictionary<string, Prop[]> s_props = new(StringComparer.Ordinal);

    // The indices (into s_props[type]) of that type's [Replicated] fields, in
    // property-table order. Built once alongside s_props so the network path never
    // re-reflects, and expressed as indices so replication reuses the existing
    // GetProperty/SetProperty bridge rather than adding a second value encoding.
    private static readonly Dictionary<string, int[]> s_replicated = new(StringComparer.Ordinal);

    // One [NetRpc] method table per script type (built once at load, alongside
    // s_props/s_replicated so the three tables cannot drift). The array index is
    // what travels on the wire (NetRpc.hpp's methodIndex), in declaration order.
    private static readonly Dictionary<string, MethodInfo[]> s_rpcMethods = new(StringComparer.Ordinal);

    // A default-constructed instance per type, so the inspector can show default
    // field values when no live instance exists (edit mode).
    // Editor-tooling windows discovered from the project assembly (IEditorWindow implementers).
    private static readonly List<IEditorWindow> s_editorWindows = new();
    // Open/closed state remembered by window Title across a C# reload. The windows are re-instantiated
    // on every reload (which resets their Visible field to its default), so without this a closed tool
    // window re-opens on every scene load / hot compile. Lives in the stable boot assembly, keyed by a
    // plain string so it never roots the collectible script context.
    private static readonly Dictionary<string, bool> s_editorWindowVisible = new(StringComparer.Ordinal);
    private static readonly Dictionary<string, EntityScript> s_defaults = new(StringComparer.Ordinal);

    // Scratch for returning a string property across the boundary. GetProperty is
    // called synchronously by the inspector, so a single pending buffer suffices.
    private static IntPtr s_stringScratch = IntPtr.Zero;

    private static PropertyType MapPropertyType(Type t)
    {
        if (t == typeof(float)) return PropertyType.Float;
        if (t == typeof(int)) return PropertyType.Int;
        if (t == typeof(bool)) return PropertyType.Bool;
        if (t == typeof(System.Numerics.Vector3)) return PropertyType.Vector3;
        if (t == typeof(string)) return PropertyType.String;
        if (t == typeof(Entity)) return PropertyType.Entity;
        if (typeof(IComponentRef).IsAssignableFrom(t)) return PropertyType.Component;
        if (t.IsEnum) return PropertyType.Enum;
        return PropertyType.None;
    }

    private static Prop[] BuildProps(Type type)
    {
        var props = new List<Prop>();
        foreach (System.Reflection.FieldInfo field in type.GetFields(
                     System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Instance))
        {
            if (field.IsInitOnly || field.IsLiteral)
            {
                continue;
            }
            if (field.IsDefined(typeof(HideInInspectorAttribute), inherit: true))
            {
                continue;
            }
            PropertyType pt = MapPropertyType(field.FieldType);
            if (pt != PropertyType.None)
            {
                // Component fields carry the required component's catalog name,
                // read once here from the wrapper type's static ComponentType.
                string? componentType = null;
                if (pt == PropertyType.Component)
                {
                    componentType = field.FieldType
                        .GetProperty("ComponentType", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static)
                        ?.GetValue(null) as string;
                }
                props.Add(new Prop
                {
                    Name = field.Name,
                    Type = pt,
                    Field = field,
                    ComponentType = componentType,
                    Replicated = field.IsDefined(typeof(ReplicatedAttribute), inherit: true),
                });
            }
        }
        int selfIndex = props.FindIndex(static p => p.Name == "Self");
        if (selfIndex > 0)
        {
            Prop self = props[selfIndex];
            props.RemoveAt(selfIndex);
            props.Insert(0, self);
        }
        return props.ToArray();
    }

    // Positions of the [Replicated] props within `props`. Derived from the finished
    // table (BuildProps moves "Self" to the front), so an index is always valid to
    // hand straight to GetProperty/SetProperty.
    private static int[] BuildReplicatedIndices(Prop[] props)
    {
        var indices = new List<int>();
        for (int i = 0; i < props.Length; i++)
        {
            if (props[i].Replicated)
            {
                indices.Add(i);
            }
        }
        return indices.ToArray();
    }

    // Public instance methods of `type` marked [NetRpc], sorted into declaration
    // order. GetMethods does not itself guarantee declaration order, so the sort
    // by MetadataToken (assigned in declaration order within a type) makes the
    // wire index deterministic - it must never silently shift between loads.
    private static MethodInfo[] BuildRpcMethods(Type type)
    {
        var methods = new List<MethodInfo>();
        foreach (MethodInfo method in type.GetMethods(BindingFlags.Public | BindingFlags.Instance))
        {
            if (method.IsDefined(typeof(NetRpcAttribute), inherit: true))
            {
                methods.Add(method);
            }
        }
        methods.Sort(static (a, b) => a.MetadataToken.CompareTo(b.MetadataToken));
        return methods.ToArray();
    }

    // ── Assembly / registry lifecycle ─────────────────────────────────────────

    [UnmanagedCallersOnly]
    internal static int LoadScripts(byte* assemblyPathUtf8)
    {
        try
        {
            return Load(Utf8.ToString(assemblyPathUtf8));
        }
        catch (ReflectionTypeLoadException ex)
        {
            // Surface the underlying loader failures - usually a missing reference.
            Bootstrap.ReportError("LoadScripts type-load failure: " + ex);
            foreach (Exception? loaderEx in ex.LoaderExceptions)
            {
                if (loaderEx != null)
                {
                    Bootstrap.ReportError("  -> " + loaderEx.Message);
                }
            }
            return -1;
        }
        catch (Exception ex)
        {
            Bootstrap.ReportError("LoadScripts failed: " + ex);
            return -1;
        }
    }

    // Clears every managed root into the current collectible context and requests
    // its unload. Shared by reload (Load) and teardown (UnloadScripts). Callers must
    // already have freed live per-instance GCHandles - the native side does this via
    // ScriptComponentSystem::Invalidate before a reload - so nothing else roots the
    // context and Unload() can complete.
    private static void ResetRegistry()
    {
        s_types.Clear();
        s_props.Clear();
        s_replicated.Clear();
        s_rpcMethods.Clear();
        s_defaults.Clear();
        // Remember each tool window's open/closed state (by Title) so a reload restores it instead of
        // reverting to the window's default; string keys don't root the context being unloaded.
        foreach (IEditorWindow w in s_editorWindows) { s_editorWindowVisible[w.Title] = w.Visible; }
        s_editorWindows.Clear();
        s_typeNames = Array.Empty<string>();
        ScriptsLoadContext? old = s_context;
        s_context = null;
        old?.Unload();
    }

    private static int Load(string assemblyPath)
    {
        // Tear down the previously loaded scripts first so the old collectible
        // context (game dll + any resolved NuGet deps) can actually unload before we
        // spin up the next one.
        ResetRegistry();

        // Load from bytes so the .dll on disk is never locked (mandatory for the
        // next `dotnet build` to overwrite it during hot reload on Windows).
        var context = new ScriptsLoadContext(assemblyPath);
        Assembly assembly;
        byte[] dllBytes = File.ReadAllBytes(assemblyPath);
        string pdbPath = Path.ChangeExtension(assemblyPath, ".pdb");
        using (var dll = new MemoryStream(dllBytes))
        {
            if (File.Exists(pdbPath))
            {
                using var pdb = new MemoryStream(File.ReadAllBytes(pdbPath));
                assembly = context.LoadFromStream(dll, pdb);
            }
            else
            {
                assembly = context.LoadFromStream(dll);
            }
        }

        var names = new List<string>();
        foreach (Type type in assembly.GetTypes())
        {
            // Editor-tooling windows (not mutually exclusive with scripts, so a separate check).
            if (!type.IsAbstract && typeof(IEditorWindow).IsAssignableFrom(type))
            {
                try
                {
                    var win = (IEditorWindow)Activator.CreateInstance(type)!;
                    // Restore the pre-reload open/closed state so a closed tool window stays closed.
                    if (s_editorWindowVisible.TryGetValue(win.Title, out bool wasVisible)) { win.Visible = wasVisible; }
                    s_editorWindows.Add(win);
                }
                catch (Exception ex) { Bootstrap.ReportError($"IEditorWindow {type.Name} ctor: {ex}"); }
            }

            if (type.IsAbstract || !typeof(EntityScript).IsAssignableFrom(type))
            {
                continue;
            }
            s_types[type.Name] = type;
            Prop[] props = BuildProps(type);
            s_props[type.Name] = props;
            s_replicated[type.Name] = BuildReplicatedIndices(props);
            s_rpcMethods[type.Name] = BuildRpcMethods(type);
            names.Add(type.Name);
            try
            {
                s_defaults[type.Name] = (EntityScript)Activator.CreateInstance(type)!;
            }
            catch
            {
                // A throwing default constructor just means no cached defaults.
            }
        }
        names.Sort(StringComparer.Ordinal);
        s_typeNames = names.ToArray();
        s_context = context;

        Log.Info($"Loaded {s_typeNames.Length} C# script type(s) from {Path.GetFileName(assemblyPath)}");
        return s_typeNames.Length;
    }

    [UnmanagedCallersOnly]
    internal static void UnloadScripts() => ResetRegistry();

    [UnmanagedCallersOnly]
    internal static int GetScriptTypeCount() => s_typeNames.Length;

    [UnmanagedCallersOnly]
    internal static int GetScriptTypeName(int index, byte* buffer, int bufferLength)
    {
        if (index < 0 || index >= s_typeNames.Length)
        {
            return 0;
        }
        return Utf8.Write(s_typeNames[index], buffer, bufferLength);
    }

    // ── Per-entity instance lifecycle ─────────────────────────────────────────

    [UnmanagedCallersOnly]
    internal static ulong CreateInstance(byte* typeNameUtf8, uint entityId)
    {
        try
        {
            string typeName = Utf8.ToString(typeNameUtf8);
            if (!s_types.TryGetValue(typeName, out Type? type))
            {
                Bootstrap.ReportError($"Unknown C# script type '{typeName}'");
                return 0;
            }

            var script = (EntityScript)Activator.CreateInstance(type)!;
            script.Bind(new Entity(entityId));
            GCHandle handle = GCHandle.Alloc(script);
            return (ulong)GCHandle.ToIntPtr(handle).ToInt64();
        }
        catch (Exception ex)
        {
            Bootstrap.ReportError("CreateInstance failed: " + ex);
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    internal static void DestroyInstance(ulong handle)
    {
        if (handle == 0)
        {
            return;
        }
        GCHandle.FromIntPtr((IntPtr)(long)handle).Free();
    }

    [UnmanagedCallersOnly]
    internal static void InvokeAttach(ulong handle)
    {
        if (Resolve(handle) is { } script)
        {
            try { script.OnAttach(); }
            catch (Exception ex) { Bootstrap.ReportError($"{script.GetType().Name}.OnAttach: {ex}"); }
        }
    }

    [UnmanagedCallersOnly]
    internal static void InvokeUpdate(ulong handle, float deltaTime)
    {
        if (Resolve(handle) is { } script)
        {
            try
            {
                script.DispatchPhysicsEvents();
                script.TickCoroutines(deltaTime);
                script.OnUpdate(deltaTime);
            }
            catch (Exception ex) { Bootstrap.ReportError($"{script.GetType().Name}.OnUpdate: {ex}"); }
        }
    }

    [UnmanagedCallersOnly]
    internal static void InvokeDetach(ulong handle)
    {
        if (Resolve(handle) is { } script)
        {
            try { script.OnDetach(); }
            catch (Exception ex) { Bootstrap.ReportError($"{script.GetType().Name}.OnDetach: {ex}"); }
        }
    }

    // Editor-only pump: each discovered IEditorWindow draws its own ImGui window while it is Visible.
    // A throwing window is isolated so a bad tool can't take down the editor. Never called from a
    // shipped GameRuntime.
    [UnmanagedCallersOnly]
    internal static void DrawEditorWindows()
    {
        for (int i = 0; i < s_editorWindows.Count; i++)
        {
            IEditorWindow w = s_editorWindows[i];
            bool visible;
            try { visible = w.Visible; }
            catch { visible = true; }
            if (!visible) { continue; }
            try { w.OnGui(); }
            catch (Exception ex) { Bootstrap.ReportError($"{w.GetType().Name}.OnGui: {ex}"); }
        }
    }

    // ── Project editor-window registry (editor menu enumeration + toggling) ────
    [UnmanagedCallersOnly]
    internal static int GetEditorWindowCount() => s_editorWindows.Count;

    [UnmanagedCallersOnly]
    internal static int GetEditorWindowTitle(int index, byte* buffer, int bufferLength)
    {
        if (index < 0 || index >= s_editorWindows.Count) { return 0; }
        string title;
        try { title = s_editorWindows[index].Title ?? string.Empty; }
        catch { title = string.Empty; }
        return Utf8.Write(title, buffer, bufferLength);
    }

    [UnmanagedCallersOnly]
    internal static int GetEditorWindowVisible(int index)
    {
        if (index < 0 || index >= s_editorWindows.Count) { return 0; }
        try { return s_editorWindows[index].Visible ? 1 : 0; }
        catch { return 0; }
    }

    [UnmanagedCallersOnly]
    internal static void SetEditorWindowVisible(int index, int visible)
    {
        if (index < 0 || index >= s_editorWindows.Count) { return; }
        try { s_editorWindows[index].Visible = visible != 0; }
        catch (Exception ex) { Bootstrap.ReportError($"{s_editorWindows[index].GetType().Name}.Visible: {ex.Message}"); }
    }

    private static EntityScript? Resolve(ulong handle)
    {
        return handle == 0 ? null : GCHandle.FromIntPtr((IntPtr)(long)handle).Target as EntityScript;
    }

    // ── Serialized script properties (inspector / scene overrides) ────────────

    [UnmanagedCallersOnly]
    internal static int GetPropertyCount(byte* typeNameUtf8)
    {
        return s_props.TryGetValue(Utf8.ToString(typeNameUtf8), out Prop[]? props) ? props.Length : 0;
    }

    [UnmanagedCallersOnly]
    internal static int GetPropertyInfo(byte* typeNameUtf8, int index, byte* nameBuf, int nameBufLen, int* outType)
    {
        if (!s_props.TryGetValue(Utf8.ToString(typeNameUtf8), out Prop[]? props) || index < 0 || index >= props.Length)
        {
            return 0;
        }
        Prop p = props[index];
        if (outType != null)
        {
            *outType = (int)p.Type;
        }
        return Utf8.Write(p.Name, nameBuf, nameBufLen);
    }

    /// <summary>
    /// Reports which of a type's properties carry [Replicated], as indices into the
    /// same table GetPropertyInfo/GetProperty/SetProperty use. Returns the total
    /// count (so a caller can detect truncation) and writes at most
    /// <paramref name="maxIndices"/> of them. The network layer reads and writes the
    /// values through the existing by-index property bridge, so there is exactly one
    /// marshalling path for a script field.
    /// </summary>
    [UnmanagedCallersOnly]
    internal static int GetReplicatedPropertyIndices(byte* typeNameUtf8, int* outIndices, int maxIndices)
    {
        if (!s_replicated.TryGetValue(Utf8.ToString(typeNameUtf8), out int[]? indices))
        {
            return 0;
        }
        if (outIndices != null)
        {
            int writable = maxIndices < indices.Length ? maxIndices : indices.Length;
            for (int i = 0; i < writable; i++)
            {
                outIndices[i] = indices[i];
            }
        }
        return indices.Length;
    }

    // ── Networking: RPCs ────────────────────────────────────────────────────

    /// <summary>
    /// Resolves a [NetRpc] method's wire index by name - the encode side, mirroring
    /// GetReplicatedPropertyIndices: a caller building an outbound call (Net.CallServer,
    /// or the native CSharpRpcBridge) turns a method name into the index that goes on
    /// the wire, which is the type's declaration-order [NetRpc] table built alongside
    /// s_props/s_replicated. Returns -1 if the type is unknown or declares no such RPC.
    /// </summary>
    [UnmanagedCallersOnly]
    internal static int GetNetRpcMethodIndex(byte* typeNameUtf8, byte* methodNameUtf8)
    {
        if (!s_rpcMethods.TryGetValue(Utf8.ToString(typeNameUtf8), out MethodInfo[]? methods))
        {
            return -1;
        }
        string methodName = Utf8.ToString(methodNameUtf8);
        for (int i = 0; i < methods.Length; i++)
        {
            if (methods[i].Name == methodName)
            {
                return i;
            }
        }
        return -1;
    }

    /// <summary>
    /// The decode side: runs [NetRpc] method <paramref name="methodIndex"/> of the
    /// script instance <paramref name="handle"/> names. <paramref name="argBlob"/> is
    /// a single value - null/empty for a parameterless method, otherwise interpreted
    /// as a UTF-8 string for a method with one string parameter (see Net.CallServer's
    /// argument-marshalling note; a richer argument shape is not supported yet).
    ///
    /// An unresolvable handle, an out-of-range index, or a method whose parameter
    /// shape is not one of the two supported above all drop the call silently rather
    /// than throwing - a peer can send anything, and a script assembly can reload
    /// out from under it. Any exception the method body itself raises is caught here
    /// too: nothing may escape across the native boundary.
    /// </summary>
    [UnmanagedCallersOnly]
    internal static void InvokeNetRpc(ulong handle, int methodIndex, byte* argBlob, int argLen)
    {
        if (Resolve(handle) is not { } script)
        {
            return;
        }
        if (!s_rpcMethods.TryGetValue(script.GetType().Name, out MethodInfo[]? methods)
            || methodIndex < 0 || methodIndex >= methods.Length)
        {
            return;
        }

        MethodInfo method = methods[methodIndex];
        try
        {
            ParameterInfo[] parameters = method.GetParameters();
            object?[] callArgs;
            if (parameters.Length == 0)
            {
                callArgs = Array.Empty<object?>();
            }
            else if (parameters.Length == 1 && parameters[0].ParameterType == typeof(string))
            {
                string arg = argBlob != null && argLen > 0
                    ? System.Text.Encoding.UTF8.GetString(argBlob, argLen)
                    : string.Empty;
                callArgs = new object?[] { arg };
            }
            else
            {
                // Only parameterless and single-string-parameter RPC methods are
                // supported today; see Net.CallServer's argument-marshalling note.
                Bootstrap.ReportError($"{script.GetType().Name}.{method.Name}: unsupported RPC parameter shape");
                return;
            }
            method.Invoke(script, callArgs);
        }
        catch (Exception ex)
        {
            Bootstrap.ReportError($"{script.GetType().Name}.{method.Name} RPC: {ex}");
        }
    }

    [UnmanagedCallersOnly]
    internal static int GetProperty(ulong handle, int index, PropertyValue* outValue)
    {
        if (outValue == null || Resolve(handle) is not { } script)
        {
            return 0;
        }
        if (!s_props.TryGetValue(script.GetType().Name, out Prop[]? props) || index < 0 || index >= props.Length)
        {
            return 0;
        }
        return FillValue(script, props[index], outValue);
    }

    [UnmanagedCallersOnly]
    internal static int GetDefaultProperty(byte* typeNameUtf8, int index, PropertyValue* outValue)
    {
        string typeName = Utf8.ToString(typeNameUtf8);
        if (outValue == null || !s_defaults.TryGetValue(typeName, out EntityScript? script))
        {
            return 0;
        }
        if (!s_props.TryGetValue(typeName, out Prop[]? props) || index < 0 || index >= props.Length)
        {
            return 0;
        }
        return FillValue(script, props[index], outValue);
    }

    private static int FillValue(object script, Prop p, PropertyValue* outValue)
    {
        object? value = p.Field.GetValue(script);
        outValue->Type = (int)p.Type;
        switch (p.Type)
        {
            case PropertyType.Float:
                outValue->F4[0] = (float)value!;
                break;
            case PropertyType.Int:
                outValue->I64 = (int)value!;
                break;
            case PropertyType.Bool:
                outValue->I64 = (bool)value! ? 1 : 0;
                break;
            case PropertyType.Enum:
                outValue->I64 = Convert.ToInt64(value);
                break;
            case PropertyType.Vector3:
                var v = (System.Numerics.Vector3)value!;
                outValue->F4[0] = v.X;
                outValue->F4[1] = v.Y;
                outValue->F4[2] = v.Z;
                break;
            case PropertyType.String:
                if (s_stringScratch != IntPtr.Zero)
                {
                    Marshal.FreeCoTaskMem(s_stringScratch);
                }
                s_stringScratch = Marshal.StringToCoTaskMemUTF8((string?)value ?? string.Empty);
                outValue->Str = (byte*)s_stringScratch;
                break;
            case PropertyType.Entity:
                outValue->I64 = ((Entity)value!).Id;
                break;
            case PropertyType.Component:
                // Entity id in I64 (like Entity), required component name in Str so
                // the inspector can validate drops without a metadata round-trip.
                outValue->I64 = value is IComponentRef cref ? cref.Owner.Id : 0;
                if (s_stringScratch != IntPtr.Zero)
                {
                    Marshal.FreeCoTaskMem(s_stringScratch);
                }
                s_stringScratch = Marshal.StringToCoTaskMemUTF8(p.ComponentType ?? string.Empty);
                outValue->Str = (byte*)s_stringScratch;
                break;
            default:
                return 0;
        }
        return 1;
    }

    [UnmanagedCallersOnly]
    internal static int SetProperty(ulong handle, int index, PropertyValue* value)
    {
        if (value == null || Resolve(handle) is not { } script)
        {
            return 0;
        }
        if (!s_props.TryGetValue(script.GetType().Name, out Prop[]? props) || index < 0 || index >= props.Length)
        {
            return 0;
        }

        Prop p = props[index];
        try
        {
            switch (p.Type)
            {
                case PropertyType.Float:
                    p.Field.SetValue(script, value->F4[0]);
                    break;
                case PropertyType.Int:
                    p.Field.SetValue(script, (int)value->I64);
                    break;
                case PropertyType.Bool:
                    p.Field.SetValue(script, value->I64 != 0);
                    break;
                case PropertyType.Enum:
                    p.Field.SetValue(script, Enum.ToObject(p.Field.FieldType, value->I64));
                    break;
                case PropertyType.Vector3:
                    p.Field.SetValue(script, new System.Numerics.Vector3(value->F4[0], value->F4[1], value->F4[2]));
                    break;
                case PropertyType.String:
                    p.Field.SetValue(script, Utf8.ToString(value->Str));
                    break;
                case PropertyType.Entity:
                    p.Field.SetValue(script, new Entity((uint)value->I64));
                    break;
                case PropertyType.Component:
                    // Reconstruct the wrapper (RigidBodyRef, ...) from the linked
                    // entity id via its public T(Entity) ctor.
                    p.Field.SetValue(script, Activator.CreateInstance(p.Field.FieldType, new Entity((uint)value->I64)));
                    break;
                default:
                    return 0;
            }
        }
        catch (Exception ex)
        {
            Bootstrap.ReportError($"SetProperty({p.Name}): {ex.Message}");
            return 0;
        }
        return 1;
    }
}

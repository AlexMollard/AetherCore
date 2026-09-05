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
    // s_props/s_replicated so the three tables cannot drift). The wire carries the
    // method NAME, never a position: two peers built from different source have
    // different declaration-order tables, so an index would name a different method
    // on each. The array position is this process's LOCAL dispatch index, resolved
    // from the name by GetNetRpcMethod on both the send and the receive side.
    private static readonly Dictionary<string, RpcEntry[]> s_rpcMethods = new(StringComparer.Ordinal);

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

    // Names ("TypeName.FieldName") already warned about in SetProperty's default
    // case, so an unsupported property type is reported once ever rather than once
    // per call - a replicated field re-set every network tick would otherwise spam
    // the log for a condition that never changes.
    private static readonly HashSet<string> s_warnedUnsupportedProperties = new(StringComparer.Ordinal);

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

    // One [NetRpc] method's resolved metadata: reflected once per type at load,
    // never re-reflected per call. Limiter is null when the method declares no
    // NetRpcAttribute.MaxPerSecond (the default) - unlimited, so InvokeNetRpc
    // dispatches exactly as it did before rate limiting existed.
    private sealed class RpcEntry
    {
        public required MethodInfo Method;
        public required NetRpcAttribute Attribute;
        public RpcRateLimiter? Limiter;
    }

    // Public instance methods of `type` marked [NetRpc], sorted into declaration
    // order. GetMethods does not itself guarantee declaration order, so the sort
    // by MetadataToken (assigned in declaration order within a type) makes the
    // wire index deterministic - it must never silently shift between loads.
    private static RpcEntry[] BuildRpcMethods(Type type)
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

        var entries = new RpcEntry[methods.Count];
        for (int i = 0; i < methods.Count; i++)
        {
            NetRpcAttribute attribute = methods[i].GetCustomAttribute<NetRpcAttribute>(inherit: true)
                ?? new NetRpcAttribute(NetRpcTarget.Server);
            entries[i] = new RpcEntry
            {
                Method = methods[i],
                Attribute = attribute,
                Limiter = attribute.MaxPerSecond > 0
                    ? new RpcRateLimiter(attribute.MaxPerSecond, attribute.Burst)
                    : null,
            };
        }
        return entries;
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
        // The live-instance table holds strong references into the context being
        // unloaded. The native side frees every handle (ScriptComponentSystem::
        // Invalidate) before getting here, so this is a backstop - but a missed
        // entry would keep the whole ALC alive, so clear it unconditionally.
        ScriptInstances.Clear();
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
        // Server-target [NetRpc] methods that declare no MaxPerSecond, collected
        // across the whole assembly so the load logs ONE diagnostic naming all of
        // them - never one per method (that is exactly the noise that trains an
        // author to stop reading these logs) and never per call (a Server method is
        // the one an unbounded remote client can reach at all, so this has nothing
        // to do with any single call). Client/Multicast methods are excluded
        // entirely: only the host ever originates those, so there is no remote
        // caller to rate-limit against in the first place.
        var unlimitedServerRpcs = new List<string>();
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
            RpcEntry[] rpcMethods = BuildRpcMethods(type);
            s_rpcMethods[type.Name] = rpcMethods;
            foreach (RpcEntry entry in rpcMethods)
            {
                if (entry.Limiter is null && entry.Attribute.Target == NetRpcTarget.Server)
                {
                    unlimitedServerRpcs.Add($"{type.Name}.{entry.Method.Name}");
                }
            }
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
        unlimitedServerRpcs.Sort(StringComparer.Ordinal);
        s_typeNames = names.ToArray();
        s_context = context;

        Log.Info($"Loaded {s_typeNames.Length} C# script type(s) from {Path.GetFileName(assemblyPath)}");
        if (unlimitedServerRpcs.Count > 0)
        {
            // Info, not Warn: this fires on every load of a project that has not
            // (yet, or ever intends to) rate-limit a given method, which for most
            // small/trusted-LAN games is most methods, most of the time - a Warn
            // that fires that unconditionally trains an author to stop reading
            // warnings, which is worse than the footgun it would be trying to
            // flag. This is a standing reminder of a decision not yet made, not a
            // report of something currently going wrong - the same distinction as
            // "N script types loaded" versus InvokeNetRpc's Warn for a call the
            // limiter is ACTIVELY dropping right now.
            Log.Info($"AetherCore: {unlimitedServerRpcs.Count} [NetRpc(NetRpcTarget.Server)] method(s) declare " +
                $"no MaxPerSecond, so any connected client can call them at any rate: " +
                $"{string.Join(", ", unlimitedServerRpcs)}. Set NetRpcAttribute.MaxPerSecond (and Burst) on each " +
                "once its rate is decided.");
        }
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
            // Pin the instance before publishing it as live: if the Alloc threw,
            // the catch below would return 0 and an entry no handle ever backed
            // would sit in ScriptInstances forever, rooting the collectible
            // context. Native cannot invoke the instance before this returns its
            // handle, so registering after the Alloc still leaves it findable via
            // Entity.GetScript by the time anything can call into it.
            GCHandle handle = GCHandle.Alloc(script);
            ScriptInstances.Register(entityId, script);
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
        try
        {
            GCHandle gc = GCHandle.FromIntPtr((IntPtr)(long)handle);
            // Stop reporting it as live on its entity first: the instance table is the
            // mirror of these handles, so it must never outlive one.
            if (gc.Target is EntityScript script)
            {
                ScriptInstances.Unregister(script);
            }
            gc.Free();
        }
        catch (Exception ex)
        {
            // The handle value is supplied entirely by native code: a double free,
            // or a free of a handle UnloadScripts already drained, makes FromIntPtr
            // or Free throw - and nothing may escape an entry point.
            Bootstrap.ReportError($"DestroyInstance({handle}): {ex.Message}");
        }
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

    /// <summary>
    /// The ownership-change callback the native OwnershipHook wiring calls
    /// whenever a script's entity's <c>NetworkIdentity.owner</c> changes - most
    /// importantly the very first time it becomes known on a client, since
    /// <see cref="Net.IsOwner"/> answers false for everything until the host's
    /// Welcome lands and ownership genuinely is not knowable at attach time.
    /// Resolves the handle exactly like <see cref="InvokeUpdate"/>, and does
    /// nothing for a stale handle - a reload or a despawn can race the native
    /// event that triggers this call.
    /// </summary>
    [UnmanagedCallersOnly]
    internal static void InvokeOwnershipChanged(ulong handle, uint owner, int isOwner)
    {
        if (Resolve(handle) is { } script)
        {
            try { script.OnOwnershipChanged(owner, isOwner != 0); }
            catch (Exception ex) { Bootstrap.ReportError($"{script.GetType().Name}.OnOwnershipChanged: {ex}"); }
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
        if (handle == 0)
        {
            return null;
        }
        try
        {
            return GCHandle.FromIntPtr((IntPtr)(long)handle).Target as EntityScript;
        }
        catch (Exception ex)
        {
            // A handle value that is not a live GCHandle (a native double free, or
            // a free of a handle UnloadScripts already drained) makes FromIntPtr
            // throw, and nothing may escape a [UnmanagedCallersOnly] entry point.
            // Every caller already treats null as "instance missing".
            Bootstrap.ReportError($"Invalid script handle {handle}: {ex.Message}");
            return null;
        }
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

    /// <summary>
    /// Resolves a [NetRpc] method by name in the type's declaration-order table,
    /// mirroring GetReplicatedPropertyIndices. Both wire sides call it: an
    /// outbound call (Net.Call, via the native CSharpRpcBridge) turns the name
    /// into this process's dispatch index, and ApplyRpc on the receiving peer
    /// turns the name that arrived into that peer's index. The method NAME - not
    /// this index - is what travels on the wire, so a peer with a different
    /// assembly drops an unknown name instead of invoking whatever sits at some
    /// position of its own table. <paramref name="outTarget"/> receives the
    /// <see cref="NetRpcTarget"/> the attribute declared, so the direction of a
    /// call is stated once, on the method, and never at the call site - nor in a
    /// wire byte that disagrees with it. Returns -1 - leaving
    /// <paramref name="outTarget"/> untouched - if the type is unknown or declares
    /// no such RPC.
    /// </summary>
    /// <remarks>
    /// Reflection over a custom attribute can throw (a torn assembly load, a missing
    /// dependency), and nothing may escape into native code, so the whole body is
    /// guarded - the same discipline as InvokeNetRpc.
    /// </remarks>
    [UnmanagedCallersOnly]
    internal static int GetNetRpcMethod(byte* typeNameUtf8, byte* methodNameUtf8, int* outTarget)
    {
        try
        {
            if (!s_rpcMethods.TryGetValue(Utf8.ToString(typeNameUtf8), out RpcEntry[]? methods))
            {
                return -1;
            }
            string methodName = Utf8.ToString(methodNameUtf8);
            for (int i = 0; i < methods.Length; i++)
            {
                if (methods[i].Method.Name != methodName)
                {
                    continue;
                }
                if (outTarget != null)
                {
                    *outTarget = (int)methods[i].Attribute.Target;
                }
                return i;
            }
            return -1;
        }
        catch (Exception ex)
        {
            Bootstrap.ReportError($"GetNetRpcMethod failed: {ex.Message}");
            return -1;
        }
    }

    /// <summary>
    /// The connection InvokeNetRpc is running THIS call on behalf of, for
    /// <see cref="RpcRateLimiter"/>. InvokeNetRpc's own arguments carry no sender
    /// field - the ABI predates per-method rate limiting - so this reads it back
    /// from the ownership invariant the RPC layer already enforces natively
    /// (see NetRpc.cpp's ApplyRpc and RouteRpc):
    ///   - Server only ever runs here for a wire-arrived call ApplyRpc has already
    ///     proven came from the target entity's owning connection, or for the
    ///     host's own local call on an entity it owns. Either way the entity's
    ///     owner names the caller. (Host code MAY call a Server RPC on an entity it
    ///     does not own - Host+Server always routes locally in RouteRpc - and this
    ///     attributes that one case to the entity's owner rather than literally
    ///     "the host process"; that is not a path a remote attacker can drive, so
    ///     sharing the bucket there is a deliberate, safe simplification, not a
    ///     workaround for missing wire data.)
    ///   - Client and Multicast only ever originate from the host (see
    ///     NetRpcTarget's remarks; ApplyRpc's direction gate drops anything else
    ///     before InvokeNetRpc ever runs), so the caller is always the host.
    /// </summary>
    private static uint CallingConnectionId(EntityScript script, NetRpcTarget target)
        => target == NetRpcTarget.Server ? Net.OwnerOf(script.Self) : HostConnectionId;

    // Connection id 0 always names the host - see Net.OwnerOf's remarks and
    // RouteRpc's use of kInvalidConnection (also 0) for a host-owned entity.
    private const uint HostConnectionId = 0;

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
    ///
    /// A method declaring <see cref="NetRpcAttribute.MaxPerSecond"/> is metered
    /// through its cached <see cref="RpcRateLimiter"/> BEFORE the reflection
    /// dispatch below: a call past the limit is dropped exactly like the other
    /// silent-drop cases above (never queued, never thrown), with one warning per
    /// distinct (method, connection) so a flood cannot turn into a logging flood.
    /// </summary>
    [UnmanagedCallersOnly]
    internal static void InvokeNetRpc(ulong handle, int methodIndex, byte* argBlob, int argLen)
    {
        if (Resolve(handle) is not { } script)
        {
            return;
        }
        if (!s_rpcMethods.TryGetValue(script.GetType().Name, out RpcEntry[]? methods)
            || methodIndex < 0 || methodIndex >= methods.Length)
        {
            return;
        }

        RpcEntry entry = methods[methodIndex];
        MethodInfo method = entry.Method;
        try
        {
            if (entry.Limiter is { } limiter)
            {
                uint connectionId = CallingConnectionId(script, entry.Attribute.Target);
                if (!limiter.TryAdmit(connectionId, out bool warnCaller))
                {
                    if (warnCaller)
                    {
                        Log.Warn($"{script.GetType().Name}.{method.Name}: dropped a Net RPC from connection " +
                            $"{connectionId} - past its [NetRpc(MaxPerSecond={entry.Attribute.MaxPerSecond})] limit. " +
                            "Raise MaxPerSecond/Burst if this caller's rate is legitimate, or investigate a flood. " +
                            "Further drops from this connection will not be logged.");
                    }
                    return;
                }
            }

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
        try
        {
            return FillValue(script, props[index], outValue);
        }
        catch (Exception ex)
        {
            // Like SetProperty below: the field table is keyed by type name, so a
            // stale instance from a reloaded context can make GetValue throw, and
            // nothing may escape a [UnmanagedCallersOnly] entry point.
            Bootstrap.ReportError($"GetProperty({props[index].Name}): {ex.Message}");
            return 0;
        }
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
        try
        {
            return FillValue(script, props[index], outValue);
        }
        catch (Exception ex)
        {
            // Same discipline as GetProperty/SetProperty: reflection over the
            // default instance must stay inside the entry point's guard.
            Bootstrap.ReportError($"GetDefaultProperty({props[index].Name}): {ex.Message}");
            return 0;
        }
    }

    // Marshals `value` into s_stringScratch as explicit UTF-8 bytes plus a
    // trailing NUL, and returns the byte count (which the caller stores in
    // Reserved). The terminator alone is NOT enough: a replicated string field
    // may contain embedded NULs and the wire is length-prefixed, but the native
    // side copies Str as a std::string - without the explicit length it would stop
    // at the first NUL and silently truncate the value on its way to the wire.
    // The NUL is still written so readers that predate Reserved (or ignore it)
    // keep seeing a valid prefix rather than over-reading.
    private static int WriteScratchString(string value)
    {
        if (s_stringScratch != IntPtr.Zero)
        {
            Marshal.FreeCoTaskMem(s_stringScratch);
        }
        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(value);
        s_stringScratch = Marshal.AllocCoTaskMem(bytes.Length + 1);
        Marshal.Copy(bytes, 0, s_stringScratch, bytes.Length);
        Marshal.WriteByte(s_stringScratch, bytes.Length, 0);
        return bytes.Length;
    }

    // The inverse contract: `reserved` carries the byte length when >= 0, and the
    // pointer is NUL-terminated regardless, so a negative length (a writer that
    // predates the explicit length) falls back to the terminator.
    private static string ReadNativeString(byte* ptr, int reserved)
    {
        if (ptr == null)
        {
            return string.Empty;
        }
        return reserved >= 0
            ? System.Text.Encoding.UTF8.GetString(ptr, reserved)
            : Utf8.ToString(ptr);
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
                outValue->Reserved = WriteScratchString((string?)value ?? string.Empty);
                outValue->Str = (byte*)s_stringScratch;
                break;
            case PropertyType.Entity:
                outValue->I64 = ((Entity)value!).Id;
                break;
            case PropertyType.Component:
                // Entity id in I64 (like Entity), required component name in Str so
                // the inspector can validate drops without a metadata round-trip.
                outValue->I64 = value is IComponentRef cref ? cref.Owner.Id : 0;
                outValue->Reserved = WriteScratchString(p.ComponentType ?? string.Empty);
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
                    p.Field.SetValue(script, ReadNativeString(value->Str, value->Reserved));
                    break;
                case PropertyType.Component:
                    // Reconstruct the wrapper (RigidBodyRef, ...) from the linked
                    // entity id via its public T(Entity) ctor.
                    p.Field.SetValue(script, Activator.CreateInstance(p.Field.FieldType, new Entity((uint)value->I64)));
                    break;
                case PropertyType.Entity:
                    // Mirrors GetProperty's Entity case in reverse: the id crosses as
                    // I64 either way, so round-tripping it through the inspector or a
                    // scene-authored `t = 'entity'` reference reads back the same value.
                    p.Field.SetValue(script, new Entity((uint)value->I64));
                    break;
                default:
                    // A property that silently never gets written is invisible until
                    // someone notices the field reads back at its default - this is
                    // exactly that failure, named once per (type, property) rather than
                    // once per call so a replicated field re-set every tick does not
                    // spam the log.
                    if (s_warnedUnsupportedProperties.Add($"{script.GetType().Name}.{p.Name}"))
                    {
                        Bootstrap.ReportError($"SetProperty({p.Name}): unsupported property type {p.Type} - value not applied.");
                    }
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

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
    }

    private static readonly Dictionary<string, Prop[]> s_props = new(StringComparer.Ordinal);

    // A default-constructed instance per type, so the inspector can show default
    // field values when no live instance exists (edit mode).
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
                props.Add(new Prop { Name = field.Name, Type = pt, Field = field });
            }
        }
        return props.ToArray();
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
        s_defaults.Clear();
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
            if (type.IsAbstract || !typeof(EntityScript).IsAssignableFrom(type))
            {
                continue;
            }
            s_types[type.Name] = type;
            s_props[type.Name] = BuildProps(type);
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
            try { script.OnUpdate(deltaTime); }
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

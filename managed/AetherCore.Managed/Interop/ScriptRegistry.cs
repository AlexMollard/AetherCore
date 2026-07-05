using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace AetherCore.Managed.Interop;

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
    // the exact AetherCore.Managed the host loaded — same type identity, and it
    // resolves regardless of which load context the host placed it in.
    private static readonly Assembly EngineAssembly = typeof(EntityScript).Assembly;

    private sealed class ScriptsLoadContext() : AssemblyLoadContext(name: "AetherScripts", isCollectible: true)
    {
        protected override Assembly? Load(AssemblyName name)
        {
            return name.Name == "AetherCore.Managed" ? EngineAssembly : null;
        }
    }

    private static ScriptsLoadContext? s_context;
    private static readonly Dictionary<string, Type> s_types = new(StringComparer.Ordinal);
    private static string[] s_typeNames = Array.Empty<string>();

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
            // Surface the underlying loader failures — usually a missing reference.
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

    private static int Load(string assemblyPath)
    {
        // Load from bytes so the .dll on disk is never locked (mandatory for the
        // next `dotnet build` to overwrite it during hot reload on Windows).
        var context = new ScriptsLoadContext();
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

        s_types.Clear();
        var names = new List<string>();
        foreach (Type type in assembly.GetTypes())
        {
            if (type.IsAbstract || !typeof(EntityScript).IsAssignableFrom(type))
            {
                continue;
            }
            s_types[type.Name] = type;
            names.Add(type.Name);
        }
        names.Sort(StringComparer.Ordinal);
        s_typeNames = names.ToArray();
        s_context = context;

        Log.Info($"Loaded {s_typeNames.Length} C# script type(s) from {Path.GetFileName(assemblyPath)}");
        return s_typeNames.Length;
    }

    [UnmanagedCallersOnly]
    internal static void UnloadScripts()
    {
        // Phase 1: drop references and request unload. Phase 3 adds the bounded
        // GC-wait + leak-tolerance around this.
        s_types.Clear();
        s_typeNames = Array.Empty<string>();
        ScriptsLoadContext? context = s_context;
        s_context = null;
        context?.Unload();
    }

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
}

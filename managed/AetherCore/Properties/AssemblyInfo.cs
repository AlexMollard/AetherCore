using System.Runtime.CompilerServices;

// The interop boundary is entirely blittable (Vector3, primitives, byte* strings,
// function pointers). Disabling runtime marshalling lets LibraryImport pass these
// types by raw copy - faster, and required to marshal Vector3 by value.
[assembly: DisableRuntimeMarshalling]

// The ABI/host-boot assembly is the engine's own internal partner: it populates
// HostBridge, binds EntityScript.Bind, and uses the Utf8 marshalling helpers.
[assembly: InternalsVisibleTo("AetherCore.Interop")]

// The SDK's unit tests. They swap EngineBackend.Api for a recording double so the
// session state machine, the label projection and the script-instance table can be
// exercised without a running engine, and they read ScriptInstances directly.
[assembly: InternalsVisibleTo("AetherCore.Tests")]

using System.Runtime.CompilerServices;

// The interop boundary is entirely blittable (Vector3, primitives, byte* strings,
// function pointers). Disabling runtime marshalling lets LibraryImport pass these
// types by raw copy — faster, and required to marshal Vector3 by value.
[assembly: DisableRuntimeMarshalling]

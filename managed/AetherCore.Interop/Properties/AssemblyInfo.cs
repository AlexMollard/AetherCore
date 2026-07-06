using System.Runtime.CompilerServices;

// UnmanagedCallersOnly entry points and the ABI structs cross the native boundary
// by raw copy; disable runtime marshalling to match the SDK and the C++ side.
[assembly: DisableRuntimeMarshalling]

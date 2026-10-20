using System.Runtime.InteropServices;

namespace AetherCore.Interop;

internal static unsafe class Program
{
    [DllImport("AetherHost")]
    private static extern int aether_managed_start(delegate* unmanaged<NativeHostCallbacks*, int, ManagedScriptApi*, int, int> bootstrap);

    private static int Main() => aether_managed_start(&Bootstrap.Init);
}

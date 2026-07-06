namespace AetherCore;

/// <summary>
/// The native host callbacks the SDK needs at runtime, extracted from the ABI
/// handshake. <c>AetherCore.Interop.Bootstrap.Init</c> copies the function
/// pointers out of the incoming <c>NativeHostCallbacks</c> into these fields.
///
/// This lives in the SDK (not the Interop assembly) to keep the assembly
/// dependency acyclic: game-facing types such as <see cref="Log"/> need the log
/// callback, and the SDK must not reference the Interop/ABI layer. Interop
/// reaches in here via <c>InternalsVisibleTo</c>.
/// </summary>
internal static unsafe class HostBridge
{
    /// <summary>Native log sink: <c>(level, utf8Message) -&gt; void</c>.</summary>
    internal static delegate* unmanaged<int, byte*, void> Log;

    /// <summary>Native script-error sink: <c>(utf8Message) -&gt; void</c>.</summary>
    internal static delegate* unmanaged<byte*, void> ReportScriptError;
}

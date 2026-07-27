using System;
using System.Text;

namespace AetherCore;

/// <summary>
/// Entry point for calling a <see cref="NetRpcAttribute"/>-marked script method.
/// </summary>
/// <remarks>
/// Argument marshalling is deliberately minimal: a call carries either no
/// arguments or a single <see cref="string"/> argument (the shape the current
/// consumer - a chat message - needs). Passing anything else throws
/// <see cref="ArgumentException"/> here, in the calling script's own code, rather
/// than failing silently or crossing the wire malformed. A general-purpose
/// argument serializer is not built here; see NetRpc.hpp's DecodeRpc for why.
/// </remarks>
public static class Net
{
    /// <summary>
    /// Invokes a <see cref="NetRpcAttribute"/> method declared on a script attached
    /// to <paramref name="entity"/>, resolved by <paramref name="methodName"/>.
    /// </summary>
    /// <remarks>
    /// There is currently no live network session wired into a running game (see
    /// AetherCore's net/ layer), so a <see cref="NetRpcTarget.Server"/> call always
    /// runs locally - correct behavior for a host, or for an unnetworked game, and
    /// the seam a future networking system hooks to redirect a client's call over
    /// the wire instead.
    /// </remarks>
    /// <exception cref="ArgumentException">
    /// More than one argument was passed, or a single argument was passed that is
    /// not a <see cref="string"/> - see the marshalling note on <see cref="Net"/>.
    /// </exception>
    public static unsafe void CallServer(Entity entity, string methodName, params object[] args)
    {
        if (args.Length > 1 || (args.Length == 1 && args[0] is not string))
        {
            throw new ArgumentException(
                "Net.CallServer supports at most one string argument today.", nameof(args));
        }

        if (args.Length == 0)
        {
            Native.aether_net_call_server(entity.Id, methodName, null, 0);
            return;
        }

        string arg = (string)args[0];
        byte[] blob = Encoding.UTF8.GetBytes(arg);
        fixed (byte* ptr = blob)
        {
            Native.aether_net_call_server(entity.Id, methodName, ptr, blob.Length);
        }
    }
}

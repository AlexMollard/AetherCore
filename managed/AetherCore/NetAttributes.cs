using System;

namespace AetherCore;

/// <summary>
/// Marks a public script field for network replication. The host writes it, every
/// client reads it - the same shape as marking a C++ component field with
/// AE_FIELD_REP, so a project replicates its own state without adding an engine
/// component. Only the host may meaningfully change a replicated field; a client
/// write is overwritten by the next snapshot.
/// </summary>
/// <remarks>
/// Replication reuses the inspector's property table, so the field must also be a
/// supported property type (float, int, bool, Vector3, string, or an enum). Entity
/// and component-reference fields are exposed to the inspector but never
/// replicated: a local entity id is meaningless on another machine.
/// </remarks>
[AttributeUsage(AttributeTargets.Field, Inherited = true)]
public sealed class ReplicatedAttribute : Attribute;

/// <summary>Where a <see cref="NetRpcAttribute"/> method runs.</summary>
/// <remarks>
/// Only client-to-host dispatch exists. Host-to-client targets (an owning-client
/// call, a multicast) are deliberately absent rather than declared and ignored: the
/// framework has exactly one RPC send path, <see cref="Net.CallServer"/>, so a
/// declared-but-unwired target would compile, run on the host, and tell nobody.
/// </remarks>
public enum NetRpcTarget
{
    /// <summary>Called on a client, executed on the host.</summary>
    Server = 0,
}

/// <summary>
/// Marks a script method as a remote procedure call. Invoking it through
/// <see cref="Net.CallServer"/> on a client sends the call to the host and runs it
/// there; on the host, and in an unnetworked game, it runs locally.
/// </summary>
[AttributeUsage(AttributeTargets.Method, Inherited = true)]
public sealed class NetRpcAttribute : Attribute
{
    /// <summary>Marks the method as an RPC dispatched to <paramref name="target"/>.</summary>
    public NetRpcAttribute(NetRpcTarget target) => Target = target;

    /// <summary>Where the call is executed.</summary>
    public NetRpcTarget Target { get; }
}

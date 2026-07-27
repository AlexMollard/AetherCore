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
/// <para>
/// The target is declared once, here on the method, and <see cref="Net.Call"/> reads
/// it - there is no way to state a different direction at the call site, so a call
/// and its declaration can never disagree.
/// </para>
/// <para>
/// The host is authoritative, so only it may originate <see cref="Client"/> and
/// <see cref="Multicast"/>: calling one of those from a client returns <c>false</c>
/// and sends nothing. A client that wants everyone to hear something sends a
/// <see cref="Server"/> call and lets the host multicast it - which is exactly how
/// chat works, and is why a client cannot spoof a broadcast.
/// </para>
/// <para>
/// These values travel on the wire, so the numbers are part of the network protocol
/// and are mirrored by <c>NetRpcTarget</c> in <c>src/app/net/NetRpc.hpp</c>. The
/// receiving peer checks them: a <see cref="Server"/> call arriving at a client, or
/// a <see cref="Client"/>/<see cref="Multicast"/> call arriving at the host, is
/// dropped.
/// </para>
/// </remarks>
public enum NetRpcTarget
{
    /// <summary>
    /// Called on a client, executed on the host. On the host, and in an unnetworked
    /// game, the host is this process, so the call runs locally.
    /// </summary>
    Server = 0,

    /// <summary>
    /// Called on the host, executed on the client that owns the target entity - and
    /// on nobody else. If the host itself owns the entity there is no remote owning
    /// client, so the call runs locally on the host instead.
    /// </summary>
    Client = 1,

    /// <summary>
    /// Called on the host, executed on every connected client AND locally on the
    /// host. Running it on the host too matches UE5 and is what a caller wants for
    /// the usual case - a chat line the host must also see.
    /// </summary>
    Multicast = 2,
}

/// <summary>
/// Marks a script method as a remote procedure call, dispatched by
/// <see cref="Net.Call"/> to wherever <see cref="Target"/> says it runs.
/// </summary>
[AttributeUsage(AttributeTargets.Method, Inherited = true)]
public sealed class NetRpcAttribute : Attribute
{
    /// <summary>Marks the method as an RPC dispatched to <paramref name="target"/>.</summary>
    public NetRpcAttribute(NetRpcTarget target) => Target = target;

    /// <summary>Where the call is executed.</summary>
    public NetRpcTarget Target { get; }
}

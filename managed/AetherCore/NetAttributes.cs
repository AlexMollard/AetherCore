using System;
using System.Collections.Generic;

namespace AetherCore;

/// <summary>
/// Marks a public script field for network replication. The OWNER of the entity
/// writes it - the owner simulates the entity and sends its changed values, and
/// the host relays them to every other peer - the same shape as marking a C++
/// component field with AE_FIELD_REP, so a project replicates its own state
/// without adding an engine component. A write by anybody else is what gets
/// overwritten: a non-owner's value is replaced by the owner's next send. Gate
/// writes with <see cref="Net.HasAuthority"/> rather than
/// <see cref="Net.IsHost"/> - the host is not authoritative for a client's
/// entities. Offline this peer owns everything, so single-player writes need no
/// role test.
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

    /// <summary>
    /// Steady-state cap on accepted calls per second from one calling connection -
    /// see <see cref="RpcRateLimiter"/> for exactly what "calling connection" means
    /// for a given <see cref="Target"/>. The default, 0, is UNLIMITED: every
    /// existing [NetRpc] method that does not set this keeps its exact current
    /// behavior. That default is also precisely the shape of the two exploitable
    /// cheats a real 4-player deathmatch's audit found - a host handler with no
    /// rate limit at all - so it trades "nothing changes for a game that says
    /// nothing" for "the footgun stays live until a game author sets this". Set it
    /// on every <see cref="NetRpcTarget.Server"/> method a client's input drives (a
    /// fire button, a hit report, a chat line): the framework cannot guess a safe
    /// number for a specific game, but it can make "no limit" a decision an author
    /// has to notice rather than one they never see.
    /// </summary>
    public int MaxPerSecond { get; init; }

    /// <summary>
    /// Extra calls a caller may spend in a burst above the steady
    /// <see cref="MaxPerSecond"/> rate (token-bucket capacity). Ignored while
    /// <see cref="MaxPerSecond"/> is 0. Left at its default (0) while
    /// <see cref="MaxPerSecond"/> is set, the bucket's capacity is just
    /// <see cref="MaxPerSecond"/> itself - a caller may spend one second's whole
    /// budget at once but no more.
    /// </summary>
    public int Burst { get; init; }
}

/// <summary>
/// Per-connection token bucket enforcing one <see cref="NetRpcAttribute"/>
/// method's <see cref="NetRpcAttribute.MaxPerSecond"/> / <see cref="NetRpcAttribute.Burst"/>.
/// One instance guards one method for the life of the loaded script assembly
/// (<c>ScriptRegistry</c> builds it once, alongside the method's cached
/// <c>MethodInfo</c>, when <see cref="NetRpcAttribute.MaxPerSecond"/> is set), so
/// a flood of calls never re-allocates and a caller's bucket survives across
/// calls for as long as the connection does.
/// </summary>
/// <remarks>
/// <c>connectionId</c>-style values are per the framework's own
/// convention: 0 always names the host (see <see cref="Net.OwnerOf"/>). Kept
/// separate from a single global counter on purpose - a global cap lets one
/// flooding peer exhaust the budget every other caller of the same method
/// needs, which is exactly the shape of the two exploits this attribute exists
/// to close. See <c>ScriptRegistry.InvokeNetRpc</c> for how "the calling
/// connection" is derived for a given call: today's ABI carries no explicit
/// sender field, so it is read back from the ownership invariant the RPC layer
/// already enforces (a <see cref="NetRpcTarget.Server"/> call only ever runs for
/// the entity's owning connection; a <see cref="NetRpcTarget.Client"/> or
/// <see cref="NetRpcTarget.Multicast"/> call only ever originates from the
/// host).
/// </remarks>
internal sealed class RpcRateLimiter
{
    private struct Bucket
    {
        public double Tokens;
        public double LastRefillSeconds;
    }

    private readonly double _refillPerSecond;
    private readonly double _capacity;
    private readonly Dictionary<uint, Bucket> _buckets = new();

    // Connections already warned about a refusal from this method - warned once,
    // ever, per connection, never once per packet. A flood is exactly the moment
    // logging must not itself become a flood.
    private readonly HashSet<uint> _warnedConnections = new();

    public RpcRateLimiter(int maxPerSecond, int burst)
    {
        _refillPerSecond = maxPerSecond;
        _capacity = burst > 0 ? burst : maxPerSecond;
    }

    /// <summary>
    /// Spends one token for <paramref name="connectionId"/> if it has one to
    /// spend. A refusal is a drop, never a queue - a queue just moves a flood's
    /// memory cost onto the process instead of preventing it. <paramref
    /// name="warnCaller"/> is true only the first time this connection is ever
    /// refused, so a caller can log a single actionable warning without
    /// repeating it for every further dropped call in the same flood.
    /// </summary>
    public bool TryAdmit(uint connectionId, out bool warnCaller)
    {
        double now = Environment.TickCount64 / 1000.0;
        if (!_buckets.TryGetValue(connectionId, out Bucket bucket))
        {
            // A caller's first-ever call sees a full bucket: a burst is meant to be
            // spendable immediately, not earned by waiting first.
            bucket = new Bucket { Tokens = _capacity, LastRefillSeconds = now };
        }
        else
        {
            double elapsed = now - bucket.LastRefillSeconds;
            bucket.Tokens = Math.Min(_capacity, bucket.Tokens + elapsed * _refillPerSecond);
            bucket.LastRefillSeconds = now;
        }

        if (bucket.Tokens < 1.0)
        {
            _buckets[connectionId] = bucket;
            warnCaller = _warnedConnections.Add(connectionId);
            return false;
        }

        bucket.Tokens -= 1.0;
        _buckets[connectionId] = bucket;
        warnCaller = false;
        return true;
    }
}

using System;
using AetherCore;
using Xunit;

namespace AetherCore.Tests;

/// <summary>
/// The per-connection token bucket behind <see cref="NetRpcAttribute.MaxPerSecond"/> /
/// <see cref="NetRpcAttribute.Burst"/> - the piece <c>ScriptRegistry.InvokeNetRpc</c>
/// consults before ever reflecting into a game's [NetRpc] method body.
///
/// Before this batch neither <c>RpcRateLimiter</c> nor these two attribute
/// properties existed at all, so every case below fails to COMPILE against the
/// prior source (there is no rate-limit surface to test); that is what "fails
/// pre-change" means here; there was nothing to enforce a limit at all - a
/// game had to hand-roll one, exactly as Whisper's PlayerCombat did three times.
/// </summary>
public sealed class RpcRateLimiterTests
{
    [Fact]
    public void TryAdmit_AllowsUpToBurstThenDropsTheNextCall()
    {
        var limiter = new RpcRateLimiter(maxPerSecond: 2, burst: 2);

        Assert.True(limiter.TryAdmit(7, out bool warn1));
        Assert.False(warn1);
        Assert.True(limiter.TryAdmit(7, out bool warn2));
        Assert.False(warn2);

        // Burst spent: the third call inside the same instant is over the limit.
        Assert.False(limiter.TryAdmit(7, out bool warn3));
        Assert.True(warn3); // first refusal ever seen for this connection
    }

    [Fact]
    public void TryAdmit_IsPerConnectionNotGlobal()
    {
        var limiter = new RpcRateLimiter(maxPerSecond: 1, burst: 1);

        // Connection A spends its one token and is then refused.
        Assert.True(limiter.TryAdmit(1, out _));
        Assert.False(limiter.TryAdmit(1, out _));

        // A global counter would already be exhausted here. A fresh connection
        // must still get its own full bucket - this is the exact property that
        // stops one flooding peer starving every other caller of the same method.
        Assert.True(limiter.TryAdmit(2, out _));
    }

    [Fact]
    public void TryAdmit_WarnsOnlyOnceForRepeatedRefusalsFromTheSameConnection()
    {
        var limiter = new RpcRateLimiter(maxPerSecond: 1, burst: 1);
        Assert.True(limiter.TryAdmit(3, out _));

        Assert.False(limiter.TryAdmit(3, out bool warnFirstRefusal));
        Assert.True(warnFirstRefusal);

        // A flood keeps calling; only the FIRST refusal warns, or the flood
        // becomes a logging flood too.
        Assert.False(limiter.TryAdmit(3, out bool warnSecondRefusal));
        Assert.False(warnSecondRefusal);
        Assert.False(limiter.TryAdmit(3, out bool warnThirdRefusal));
        Assert.False(warnThirdRefusal);
    }

    [Fact]
    public void TryAdmit_RefusalReturnsFalseRatherThanThrowing()
    {
        var limiter = new RpcRateLimiter(maxPerSecond: 1, burst: 1);
        Assert.True(limiter.TryAdmit(9, out _));

        // A refused call is a dispatcher decision, never an exception - a peer's
        // send rate is not a script bug, and InvokeNetRpc must never let anything
        // escape across the native boundary.
        Exception? thrown = Record.Exception(() => limiter.TryAdmit(9, out _));
        Assert.Null(thrown);
    }

    [Fact]
    public void NetRpcAttribute_DefaultsToUnlimited()
    {
        // No MaxPerSecond set at all - the shape of every [NetRpc] method written
        // before this batch, and the one that must keep behaving as "no limit".
        var attribute = new NetRpcAttribute(NetRpcTarget.Server);
        Assert.Equal(0, attribute.MaxPerSecond);
        Assert.Equal(0, attribute.Burst);
    }
}

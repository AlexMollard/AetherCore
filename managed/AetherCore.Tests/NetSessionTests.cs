using AetherCore;
using Xunit;

namespace AetherCore.Tests;

/// <summary>Address parsing, the name policy, and the host/join intent pairing.</summary>
public sealed class NetSessionTests : SdkTestBase
{
    // ── ParseAddress ────────────────────────────────────────────────────────────

    [Fact]
    public void ParseAddress_EmptyIsLoopback()
    {
        Assert.Equal(("127.0.0.1", (ushort)7777), NetSession.ParseAddress(string.Empty, 7777));
    }

    [Fact]
    public void ParseAddress_WhitespaceIsLoopback()
    {
        Assert.Equal(("127.0.0.1", (ushort)7777), NetSession.ParseAddress("   ", 7777));
    }

    [Fact]
    public void ParseAddress_NullIsLoopback()
    {
        Assert.Equal(("127.0.0.1", (ushort)7777), NetSession.ParseAddress(null!, 7777));
    }

    [Fact]
    public void ParseAddress_BareHostTakesTheDefaultPort()
    {
        Assert.Equal(("192.168.0.4", (ushort)7777), NetSession.ParseAddress("192.168.0.4", 7777));
    }

    [Fact]
    public void ParseAddress_HostAndPortAreSplit()
    {
        Assert.Equal(("192.168.0.4", (ushort)9999), NetSession.ParseAddress("192.168.0.4:9999", 7777));
    }

    [Fact]
    public void ParseAddress_TrimsSurroundingWhitespace()
    {
        Assert.Equal(("example.test", (ushort)9000), NetSession.ParseAddress("  example.test:9000  ", 7777));
    }

    [Fact]
    public void ParseAddress_TrailingColonIsNoPort()
    {
        // A half-typed "host:" must still connect somewhere rather than resolve to a
        // host with an empty port.
        Assert.Equal(("example.test:", (ushort)7777), NetSession.ParseAddress("example.test:", 7777));
    }

    [Fact]
    public void ParseAddress_UnparseablePortFallsBackWithoutLosingTheHost()
    {
        Assert.Equal(("example.test", (ushort)7777), NetSession.ParseAddress("example.test:abc", 7777));
    }

    [Fact]
    public void ParseAddress_OutOfRangePortFallsBackWithoutLosingTheHost()
    {
        // 70000 does not fit a ushort; the documented rule is that a typo in the port is
        // not a reason to reject a good address.
        Assert.Equal(("example.test", (ushort)7777), NetSession.ParseAddress("example.test:70000", 7777));
    }

    [Fact]
    public void ParseAddress_LeadingColonIsTakenWholeAsTheAddress()
    {
        Assert.Equal((":9999", (ushort)7777), NetSession.ParseAddress(":9999", 7777));
    }

    [Fact]
    public void ParseAddress_PortIsReadFromTheLastColon()
    {
        // LastIndexOf, so a host that itself contains a colon keeps everything before
        // the final one.
        Assert.Equal(("a:b", (ushort)80), NetSession.ParseAddress("a:b:80", 7777));
    }

    // ── LocalPlayerName ─────────────────────────────────────────────────────────

    [Fact]
    public void LocalPlayerName_DefaultsToPlayer()
    {
        Assert.Equal(NetSession.DefaultPlayerName, NetSession.LocalPlayerName);
    }

    [Fact]
    public void LocalPlayerName_IsTrimmed()
    {
        NetSession.LocalPlayerName = "  Alice  ";
        Assert.Equal("Alice", NetSession.LocalPlayerName);
    }

    [Fact]
    public void LocalPlayerName_BlankBecomesTheDefault()
    {
        NetSession.LocalPlayerName = "Alice";
        NetSession.LocalPlayerName = "   ";
        Assert.Equal(NetSession.DefaultPlayerName, NetSession.LocalPlayerName);
    }

    [Fact]
    public void LocalPlayerName_NullBecomesTheDefault()
    {
        NetSession.LocalPlayerName = "Alice";
        NetSession.LocalPlayerName = null!;
        Assert.Equal(NetSession.DefaultPlayerName, NetSession.LocalPlayerName);
    }

    // ── StatusMessage ───────────────────────────────────────────────────────────

    [Fact]
    public void TakeStatusMessage_ReturnsAndClears()
    {
        NetSession.StatusMessage = "Host disconnected";
        Assert.Equal("Host disconnected", NetSession.TakeStatusMessage());
        Assert.Equal(string.Empty, NetSession.StatusMessage);
        Assert.Equal(string.Empty, NetSession.TakeStatusMessage());
    }

    // ── BeginHost / BeginJoin ───────────────────────────────────────────────────

    [Fact]
    public void BeginHost_ClearsTheJoinIntentAndTheAddressToReconnectTo()
    {
        NetSession.JoinRequested = true;
        NetSession.HostAddress = "10.0.0.1";
        NetSession.HostPort = 1234;

        Assert.True(NetSession.BeginHost(7777, 3));

        Assert.False(NetSession.JoinRequested);
        Assert.Equal(string.Empty, NetSession.HostAddress);
        Assert.Equal((ushort)0, NetSession.HostPort);
    }

    [Fact]
    public void BeginHost_LeavesEverythingAloneWhenThePortCouldNotBeOpened()
    {
        Engine.HostSucceeds = false;
        NetSession.JoinRequested = true;
        NetSession.HostAddress = "10.0.0.1";
        NetSession.HostPort = 1234;

        Assert.False(NetSession.BeginHost(7777, 3));

        Assert.True(NetSession.JoinRequested);
        Assert.Equal("10.0.0.1", NetSession.HostAddress);
        Assert.Equal((ushort)1234, NetSession.HostPort);
    }

    [Fact]
    public void BeginJoin_RecordsTheIntentAndWhereToComeBackTo()
    {
        Assert.True(NetSession.BeginJoin("10.0.0.1", 1234));

        Assert.True(NetSession.JoinRequested);
        Assert.Equal("10.0.0.1", NetSession.HostAddress);
        Assert.Equal((ushort)1234, NetSession.HostPort);
        Assert.Equal(("10.0.0.1", 1234), Assert.Single(Engine.ConnectAttempts));
    }

    [Fact]
    public void BeginJoin_RecordsNothingWhenTheAttemptCouldNotStart()
    {
        Engine.ConnectSucceeds = false;

        Assert.False(NetSession.BeginJoin("10.0.0.1", 1234));

        Assert.False(NetSession.JoinRequested);
        Assert.Equal(string.Empty, NetSession.HostAddress);
        Assert.Equal((ushort)0, NetSession.HostPort);
    }
}

using System;
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
    public void ParseAddress_MultipleUnbracketedColonsAreTakenAsABareIPv6Literal()
    {
        // Splitting an unbracketed multi-colon string on its LAST colon is exactly the
        // bug that mangles IPv6 literals ("::1" -> host ":", port 1). The discriminator
        // is colon count, not position: two or more unbracketed colons means there is no
        // port to split off, whatever the text actually is.
        Assert.Equal(("a:b:80", (ushort)7777), NetSession.ParseAddress("a:b:80", 7777));
    }

    // ── ParseAddress: bare IPv6 ─────────────────────────────────────────────────

    [Fact]
    public void ParseAddress_BareIPv6LoopbackHasNoPortToSplitOff()
    {
        Assert.Equal(("::1", (ushort)7777), NetSession.ParseAddress("::1", 7777));
    }

    [Fact]
    public void ParseAddress_BareIPv6LinkLocalHasNoPortToSplitOff()
    {
        Assert.Equal(("fe80::1", (ushort)7777), NetSession.ParseAddress("fe80::1", 7777));
    }

    [Fact]
    public void ParseAddress_BareIPv6FullFormHasNoPortToSplitOff()
    {
        Assert.Equal(("2001:db8::8a2e:370:7334", (ushort)7777),
            NetSession.ParseAddress("2001:db8::8a2e:370:7334", 7777));
    }

    // ── ParseAddress: bracketed IPv6 ────────────────────────────────────────────

    [Fact]
    public void ParseAddress_BracketedIPv6WithPortStripsTheBracketsAndSplitsThePort()
    {
        Assert.Equal(("::1", (ushort)7777), NetSession.ParseAddress("[::1]:7777", 9999));
    }

    [Fact]
    public void ParseAddress_BracketedIPv6WithoutPortTakesTheDefault()
    {
        Assert.Equal(("::1", (ushort)7777), NetSession.ParseAddress("[::1]", 7777));
    }

    [Fact]
    public void ParseAddress_BracketedIPv6FullFormWithPortStripsTheBrackets()
    {
        Assert.Equal(("2001:db8::8a2e:370:7334", (ushort)9000),
            NetSession.ParseAddress("[2001:db8::8a2e:370:7334]:9000", 7777));
    }

    // ── ParseAddress: malformed ─────────────────────────────────────────────────

    [Fact]
    public void ParseAddress_UnclosedBracketIsTakenWholeAsTheAddress()
    {
        // No closing bracket at all - this cannot be split, so it is treated like any
        // other address this method cannot make sense of: kept whole.
        Assert.Equal(("[::1", (ushort)7777), NetSession.ParseAddress("[::1", 7777));
    }

    [Fact]
    public void ParseAddress_BracketedIPv6WithUnparseablePortFallsBackWithoutLosingTheHost()
    {
        // Same rule as the unbracketed form: a port typo does not cost the address.
        Assert.Equal(("::1", (ushort)7777), NetSession.ParseAddress("[::1]:notaport", 7777));
    }

    [Fact]
    public void ParseAddress_StrayClosingBracketWithNoOpenerIsOrdinaryHostText()
    {
        // Only a LEADING '[' engages the bracket-parsing path; this string never opens
        // one, so it is read as an ordinary "host:port" whose host happens to be "]".
        Assert.Equal(("]", (ushort)7777), NetSession.ParseAddress("]:7777", 9999));
    }

    // ── ParseAddress: hostnames unaffected ──────────────────────────────────────

    [Fact]
    public void ParseAddress_BareHostnameTakesTheDefaultPort()
    {
        Assert.Equal(("example.test", (ushort)7777), NetSession.ParseAddress("example.test", 7777));
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

    // ── BeginHostWithCode / BeginJoinByCode ─────────────────────────────────────

    [Fact]
    public void BeginHostWithCode_MintsACodeWhenNoneIsGivenAndHasNowhereToReconnectTo()
    {
        Engine.RoomCodeToMint = "ABCDEF";
        NetSession.JoinRequested = true;
        NetSession.HostAddress = "10.0.0.1";
        NetSession.HostPort = 1234;
        NetSession.HostRoomCode = "STALE1";

        (bool started, string code) = NetSession.BeginHostWithCode();

        Assert.True(started);
        Assert.Equal("ABCDEF", code);
        Assert.False(NetSession.JoinRequested);
        Assert.Equal(string.Empty, NetSession.HostAddress);
        Assert.Equal((ushort)0, NetSession.HostPort);
        Assert.Equal(string.Empty, NetSession.HostRoomCode); // a host has nowhere to reconnect to
        Assert.Equal(("ABCDEF", 0, 32), Assert.Single(Engine.HostWithCodeAttempts));
    }

    [Fact]
    public void BeginHostWithCode_HostsUnderTheSuppliedCodeInsteadOfMintingOne()
    {
        (bool started, string code) = NetSession.BeginHostWithCode("PA1R01", port: 7000, maxConnections: 5);

        Assert.True(started);
        Assert.Equal("PA1R01", code);
        Assert.Equal(("PA1R01", 7000, 5), Assert.Single(Engine.HostWithCodeAttempts));
    }

    [Fact]
    public void BeginHostWithCode_LeavesEverythingAloneWhenTheLocalBindFailed()
    {
        Engine.HostWithCodeSucceeds = false;
        NetSession.JoinRequested = true;
        NetSession.HostAddress = "10.0.0.1";
        NetSession.HostPort = 1234;
        NetSession.HostRoomCode = "STALE1";

        (bool started, string code) = NetSession.BeginHostWithCode("PA1R01");

        Assert.False(started);
        Assert.Equal("PA1R01", code); // the code attempted is still reported, for logging
        Assert.True(NetSession.JoinRequested);
        Assert.Equal("10.0.0.1", NetSession.HostAddress);
        Assert.Equal((ushort)1234, NetSession.HostPort);
        Assert.Equal("STALE1", NetSession.HostRoomCode);
    }

    [Fact]
    public void BeginJoinByCode_RecordsTheCodeAsTheReconnectTargetInsteadOfAnAddress()
    {
        Assert.True(NetSession.BeginJoinByCode("PA1R01"));

        Assert.True(NetSession.JoinRequested);
        Assert.Equal(string.Empty, NetSession.HostAddress);
        Assert.Equal((ushort)0, NetSession.HostPort);
        Assert.Equal("PA1R01", NetSession.HostRoomCode);
        Assert.Equal("PA1R01", Assert.Single(Engine.JoinByCodeAttempts));
    }

    [Fact]
    public void BeginJoinByCode_RecordsNothingWhenTheAttemptCouldNotStart()
    {
        Engine.JoinByCodeSucceeds = false;

        Assert.False(NetSession.BeginJoinByCode("PA1R01"));

        Assert.False(NetSession.JoinRequested);
        Assert.Equal(string.Empty, NetSession.HostRoomCode);
    }

    [Fact]
    public void BeginHost_ClearsAPreviousRoomCode()
    {
        // Mutual exclusivity: a direct host has nowhere to reconnect to, by address OR
        // by a code left over from an earlier code-based session.
        NetSession.HostRoomCode = "STALE1";

        Assert.True(NetSession.BeginHost(7777, 3));

        Assert.Equal(string.Empty, NetSession.HostRoomCode);
    }

    [Fact]
    public void BeginJoin_ClearsAPreviousRoomCode()
    {
        // A direct-address join reconnects by address, not by a code from an earlier
        // session - the two reconnect targets are mutually exclusive.
        NetSession.HostRoomCode = "STALE1";

        Assert.True(NetSession.BeginJoin("10.0.0.1", 1234));

        Assert.Equal(string.Empty, NetSession.HostRoomCode);
    }

    // ── Signalling defaulting rule ───────────────────────────────────────────────

    [Fact]
    public void BeginHostWithCode_NeedsNoSignalingSelectionMadeFirst()
    {
        // Neither Net.UseLanSignaling nor Net.UseRendezvousSignaling has been called.
        // The engine's own connect ladder already defaults to LAN broadcast with zero
        // configuration (NetTraversalSession's m_backend default), so NetSession must
        // not gate hosting on a selection, and must not make one on the caller's
        // behalf either - an implicit choice here would permanently override a future
        // settings-driven rendezvous default the same way an explicit call would.
        (bool started, string _) = NetSession.BeginHostWithCode();

        Assert.True(started);
    }

    [Fact]
    public void BeginJoinByCode_NeedsNoSignalingSelectionMadeFirst()
    {
        Assert.True(NetSession.BeginJoinByCode("PA1R01"));
    }

    // ── FFI enum ordinals ────────────────────────────────────────────────────────

    [Fact]
    public void NetTraversalState_OrdinalsMatchTheEngineEnum()
    {
        // aether_net_traversal_state crosses the native boundary as a plain int cast
        // from the C++ TraversalState enum (NetExports.cpp), so these ordinals have
        // to match that enum member-for-member. A C# test cannot read a C++ enum:
        // this pins the MANAGED side only. The native side is pinned where it is
        // visible, by static_asserts in src/app/scripting/interop/NetExports.cpp
        // that stop the build on a native reorder - so a change on either side
        // fails somewhere loud instead of every peer silently reporting the wrong
        // traversal state. Change both sides together or not at all.
        Assert.Equal(0, (int)NetTraversalState.Idle);
        Assert.Equal(1, (int)NetTraversalState.Mapping);
        Assert.Equal(2, (int)NetTraversalState.Signaling);
        Assert.Equal(3, (int)NetTraversalState.Punching);
        Assert.Equal(4, (int)NetTraversalState.Relaying);
        Assert.Equal(5, (int)NetTraversalState.Connecting);
        Assert.Equal(6, (int)NetTraversalState.Connected);
        Assert.Equal(7, (int)NetTraversalState.Failed);
    }

    [Fact]
    public void NetRpcTarget_OrdinalsMatchTheEngineEnum()
    {
        // Same rule as NetTraversalState: Net.Call hands the target to
        // aether_net_call_rpc as a raw int, and the native half (NetRpcExports.cpp)
        // puts it on the wire as a raw byte the receiver checks for direction -
        // compared by ordinal, never by name. This pins the MANAGED side only (a
        // C# test cannot read aether::net::NetRpcTarget in src/app/net/NetRpc.hpp);
        // the native side is pinned by static_asserts in
        // src/app/scripting/interop/NetExports.cpp.
        Assert.Equal(0, (int)NetRpcTarget.Server);
        Assert.Equal(1, (int)NetRpcTarget.Client);
        Assert.Equal(2, (int)NetRpcTarget.Multicast);
    }

    // ── Net port arguments ───────────────────────────────────────────────────────

    [Fact]
    public void Net_PortsThatDoNotFitAUshortAreRejectedNotTruncated()
    {
        // (ushort)70000 is 4464: without the check, Net.Connect(host, 70000) dials
        // the WRONG port and reports the attempt started, and Net.Host(-1) binds
        // 65535. Each throw happens before any P/Invoke, so this needs no engine
        // behind it. The accept path cannot be tested here - a port that fits
        // crosses into the native engine.
        Assert.Throws<ArgumentOutOfRangeException>(() => Net.Host(70000));
        Assert.Throws<ArgumentOutOfRangeException>(() => Net.Connect("10.0.0.1", -1));
        Assert.Throws<ArgumentOutOfRangeException>(() => Net.HostWithCode("ABCD", 65536));
        Assert.Throws<ArgumentOutOfRangeException>(() => Net.ConfigureRelay("turn.example.com", 70000, "u", "p"));
    }
}

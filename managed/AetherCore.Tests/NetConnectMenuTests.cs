using AetherCore;
using Xunit;

namespace AetherCore.Tests;

/// <summary>
/// The connect ceremony this collapses: minting/accepting a room code, the
/// <see cref="Net.ReplicationReady"/> flip around a join, narrating the connect ladder,
/// and handing off to the arena only once it is safe to. Also pins that a game which
/// never uses this at all - the offline, no-session path - is completely unaffected.
/// </summary>
public sealed class NetConnectMenuTests : SdkTestBase
{
    private NetConnectMenu NewMenu(string arenaScene = "Arena")
    {
        NetConnectMenu menu = new() { ArenaScene = arenaScene };
        menu.Self = Engine.NewEntity();
        return menu;
    }

    // ── Hosting ─────────────────────────────────────────────────────────────────

    [Fact]
    public void HostingMintsAFreshCodeAndNeverTouchesReplicationReady()
    {
        // A host originates the join burst rather than receiving one, so there is
        // nothing here for ReplicationReady to protect against.
        Engine.RoomCodeToMint = "PA1R01";
        NetConnectMenu menu = NewMenu();

        bool started = menu.Host();

        Assert.True(started);
        Assert.Equal("PA1R01", menu.HostedRoomCode);
        Assert.True(menu.IsHosting);
        Assert.Empty(Engine.ReplicationReadyWrites);
    }

    [Fact]
    public void HostingReportsWhyOnFailure()
    {
        Engine.HostWithCodeSucceeds = false;
        NetConnectMenu menu = NewMenu();

        bool started = menu.Host();

        Assert.False(started);
        Assert.False(menu.IsHosting);
        Assert.Contains("Could not host", menu.StatusText);
    }

    [Fact]
    public void PressingEnterWhileHostingLoadsTheArenaExactlyOnce()
    {
        NetConnectMenu menu = NewMenu("Arena");
        menu.Host();
        Engine.PressedKeys.Add(Key.Enter);

        menu.OnUpdate(1.0f / 60.0f);
        // A second tick must not re-fire: IsHosting already dropped, which is what
        // keeps a held Enter key from loading the scene twice.
        menu.OnUpdate(1.0f / 60.0f);

        Assert.Equal(new[] { "Arena" }, Engine.ScenesLoaded);
        Assert.False(menu.IsHosting);
    }

    // ── Joining: the ReplicationReady transition ───────────────────────────────

    [Fact]
    public void JoinSetsReplicationReadyFalseBeforeTheAttemptStarts()
    {
        // The whole reason this class exists: get this line right so a client's own
        // join burst is not built into the menu and destroyed a frame later.
        NetConnectMenu menu = NewMenu();

        bool started = menu.Join("PA1R01");

        Assert.True(started);
        Assert.Equal(new[] { false }, Engine.ReplicationReadyWrites);
    }

    [Fact]
    public void SuccessfulJoinLoadsTheArenaWithoutEverSettingReplicationReadyBackTrue()
    {
        // NetSessionDirector.OnAttach is what sets it true, once this peer is actually
        // standing in the arena - not this class, and not a moment earlier. A stray
        // "true" written here would race the scene switch and reopen the exact bug
        // ReplicationReady exists to prevent.
        NetConnectMenu menu = NewMenu("Arena");
        menu.Join("PA1R01");

        Engine.IsConnected = true;
        menu.OnUpdate(1.0f / 60.0f);

        Assert.False(menu.IsJoining);
        Assert.Equal(new[] { "Arena" }, Engine.ScenesLoaded);
        Assert.Equal(new[] { false }, Engine.ReplicationReadyWrites);
    }

    [Fact]
    public void FailedTraversalAbandonsTheJoinAndRestoresReplicationReady()
    {
        NetConnectMenu menu = NewMenu();
        menu.Join("PA1R01");

        Engine.TraversalState = NetTraversalState.Failed;
        Engine.TraversalError = "symmetric NAT, no relay configured";
        menu.OnUpdate(1.0f / 60.0f);

        Assert.False(menu.IsJoining);
        Assert.Equal(new[] { false, true }, Engine.ReplicationReadyWrites);
        Assert.Equal(1, Engine.DisconnectCount);
        Assert.Contains("symmetric NAT", menu.StatusText);
    }

    [Fact]
    public void JoinTimesOutAndRestoresReplicationReady()
    {
        NetConnectMenu menu = NewMenu();
        menu.Join("PA1R01");

        menu.OnUpdate(menu.JoinTimeoutSeconds - 0.01f);
        Assert.True(menu.IsJoining); // not yet

        menu.OnUpdate(0.02f);

        Assert.False(menu.IsJoining);
        Assert.Equal(new[] { false, true }, Engine.ReplicationReadyWrites);
        Assert.Contains("Timed out", menu.StatusText);
    }

    [Fact]
    public void EscapeCancelsAnInFlightJoinAndRestoresReplicationReady()
    {
        NetConnectMenu menu = NewMenu();
        menu.Join("PA1R01");

        Engine.PressedKeys.Add(Key.Escape);
        menu.OnUpdate(1.0f / 60.0f);

        Assert.False(menu.IsJoining);
        Assert.Equal(new[] { false, true }, Engine.ReplicationReadyWrites);
        Assert.Equal(1, Engine.DisconnectCount);
    }

    [Fact]
    public void JoinFailingToStartRestoresReplicationReadyImmediately()
    {
        Engine.JoinByCodeSucceeds = false;
        NetConnectMenu menu = NewMenu();

        bool started = menu.Join("PA1R01");

        Assert.False(started);
        Assert.False(menu.IsJoining);
        Assert.Equal(new[] { false, true }, Engine.ReplicationReadyWrites);
    }

    [Fact]
    public void BlankCodeIsRefusedWithoutTouchingReplicationReady()
    {
        NetConnectMenu menu = NewMenu();

        bool started = menu.Join("   ");

        Assert.False(started);
        Assert.Empty(Engine.ReplicationReadyWrites);
    }

    [Fact]
    public void ASecondAttemptIsRefusedWhileOneIsAlreadyInFlight()
    {
        NetConnectMenu menu = NewMenu();
        menu.Join("PA1R01");

        bool secondJoin = menu.Join("OTHER1");
        bool hostWhileJoining = menu.Host();

        Assert.False(secondJoin);
        Assert.False(hostWhileJoining);
        Assert.Equal(new[] { "PA1R01" }, Engine.JoinByCodeAttempts);
    }

    // ── Offline: unaffected by this class entirely ─────────────────────────────

    [Fact]
    public void ArenaOpenedDirectlyWithNoConnectMenuAtAllStillPlaysSolo()
    {
        // This is the property most likely to break: a NetConnectMenu never existed in
        // this scenario at all, exactly like a level opened straight from the editor.
        // NetSessionDirector's own offline fallback must be completely untouched.
        var director = new PlainDirector { PlayerPrefab = "player" };
        director.Self = Engine.NewEntity();

        director.OnAttach();
        director.OnUpdate(1.0f / 60.0f);

        Assert.Single(Engine.Instantiated);
        Assert.Equal("player", Engine.Instantiated[0].Prefab);
        Assert.True(director.LocalPlayer.IsValid);
    }

    [Fact]
    public void PlaySoloSkipsTheSessionAndGoesStraightToTheArena()
    {
        NetConnectMenu menu = NewMenu("Arena");

        menu.PlaySolo();

        Assert.Equal(new[] { "Arena" }, Engine.ScenesLoaded);
        Assert.Empty(Engine.HostWithCodeAttempts);
        Assert.Empty(Engine.JoinByCodeAttempts);
    }

    private sealed class PlainDirector : NetSessionDirector { }
}

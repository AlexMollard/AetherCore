using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;
using Xunit;

namespace AetherCore.Tests;

/// <summary>
/// The session lifecycle: who gets spawned, who gets announced and when, what a link
/// ending means, and how far a reconnect is allowed to go.
/// </summary>
public sealed class NetSessionDirectorTests : SdkTestBase
{
    /// <summary>A game that records what it was told, and can refuse to take the news.</summary>
    private sealed class RecordingDirector : NetSessionDirector
    {
        public readonly List<string> Joined = new();
        public readonly List<string> Left = new();
        public readonly List<Entity> JoinedEntities = new();
        public readonly List<Entity> LeftEntities = new();

        /// <summary>While true, every notice is refused and must be re-offered.</summary>
        public bool RefuseNotices;

        /// <summary>What <see cref="WantsToLeave"/> answers.</summary>
        public bool LeaveRequested;

        protected override bool WantsToLeave() => LeaveRequested;

        protected override bool AnnouncePlayerJoined(NetSessionPlayer player)
        {
            if (RefuseNotices)
            {
                return false;
            }
            Joined.Add(player.Name);
            JoinedEntities.Add(player.Entity);
            return true;
        }

        protected override bool AnnouncePlayerLeft(NetSessionPlayer player)
        {
            if (RefuseNotices)
            {
                return false;
            }
            Left.Add(player.Name);
            LeftEntities.Add(player.Entity);
            return true;
        }
    }

    private RecordingDirector NewDirector(string returnScene = "Title")
    {
        RecordingDirector director = new() { ReturnScene = returnScene, PlayerPrefab = "player" };
        director.Self = Engine.NewEntity();
        return director;
    }

    /// <summary>Tick the director for one frame.</summary>
    private static void Tick(NetSessionDirector director, float deltaTime = 1.0f / 60.0f)
        => director.OnUpdate(deltaTime);

    /// <summary>Run <paramref name="frames"/> frames of <paramref name="deltaTime"/>.</summary>
    private static void Tick(NetSessionDirector director, int frames, float deltaTime)
    {
        for (int i = 0; i < frames; i++)
        {
            director.OnUpdate(deltaTime);
        }
    }

    /// <summary>Tick until the director gives up and loads its return scene, or run out
    /// of patience.</summary>
    /// <remarks>
    /// Stopping at the scene load is what the engine does: <c>Scene.Load</c> tears the
    /// scene down at the end of that frame's script update, so this script does not get
    /// another tick. Ticking on past it would be testing a director that cannot exist.
    /// </remarks>
    private bool TickUntilReturned(NetSessionDirector director, int maxFrames, float deltaTime)
    {
        for (int i = 0; i < maxFrames; i++)
        {
            director.OnUpdate(deltaTime);
            if (Engine.ScenesLoaded.Count > 0)
            {
                return true;
            }
        }
        return false;
    }

    // ── Attach ──────────────────────────────────────────────────────────────────

    [Fact]
    public void AttachingDeclaresThisPeerReadyToReceiveReplicatedEntities()
    {
        // A game that forgets loses every replicated player silently, which is why the
        // SDK does it rather than each project.
        RecordingDirector director = NewDirector();

        director.OnAttach();

        Assert.Equal(new[] { true }, Engine.ReplicationReadyWrites);
    }

    [Fact]
    public void AttachingWithNothingLiveClearsAStaleJoinIntent()
    {
        // The static survives the editor's Play/Stop cycle; left set, a genuine
        // single-player session would take the client branch and spawn nobody.
        NetSession.JoinRequested = true;
        RecordingDirector director = NewDirector();

        director.OnAttach();

        Assert.False(NetSession.JoinRequested);
    }

    [Fact]
    public void AttachingWhileAJoinIsStillInFlightKeepsTheIntent()
    {
        NetSession.JoinRequested = true;
        Engine.IsClient = true;
        RecordingDirector director = NewDirector();

        director.OnAttach();

        Assert.True(NetSession.JoinRequested);
    }

    [Fact]
    public void AttachingAfterARefusalKeepsTheIntentSoTheRefusalCanBeReported()
    {
        NetSession.JoinRequested = true;
        Engine.DisconnectReason = "Server is full";
        RecordingDirector director = NewDirector();

        director.OnAttach();

        Assert.True(NetSession.JoinRequested);
    }

    // ── Host: spawning ──────────────────────────────────────────────────────────

    [Fact]
    public void TheHostSpawnsItsOwnPlayerUnderItsChosenName()
    {
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        Engine.PlaceEntity("Spawn0", new Vector3(1.0f, 2.0f, 0.0f));
        RecordingDirector director = NewDirector();
        director.OnAttach();

        Tick(director);

        (string prefab, Vector3 position, uint owner) = Assert.Single(Engine.Spawned);
        Assert.Equal("player", prefab);
        Assert.Equal(new Vector3(1.0f, 2.0f, 0.0f), position);
        Assert.Equal(0u, owner);
        Assert.Equal("Alice", Assert.Single(Engine.NamesSet).Name);
        Assert.True(director.LocalPlayer.IsValid);
    }

    [Fact]
    public void TheHostSpawnsItsOwnPlayerExactlyOnce()
    {
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();

        Tick(director, 10, 1.0f / 60.0f);

        Assert.Single(Engine.Spawned);
    }

    [Fact]
    public void AConnectionThatAppearsGetsAPlayerAtItsOwnSpawnMarker()
    {
        Engine.IsHost = true;
        Engine.PlaceEntity("Spawn0", new Vector3(1.0f, 0.0f, 0.0f));
        Engine.PlaceEntity("Spawn1", new Vector3(5.0f, 0.0f, 0.0f));
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);

        Engine.Connections.Add(1);
        Tick(director);

        Assert.Equal(2, Engine.Spawned.Count);
        Assert.Equal(1u, Engine.Spawned[1].Owner);
        Assert.Equal(new Vector3(5.0f, 0.0f, 0.0f), Engine.Spawned[1].Position);
    }

    [Fact]
    public void AConnectionThatIsStillThereIsNotSpawnedAgain()
    {
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Engine.Connections.Add(1);

        Tick(director, 10, 1.0f / 60.0f);

        Assert.Equal(2, Engine.Spawned.Count); // the host's own, plus connection 1
    }

    [Fact]
    public void AMissingSpawnMarkerPutsThePlayerAtTheOrigin()
    {
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();

        Tick(director);

        Assert.Equal(Vector3.Zero, Assert.Single(Engine.Spawned).Position);
    }

    [Fact]
    public void ADepartedConnectionGivesItsSpawnMarkerBack()
    {
        Engine.IsHost = true;
        Engine.PlaceEntity("Spawn0", new Vector3(0.0f, 0.0f, 0.0f));
        Engine.PlaceEntity("Spawn1", new Vector3(5.0f, 0.0f, 0.0f));
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Engine.Connections.Add(1);
        Tick(director);

        Engine.Connections.Remove(1);
        Tick(director);
        Engine.Connections.Add(2);
        Tick(director);

        // Connection 2 lands on Spawn1, the marker connection 1 handed back - not on a
        // third marker, and not stacked on the host.
        Assert.Equal(3, Engine.Spawned.Count);
        Assert.Equal(new Vector3(5.0f, 0.0f, 0.0f), Engine.Spawned[2].Position);
    }

    [Fact]
    public void PlayersBeyondTheMarkerCountShareOneRatherThanNotSpawning()
    {
        Engine.IsHost = true;
        Engine.PlaceEntity("Spawn0", new Vector3(0.0f, 0.0f, 0.0f));
        Engine.PlaceEntity("Spawn1", new Vector3(5.0f, 0.0f, 0.0f));
        RecordingDirector director = NewDirector();
        director.SpawnPointCount = 2;
        director.SpawnSpread = 1.25f;
        director.OnAttach();
        Tick(director);

        Engine.Connections.Add(1);
        Engine.Connections.Add(2); // one more than there are markers
        Tick(director);

        Assert.Equal(3, Engine.Spawned.Count);
        // The overflow player lands BESIDE an occupied marker, not inside whoever is
        // already standing on it.
        Assert.Equal(new Vector3(1.25f, 0.0f, 0.0f), Engine.Spawned[2].Position);
    }

    [Fact]
    public void AFailedSpawnDoesNotBurnTheMarkerForTheRestOfTheSession()
    {
        Engine.IsHost = true;
        Engine.PlaceEntity("Spawn0", new Vector3(1.0f, 0.0f, 0.0f));
        Engine.SpawnSucceeds = false;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);

        Engine.SpawnSucceeds = true;
        Engine.Connections.Add(1);
        Tick(director);

        // Spawn0 was released by the failure, so the next player gets it.
        Assert.Equal(new Vector3(1.0f, 0.0f, 0.0f), Engine.Spawned[1].Position);
    }

    // ── The name gate ───────────────────────────────────────────────────────────

    [Fact]
    public void NobodyIsAnnouncedUntilARealNameHasArrived()
    {
        // A player prefab is authored nameless and the real name is replicated in a few
        // frames later. Announcing before it lands introduces somebody as nobody.
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Engine.Connections.Add(1);

        Tick(director, 30, 1.0f / 60.0f);

        // The host itself is named from the start and is announced; the joiner, whose
        // name has not replicated, is not - not blank, and not under any stand-in.
        Assert.Equal(new[] { "Alice" }, director.Joined);
    }

    [Fact]
    public void AnAnnouncementNeverCarriesAnEmptyName()
    {
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Engine.Connections.Add(1);
        Tick(director, 5, 1.0f / 60.0f);
        Assert.Equal(new[] { "Alice" }, director.Joined);

        Entity joiner = Engine.Players[1].Entity;
        Engine.NamePlayer(joiner, "Bob");
        Tick(director);

        Assert.Equal(new[] { "Alice", "Bob" }, director.Joined);
        Assert.DoesNotContain(director.Joined, name => name.Length == 0);
    }

    [Fact]
    public void AnAnnouncementNeverCarriesThePlaceholderName()
    {
        // "Player" is what an unnamed peer would fall back to. It must come from the peer
        // choosing it, never from the director filling a gap.
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Engine.Connections.Add(1);

        Tick(director, 30, 1.0f / 60.0f);

        Assert.DoesNotContain(NetSession.DefaultPlayerName, director.Joined);
        Assert.Equal(new[] { "Alice" }, director.Joined);
    }

    [Fact]
    public void EachPlayerIsAnnouncedExactlyOnceHoweverLongTheSessionRuns()
    {
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);
        Engine.Connections.Add(1);
        Tick(director);
        Engine.NamePlayer(Engine.Players[1].Entity, "Bob");

        Tick(director, 60, 1.0f / 60.0f);

        Assert.Equal(new[] { "Alice", "Bob" }, director.Joined);
    }

    [Fact]
    public void AJoinerIsAnnouncedWithTheLiveEntityAndALeaverWithoutOne()
    {
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Engine.Connections.Add(1);
        Tick(director);
        Entity joiner = Engine.Players[1].Entity;
        Engine.NamePlayer(joiner, "Bob");
        Tick(director);

        Engine.Connections.Remove(1);
        Engine.RemovePlayer(joiner); // the framework despawns before scripts run
        Tick(director);

        Assert.Equal(new[] { "Alice", "Bob" }, director.Joined);
        Assert.Equal(joiner, director.JoinedEntities[1]);
        // The entity is already destroyed by the time anybody notices, which is exactly
        // why the roster entry carries the name.
        Assert.Equal(default, Assert.Single(director.LeftEntities));
        Assert.Equal(new[] { "Bob" }, director.Left);
    }

    [Fact]
    public void SomebodyWhoNeverGotANameIsNeverReportedAsLeavingEither()
    {
        // "left" with nobody in front of it names no one.
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Engine.Connections.Add(1);
        Tick(director);
        Entity joiner = Engine.Players[1].Entity;

        Engine.Connections.Remove(1);
        Engine.RemovePlayer(joiner);
        Tick(director);

        Assert.Equal(new[] { "Alice" }, director.Joined);
        Assert.Empty(director.Left);
    }

    // ── The retry contract ──────────────────────────────────────────────────────

    [Fact]
    public void ARefusedNoticeIsHeldAndOfferedAgain()
    {
        // A multicast needs a live script instance on its carrier, and an entity spawned
        // this frame has none for a frame or two.
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.RefuseNotices = true;
        director.OnAttach();

        Tick(director, 5, 1.0f / 60.0f);
        Assert.Empty(director.Joined);

        director.RefuseNotices = false;
        Tick(director);

        Assert.Equal(new[] { "Alice" }, director.Joined);
    }

    [Fact]
    public void ARefusedNoticeIsNotDeliveredTwiceWhenItIsFinallyTaken()
    {
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.RefuseNotices = true;
        director.OnAttach();
        Tick(director, 5, 1.0f / 60.0f);

        director.RefuseNotices = false;
        Tick(director, 5, 1.0f / 60.0f);

        Assert.Equal(new[] { "Alice" }, director.Joined);
    }

    [Fact]
    public void NoticesAreDeliveredOldestFirstAndNeverOutOfOrder()
    {
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.RefuseNotices = true;
        director.OnAttach();
        Tick(director); // Alice joins (held)

        Engine.Connections.Add(1);
        Tick(director);
        Entity bob = Engine.Players[1].Entity;
        Engine.NamePlayer(bob, "Bob");
        Tick(director); // Bob joins (held)

        Engine.Connections.Remove(1);
        Engine.RemovePlayer(bob);
        Tick(director); // Bob leaves (held)

        director.RefuseNotices = false;
        Tick(director);

        Assert.Equal(new[] { "Alice", "Bob" }, director.Joined);
        Assert.Equal(new[] { "Bob" }, director.Left);
    }

    // ── Roster ──────────────────────────────────────────────────────────────────

    [Fact]
    public void TheRosterIsHostFirstThenJoinOrderWhateverOrderTheWorldIsIn()
    {
        // Net.Players is in world order, which is not stable and would shuffle a HUD
        // between frames.
        AsAConnectedClient();
        Engine.AddPlayer(owner: 3, name: "Carol");
        Engine.AddPlayer(owner: 0, name: "Alice");
        Engine.AddPlayer(owner: 1, name: "Bob");
        RecordingDirector director = NewDirector();
        director.OnAttach();

        Tick(director);

        Assert.Equal(new[] { "Alice", "Bob", "Carol" }, ToNames(director.Players));
        Assert.True(director.Players[0].IsHost);
        Assert.False(director.Players[1].IsHost);
    }

    [Fact]
    public void ANamelessPlayerIsSkippedRatherThanListedBlank()
    {
        AsAConnectedClient();
        Engine.AddPlayer(owner: 0, name: "Alice");
        Engine.AddPlayer(owner: 1, name: string.Empty);
        RecordingDirector director = NewDirector();
        director.OnAttach();

        Tick(director);

        Assert.Equal(new[] { "Alice" }, ToNames(director.Players));
    }

    [Fact]
    public void TheRosterIsRebuiltEachFrameRatherThanAccumulated()
    {
        AsAConnectedClient();
        Entity alice = Engine.AddPlayer(owner: 0, name: "Alice");
        Engine.AddPlayer(owner: 1, name: "Bob");
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);
        Assert.Equal(2, director.Players.Count);

        Engine.RemovePlayer(alice);
        Tick(director);

        Assert.Equal(new[] { "Bob" }, ToNames(director.Players));
    }

    [Fact]
    public void ARosterEntryReadsItsPingLiveRatherThanFromASnapshot()
    {
        AsAConnectedClient();
        Entity bob = Engine.AddPlayer(owner: 1, name: "Bob");
        Engine.Pings[bob.Id] = 42;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);

        Assert.Equal(42u, director.Players[0].PingMs);

        Engine.Pings[bob.Id] = 91;
        Assert.Equal(91u, director.Players[0].PingMs);
    }

    [Fact]
    public void ADepartedRosterEntryReportsNoPingRatherThanAStaleOne()
    {
        // The engine would answer for the invalid entity too - with whatever it has for
        // entity 0 - so the guard has to be the thing that stops the read, not luck.
        Engine.Pings[0] = 999;
        NetSessionPlayer departed = new(1, default, "Bob");

        Assert.Equal(0u, departed.PingMs);
    }

    // ── Client: connecting ──────────────────────────────────────────────────────

    [Fact]
    public void AClientSaysSoOnceWhenItIsLetIn()
    {
        // A client's level starts empty until the host's players replicate in, which is
        // exactly when a player concludes nothing worked.
        Engine.IsClient = true;
        Engine.IsConnected = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();

        Tick(director);

        Assert.Equal("Connected.", LastStatusText());
        int statusWrites = Engine.UiCalls.FindAll(c => c.Op == "SetText").Count;
        Tick(director);
        Assert.Equal(statusWrites, Engine.UiCalls.FindAll(c => c.Op == "SetText").Count);
    }

    [Fact]
    public void TheConnectedNoticeTakesItselfDownAgain()
    {
        Engine.IsClient = true;
        Engine.IsConnected = true;
        RecordingDirector director = NewDirector();
        director.NoticeSeconds = 1.5f;
        director.OnAttach();
        Tick(director);
        Assert.Empty(Engine.Destroyed);

        Tick(director, 100, 0.02f); // two seconds

        Assert.Single(Engine.Destroyed);
    }

    [Fact]
    public void AClientWaitingToBeLetInIsToldWhoItIsWaitingFor()
    {
        Engine.IsClient = true;
        NetSession.HostAddress = "10.0.0.1";
        NetSession.HostPort = 7777;
        NetSession.JoinRequested = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();

        Tick(director);

        Assert.Contains("10.0.0.1:7777", LastStatusText());
    }

    [Fact]
    public void AFirstConnectionThatIsNeverAnsweredGivesUpOutLoud()
    {
        Engine.IsClient = true;
        NetSession.JoinRequested = true;
        RecordingDirector director = NewDirector();
        director.ConnectTimeoutSeconds = 8.0f;
        director.OnAttach();

        Assert.True(TickUntilReturned(director, 40, 0.25f)); // up to ten seconds

        Assert.Equal(NetSessionDirector.ConnectTimedOutMessage, NetSession.StatusMessage);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
    }

    [Fact]
    public void AFirstConnectionIsNotAbandonedBeforeItsTimeout()
    {
        Engine.IsClient = true;
        NetSession.JoinRequested = true;
        RecordingDirector director = NewDirector();
        director.ConnectTimeoutSeconds = 8.0f;
        director.OnAttach();

        Tick(director, 20, 0.25f); // five seconds

        Assert.Empty(Engine.ScenesLoaded);
    }

    [Fact]
    public void AnUnreachableAddressIsReportedAsUnreachableRatherThanAsATimeout()
    {
        // Nobody is out there to send a reason, so the transport simply giving up is the
        // only signal there is.
        Engine.IsClient = true; // the attempt is in flight when the level opens
        NetSession.JoinRequested = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);
        Assert.Empty(Engine.ScenesLoaded);

        Engine.IsClient = false; // the transport gave up on its own
        Tick(director);

        Assert.Equal(NetSessionDirector.UnreachableMessage, NetSession.StatusMessage);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
    }

    // ── First connection: by code ───────────────────────────────────────────────

    [Fact]
    public void AJoinByCodeStillPunchingIsNotTreatedAsUnreachable()
    {
        // While the connect ladder punches, the transport reports neither host nor
        // client (see NetTraversalSession's class remarks on JoinInProgress) - the
        // exact signal a direct join's watchdog reads as "gave up on its own". A
        // code-based join must not confuse the two and bail out on the first frame.
        NetSession.HostRoomCode = "PA1R01";
        NetSession.JoinRequested = true;
        Engine.TraversalState = NetTraversalState.Punching;
        RecordingDirector director = NewDirector();
        director.ConnectTimeoutSeconds = 8.0f;
        director.OnAttach();

        Tick(director, 20, 0.1f); // two seconds, well under the timeout

        Assert.Empty(Engine.ScenesLoaded);
    }

    [Fact]
    public void AJoinByCodeThatFailsTraversalReturnsToTheMenuWithTheTraversalError()
    {
        NetSession.HostRoomCode = "PA1R01";
        NetSession.JoinRequested = true;
        Engine.TraversalState = NetTraversalState.Punching;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);
        Assert.Empty(Engine.ScenesLoaded);

        Engine.TraversalState = NetTraversalState.Failed;
        Engine.TraversalError = "No relay is configured for a symmetric NAT";
        Tick(director);

        Assert.Equal("No relay is configured for a symmetric NAT", NetSession.StatusMessage);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
    }

    [Fact]
    public void AJoinByCodeThatNeverAnswersGivesUpOutLoud()
    {
        NetSession.HostRoomCode = "PA1R01";
        NetSession.JoinRequested = true;
        Engine.TraversalState = NetTraversalState.Signaling; // nobody has answered the room yet
        RecordingDirector director = NewDirector();
        director.ConnectTimeoutSeconds = 8.0f;
        director.OnAttach();

        Assert.True(TickUntilReturned(director, 40, 0.25f)); // up to ten seconds

        Assert.Equal(NetSessionDirector.ConnectTimedOutMessage, NetSession.StatusMessage);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
    }

    [Fact]
    public void AJoinByCodeConnectingSucceedsTheSameWayADirectJoinDoes()
    {
        NetSession.HostRoomCode = "PA1R01";
        NetSession.JoinRequested = true;
        Engine.TraversalState = NetTraversalState.Punching;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);

        Engine.TraversalState = NetTraversalState.Connected;
        Engine.IsClient = true;
        Engine.IsConnected = true;
        Tick(director);

        Assert.Equal("Connected.", LastStatusText());
        Assert.Empty(Engine.ScenesLoaded);
    }

    // ── Client: endings ─────────────────────────────────────────────────────────

    [Fact]
    public void AnEndingWithAReasonIsFinalAndCarriesTheReasonToTheMenu()
    {
        Engine.IsClient = true;
        Engine.IsConnected = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);

        Engine.IsClient = false;
        Engine.IsConnected = false;
        Engine.DisconnectReason = "Host closed the session";
        Tick(director);

        Assert.Equal("Host closed the session", NetSession.StatusMessage);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
        Assert.False(director.IsReconnecting);
        Assert.Empty(Engine.ConnectAttempts);
    }

    [Fact]
    public void ARefusalBeforeTheSessionWasEverLiveStillReachesTheMenu()
    {
        NetSession.JoinRequested = true;
        Engine.DisconnectReason = "Server is full";
        RecordingDirector director = NewDirector();
        director.OnAttach();

        Tick(director);

        Assert.Equal("Server is full", NetSession.StatusMessage);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
    }

    [Fact]
    public void AnEndingWithNothingToSayIsTreatedAsAnAccidentAndRetried()
    {
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");

        DropTheLink();
        Tick(director);

        Assert.True(director.IsReconnecting);
        Assert.Contains("Reconnecting", LastStatusText());
        Assert.Empty(Engine.ScenesLoaded);
    }

    [Fact]
    public void ThereIsNothingToReconnectToWhenThisPeerWasHosting()
    {
        // A player who got here by hosting has no address; six seconds of failing would
        // buy nothing.
        RecordingDirector director = InSession(hostAddress: string.Empty);

        DropTheLink();
        Tick(director);

        Assert.False(director.IsReconnecting);
        Assert.Equal(NetSessionDirector.HostDisconnectedMessage, NetSession.StatusMessage);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
    }

    // ── Reconnecting ────────────────────────────────────────────────────────────

    [Fact]
    public void ReconnectingWaitsBeforeTheFirstAttemptRatherThanBurningItImmediately()
    {
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");
        director.ReconnectDelaySeconds = 2.0f;
        DropTheLink();
        Tick(director);

        Tick(director, 5, 0.1f); // half a second

        Assert.Empty(Engine.ConnectAttempts);
    }

    [Fact]
    public void ReconnectingStopsAfterItsBoundedNumberOfAttempts()
    {
        Engine.ConnectSucceeds = false; // every attempt fails to even open a socket
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");
        director.ReconnectAttempts = 3;
        director.ReconnectDelaySeconds = 2.0f;
        DropTheLink();
        Tick(director);

        Tick(director, 200, 0.1f); // twenty seconds - far more than 3 x 2s

        Assert.Equal(3, Engine.ConnectAttempts.Count);
        Assert.Equal(NetSessionDirector.HostDisconnectedMessage, NetSession.StatusMessage);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
    }

    [Fact]
    public void ReconnectingHonoursADifferentAttemptBudget()
    {
        Engine.ConnectSucceeds = false;
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");
        director.ReconnectAttempts = 1;
        director.ReconnectDelaySeconds = 2.0f;
        DropTheLink();
        Tick(director);

        Tick(director, 200, 0.1f);

        Assert.Single(Engine.ConnectAttempts);
    }

    [Fact]
    public void AnAttemptThatIsNeverAnsweredIsAbandonedSoTheNextOneCanRun()
    {
        // Without the per-attempt timeout the whole sequence hangs on attempt one.
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");
        director.ReconnectAttempts = 3;
        director.ReconnectDelaySeconds = 2.0f;
        director.ReconnectTimeoutSeconds = 5.0f;
        DropTheLink();
        Tick(director);

        // Connect "starts" but the peer never becomes a client, so each attempt has to
        // time out on its own.
        Tick(director, 300, 0.1f); // thirty seconds

        Assert.Equal(3, Engine.ConnectAttempts.Count);
        Assert.Equal(NetSessionDirector.HostDisconnectedMessage, NetSession.StatusMessage);
    }

    [Fact]
    public void ASuccessfulReconnectClearsTheModeAndSaysSo()
    {
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");
        director.ReconnectDelaySeconds = 0.5f;
        DropTheLink();
        Tick(director);
        TickUntilAttemptGoesOut(director, 20, 0.1f);
        Assert.NotEmpty(Engine.ConnectAttempts);

        Engine.IsClient = true;
        Engine.IsConnected = true;
        Tick(director);

        Assert.False(director.IsReconnecting);
        Assert.Equal("Reconnected.", LastStatusText());
        Assert.Empty(Engine.ScenesLoaded);
    }

    [Fact]
    public void AReconnectThatIsRefusedIsAnAnswerRatherThanAFailureToRetry()
    {
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");
        director.ReconnectAttempts = 3;
        director.ReconnectDelaySeconds = 0.5f;
        DropTheLink();
        Tick(director);
        TickUntilAttemptGoesOut(director, 20, 0.1f);
        Engine.IsClient = true; // the attempt is in flight
        int attemptsBeforeTheRefusal = Engine.ConnectAttempts.Count;

        Engine.DisconnectReason = "Server is full";
        Tick(director);

        Assert.Equal("Server is full", NetSession.StatusMessage);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
        Assert.Equal(attemptsBeforeTheRefusal, Engine.ConnectAttempts.Count);
    }

    [Fact]
    public void AReasonArrivingBetweenAttemptsEndsTheRetrySequenceRatherThanBeingIgnored()
    {
        // A reason does not only arrive while an attempt is in flight - the host can
        // reject this peer before it ever opens another socket, while this peer is just
        // sitting in the delay counting down to the NEXT attempt. That answer must not
        // be missed for the accident of nothing being actively connecting when it shows
        // up. Every connect attempt is made to fail outright (never even reaches
        // "in flight") so the sequence spends its time in the waiting state, not the
        // attempting one.
        Engine.ConnectSucceeds = false;
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");
        director.ReconnectAttempts = 3;
        director.ReconnectDelaySeconds = 2.0f;
        DropTheLink();
        Tick(director);
        TickUntilAttemptGoesOut(director, 40, 0.1f); // burn attempt #1
        int attemptsBeforeTheRefusal = Engine.ConnectAttempts.Count;
        Assert.True(director.IsReconnecting); // waiting for attempt #2, none in flight

        Engine.DisconnectReason = "Server is full";
        Tick(director, 5, 0.1f); // well short of the delay to the next attempt

        Assert.Equal("Server is full", NetSession.StatusMessage);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
        Assert.False(director.IsReconnecting);
        Assert.Equal(attemptsBeforeTheRefusal, Engine.ConnectAttempts.Count);
    }

    // ── Reconnecting: by code ───────────────────────────────────────────────────

    [Fact]
    public void ThereIsSomethingToReconnectToWhenThisPeerJoinedByCode()
    {
        RecordingDirector director = InSessionByCode("PA1R01");

        DropTheLink();
        Tick(director);

        Assert.True(director.IsReconnecting);
        Assert.Contains("Reconnecting", LastStatusText());
        Assert.Empty(Engine.ScenesLoaded);
    }

    [Fact]
    public void ReconnectingByCodeRetriesThroughJoinByCodeNotADirectConnect()
    {
        RecordingDirector director = InSessionByCode("PA1R01");
        director.ReconnectDelaySeconds = 0.5f;
        DropTheLink();
        Tick(director);

        TickUntilJoinByCodeAttemptGoesOut(director, 20, 0.1f);

        Assert.Empty(Engine.ConnectAttempts);
        Assert.Equal("PA1R01", Assert.Single(Engine.JoinByCodeAttempts));
    }

    [Fact]
    public void AReconnectByCodeStillPunchingIsNotAbandonedBeforeItsTimeout()
    {
        // The same misleading-transport window as a first connection by code (see
        // TickInitialConnectByCode): NetIsClient would report false for the whole time
        // the ladder punches, so the in-flight test has to read TraversalState instead.
        RecordingDirector director = InSessionByCode("PA1R01");
        director.ReconnectDelaySeconds = 0.5f;
        director.ReconnectTimeoutSeconds = 5.0f;
        DropTheLink();
        Tick(director);
        TickUntilJoinByCodeAttemptGoesOut(director, 20, 0.1f);

        Engine.TraversalState = NetTraversalState.Punching; // still working, not connected yet
        Tick(director, 20, 0.1f); // two more seconds, well under the 5s timeout

        Assert.Single(Engine.JoinByCodeAttempts); // no second attempt fired yet
        Assert.True(director.IsReconnecting);
    }

    [Fact]
    public void AReconnectByCodeThatFailsTraversalCountsAsAFailedAttemptRatherThanWaitingOutTheTimeout()
    {
        RecordingDirector director = InSessionByCode("PA1R01");
        director.ReconnectAttempts = 1;
        director.ReconnectDelaySeconds = 0.5f;
        director.ReconnectTimeoutSeconds = 5.0f;
        DropTheLink();
        Tick(director);
        TickUntilJoinByCodeAttemptGoesOut(director, 20, 0.1f);

        Engine.TraversalState = NetTraversalState.Failed;
        Tick(director, 5, 0.1f); // half a second - far short of the 5s per-attempt timeout

        Assert.False(director.IsReconnecting);
        Assert.Equal(NetSessionDirector.HostDisconnectedMessage, NetSession.StatusMessage);
    }

    [Fact]
    public void ASuccessfulReconnectByCodeClearsTheModeAndSaysSo()
    {
        RecordingDirector director = InSessionByCode("PA1R01");
        director.ReconnectDelaySeconds = 0.5f;
        DropTheLink();
        Tick(director);
        TickUntilJoinByCodeAttemptGoesOut(director, 20, 0.1f);

        Engine.TraversalState = NetTraversalState.Connected;
        Engine.IsClient = true;
        Engine.IsConnected = true;
        Tick(director);

        Assert.False(director.IsReconnecting);
        Assert.Equal("Reconnected.", LastStatusText());
    }

    // ── Leaving ─────────────────────────────────────────────────────────────────

    [Fact]
    public void AskingToLeaveTearsTheSessionDownAndGoesBackToTheMenu()
    {
        Engine.IsHost = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);

        director.LeaveRequested = true;
        Tick(director);

        Assert.Equal(1, Engine.DisconnectCount);
        Assert.Equal(new[] { "Title" }, Engine.ScenesLoaded);
        Assert.Equal(string.Empty, NetSession.StatusMessage);
        Assert.False(NetSession.JoinRequested);
    }

    [Fact]
    public void LeavingClearsTheReconnectTargetSoItCannotSurviveIntoWhateverComesNext()
    {
        // NetSession.HostAddress/HostPort/HostRoomCode are process-wide statics that
        // outlive this scene on purpose - that is what lets a dropped link reconnect
        // after the arena reloads them. A VOLUNTARY leave ends that session for good,
        // so nothing here should still look like a live reconnect target to whatever
        // this peer does next: a fresh join by a different code, a hosted session, or
        // a solo game that should never attempt to reconnect to anything.
        RecordingDirector director = InSessionByCode("PA1R01");

        director.LeaveRequested = true;
        Tick(director);

        Assert.Equal(string.Empty, NetSession.HostAddress);
        Assert.Equal((ushort)0, NetSession.HostPort);
        Assert.Equal(string.Empty, NetSession.HostRoomCode);
    }

    [Fact]
    public void AnEmptyReturnSceneMeansStayWhereWeAre()
    {
        // What a game with a single persistent scene wants.
        Engine.IsHost = true;
        RecordingDirector director = NewDirector(returnScene: string.Empty);
        director.OnAttach();
        Tick(director);

        director.LeaveRequested = true;
        Tick(director);

        Assert.Empty(Engine.ScenesLoaded);
        Assert.Equal(1, Engine.DisconnectCount);
    }

    [Fact]
    public void LeavingDuringAReconnectStopsTheAttemptDraggingThePlayerBackIn()
    {
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");
        director.ReconnectDelaySeconds = 0.5f;
        DropTheLink();
        Tick(director);
        Assert.True(director.IsReconnecting);

        director.LeaveRequested = true;
        Tick(director);

        Assert.False(director.IsReconnecting);
        int attemptsAtLeave = Engine.ConnectAttempts.Count;
        director.LeaveRequested = false;
        Tick(director, 100, 0.1f);
        Assert.Equal(attemptsAtLeave, Engine.ConnectAttempts.Count);
    }

    [Fact]
    public void LeavingForgetsWhoWasIntroducedSoARejoinReportsEverybodyAgain()
    {
        NetSession.LocalPlayerName = "Alice";
        Engine.IsHost = true;
        RecordingDirector director = NewDirector(returnScene: string.Empty);
        director.OnAttach();
        Tick(director, 2, 1.0f / 60.0f);
        Assert.Equal(new[] { "Alice" }, director.Joined);

        director.LeaveRequested = true;
        Tick(director);
        director.LeaveRequested = false;
        Tick(director);

        Assert.Equal(new[] { "Alice", "Alice" }, director.Joined);
    }

    [Fact]
    public void LeavingTakesTheStatusLineOffTheScreen()
    {
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");
        DropTheLink();
        Tick(director); // puts "Connection lost..." up
        Assert.Empty(Engine.Destroyed);

        director.LeaveRequested = true;
        Tick(director);

        Assert.Single(Engine.Destroyed);
    }

    [Fact]
    public void DetachingTakesTheStatusLineOffTheScreen()
    {
        RecordingDirector director = InSession(hostAddress: "10.0.0.1");
        DropTheLink();
        Tick(director);

        director.OnDetach();

        Assert.Single(Engine.Destroyed);
    }

    // ── Offline ─────────────────────────────────────────────────────────────────

    [Fact]
    public void WithNoSessionAtAllOneSoloPlayerIsStillPutInTheLevel()
    {
        NetSession.LocalPlayerName = "Alice";
        Engine.PlaceEntity("Spawn0", new Vector3(2.0f, 0.0f, 0.0f));
        RecordingDirector director = NewDirector();
        director.OnAttach();

        Tick(director);

        // Scene.Instantiate, not Net.Spawn - the latter is host-only and returns nothing
        // here.
        Assert.Empty(Engine.Spawned);
        (string prefab, Vector3 position) = Assert.Single(Engine.Instantiated);
        Assert.Equal("player", prefab);
        Assert.Equal(new Vector3(2.0f, 0.0f, 0.0f), position);
        Assert.Equal("Alice", Assert.Single(Engine.NamesSet).Name);
        Assert.True(director.LocalPlayer.IsValid);
    }

    [Fact]
    public void TheSoloPlayerIsCreatedOnceAndNotAnnouncedToAnybody()
    {
        // A game telling itself that it joined reads as a bug.
        NetSession.LocalPlayerName = "Alice";
        RecordingDirector director = NewDirector();
        director.OnAttach();

        Tick(director, 30, 1.0f / 60.0f);

        Assert.Single(Engine.Instantiated);
        Assert.Empty(director.Joined);
        Assert.Equal(new[] { "Alice" }, ToNames(director.Players));
    }

    // ── Helpers ─────────────────────────────────────────────────────────────────

    /// <summary>Describe this peer as a connected client, so the director watches the
    /// link instead of putting a solo player in the level.</summary>
    private void AsAConnectedClient()
    {
        Engine.IsClient = true;
        Engine.IsConnected = true;
    }

    /// <summary>Tick until the reconnect sequence actually issues a connect.</summary>
    private void TickUntilAttemptGoesOut(NetSessionDirector director, int maxFrames, float deltaTime)
    {
        int before = Engine.ConnectAttempts.Count;
        for (int i = 0; i < maxFrames; i++)
        {
            director.OnUpdate(deltaTime);
            if (Engine.ConnectAttempts.Count > before)
            {
                return;
            }
        }
        throw new InvalidOperationException($"No reconnect attempt within {maxFrames} frames.");
    }

    /// <summary>A director that has been a live client for a frame.</summary>
    private RecordingDirector InSession(string hostAddress)
    {
        NetSession.HostAddress = hostAddress;
        NetSession.HostPort = 7777;
        NetSession.JoinRequested = true;
        Engine.IsClient = true;
        Engine.IsConnected = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);
        return director;
    }

    /// <summary>A director that has been a live client, joined by room code, for a
    /// frame.</summary>
    private RecordingDirector InSessionByCode(string code)
    {
        NetSession.HostRoomCode = code;
        NetSession.HostAddress = string.Empty;
        NetSession.HostPort = 0;
        NetSession.JoinRequested = true;
        Engine.IsClient = true;
        Engine.IsConnected = true;
        RecordingDirector director = NewDirector();
        director.OnAttach();
        Tick(director);
        return director;
    }

    /// <summary>Tick until the reconnect sequence actually issues a Net.JoinByCode.</summary>
    private void TickUntilJoinByCodeAttemptGoesOut(NetSessionDirector director, int maxFrames, float deltaTime)
    {
        int before = Engine.JoinByCodeAttempts.Count;
        for (int i = 0; i < maxFrames; i++)
        {
            director.OnUpdate(deltaTime);
            if (Engine.JoinByCodeAttempts.Count > before)
            {
                return;
            }
        }
        throw new InvalidOperationException($"No reconnect attempt within {maxFrames} frames.");
    }

    /// <summary>Lose the link with nothing to say - a timeout, a pulled cable. The
    /// framework drops the role back to offline when a session stops.</summary>
    private void DropTheLink()
    {
        Engine.IsClient = false;
        Engine.IsConnected = false;
        Engine.DisconnectReason = string.Empty;
    }

    private string LastStatusText()
    {
        List<UiCall> texts = Engine.UiCalls.FindAll(c => c.Op == "SetText");
        return texts.Count == 0 ? string.Empty : texts[texts.Count - 1].Text;
    }

    private static string[] ToNames(IReadOnlyList<NetSessionPlayer> players)
    {
        string[] names = new string[players.Count];
        for (int i = 0; i < players.Count; i++)
        {
            names[i] = players[i].Name;
        }
        return names;
    }
}

# Multiplayer

What this engine's netcode actually gives a game, what each piece costs, and where
every number below comes from — so that when a change invalidates one, the comment
or test that produced it is easy to find again. This is a reference to return to,
not a tutorial; the smallest working example is near the bottom.

Audience: you have shipped something single-player and have never written netcode.
Nothing here assumes you have.

## The model, in one paragraph

A session is one host and zero or more clients. Every replicated entity has exactly
one owner (a connection id, `0` for the host), and **the owner simulates it** — your
own player responds to your own input with no round trip and no correction of any
kind, because what you simulate *is* what everyone else eventually sees. Other
players' entities arrive by replication: the owner sends its changed state each
tick, the host relays it to everyone else, and (opt-in, see below) it is smoothed
on your screen instead of snapping between packets. There is no host migration —
if the host leaves, the session ends for everyone, on purpose (`NetSessionDirector`'s
own remarks: promoting a client would mean re-deriving an authoritative world from a
peer that only ever saw snapshots of it).

## The connect ladder

Every rung is tried cheapest-first, and **nobody forwards a port at any rung** —
that instruction to the player does not exist anywhere in this framework.

| Rung | What it needs | What it costs | Where |
|---|---|---|---|
| LAN broadcast | Nothing. No server, no port, not even the internet. | Only reaches a peer on the same network segment. | Default signaling backend until `Net.UseLanSignaling()`/`UseRendezvousSignaling()` is called; candidates go out as UDP broadcast (`BroadcastSignaling.hpp`). |
| Router port mapping | Nothing you run — UPnP‑IGD, NAT‑PMP or PCP asks the router itself. | Only works if the router speaks one of the three and it's enabled. | Tried automatically by `HostWithCode`; no data socket touched (`PortMapping.hpp`). |
| STUN + hole punch | A public STUN server for the one-shot "what do I look like from outside" lookup — already defaulted, nothing to run. | Fails outright on a symmetric NAT (a different public mapping per destination defeats punching by design, not by bug). | `network.stunHost`/`stunPort`, default `stun.l.google.com` / `19302` (`EngineSettings.hpp`, also in [multiplayer-relay.md](multiplayer-relay.md)). |
| Rendezvous signaling | A small box someone runs — free-ish to host, carries only candidate blobs (addresses and ports), never game traffic. | Needed the moment two peers are behind different routers and can't reach each other via LAN broadcast. | `Net.UseRendezvousSignaling("host:port")`, before `HostWithCode`/`JoinByCode` (`RendezvousChannel.hpp`). |
| TURN relay | A coturn box (or any RFC 5766/8656-compliant TURN server) — **off by default.** | Carries *every byte* of the match, both directions, at whoever runs it's expense. Only reached after a punch has genuinely failed, never in parallel with one. | `Net.ConfigureRelay(...)`, `network.allowRelay` defaults `false`. Full setup, credential handling and coturn config: [multiplayer-relay.md](multiplayer-relay.md). |


**A project ships the rendezvous rung for free, with nothing for a player to type.**
Setting `network.rendezvousHost` (and `network.rendezvousPort`, default `24701`) in
`ProjectSettings.toml`/`EngineSettings.toml` is a DEFAULT, not a call a game has to
make: `NetTraversalSession::SetRendezvousDefault` applies it on every
`HostWithCode`/`JoinByCode`, but only when nothing has explicitly chosen a backend
yet (`Net.UseLanSignaling()`/`UseRendezvousSignaling()`, from a menu or a settings
screen, always wins — see its own comment for the exact precedence). A project that
never calls either therefore gets the configured server automatically, on every
attempt, with no code change and no per-player configuration. `tools/rendezvous/`
covers standing one up for free (Docker image, and which free host's tier actually
carries UDP, checked against the others as of the date in that file); `projects/
Whisper/ProjectSettings.toml` and `ConnectScreen.cs`'s `ConfigureSignaling` are a
worked example of layering a player's own per-session override on top of that
project default without either one silently clobbering the other.

A same-network game (couch co-op over Wi-Fi, a LAN party) needs the first rung and
nothing else. Symmetric NAT is the only case that needs the last.

## The authority model, and the RPC rule that trips everyone

**Client-authoritative with host validation.** `Net.HasAuthority(entity)` and
`Net.IsOwner(entity)` both answer `true` for an entity you own and `true` for
everything, offline — code written for multiplayer runs unchanged in single-player.
`[Replicated]` script fields follow the same rule: the owner writes, everyone else's
write is silently overwritten by the owner's next send. The host is *not*
automatically authoritative for anyone else's entity; it only decides what a
`[NetRpc(NetRpcTarget.Server)]` call is allowed to do once it arrives.

**The rule.** A `[NetRpc(NetRpcTarget.Server)]` method is only ever run for the
connection that owns the entity it's attached to — the host checks the sender
against `NetworkIdentity.owner` before it will invoke anything
(`NetRpc.cpp`'s `ApplyRpc`). That means:

- A server RPC belongs on a script attached to **the caller's own entity** — a
  player's own prefab, a projectile it spawned and owns.
- Putting the same method on a scene-placed, **host-owned** entity (a game
  manager, a spawner) compiles, and calling it from a client *routes* —
  `Net.Call` returns `true` (and `Net.CallServer`, the usual call site for a
  server RPC, returns `void` — there is no return value to even check), because
  routing only checks that the entity is replicated at all, not who owns it
  (`NetRpc.cpp`'s `RouteRpc`, client branch).
  The call reaches the host, the host's ownership check fails silently, and nothing
  happens. No exception, no `false` return, no client-visible error — only a
  warning in the *host's* own log ("does not own it; put a `[NetRpc(Server)]`
  method on a script attached to an entity the CALLER owns").
- If a client genuinely needs to ask the host to do something host-side (spawn an
  entity, adjudicate a hit), that request still has to be declared on a script the
  *client* owns — its own player, its own projectile — which then calls into
  whatever host-side logic it needs.

## The latency budget

**Your own input is never delayed.** There is no round trip between pressing a key
and your own entity moving — you own it, you simulate it. Everything below is only
about what you see of *other* players.

A remote entity with a `NetworkTransform` component is rendered some delay behind
now so there's always a sample on both sides of render time to interpolate between
(`interpolationDelaySeconds`, auto-tuned by default from the observed arrival
spacing and jitter — see `InterpolationBuffer::RecommendedDelaySeconds`). That
delay is, algebraically, `measured send interval + 4×jitter` — the interval term
exists purely because packets arrive discretely, the jitter term exists to survive
a genuinely late one (`NetInterpolation.hpp`, `kJitterMultiplier = 4`, an RFC
3550-style playout sizing).

Extrapolation (`extrapolate`, default on) spends part of that delay proactively: it
dead-reckons the entity forward from the velocity between its last two real
arrivals instead of only ever holding a confirmed sample, shrinking how far in the
past it's drawn. With `autoExtrapolationBudget` on (default), the amount spent is
`extrapolationBudgetFraction × the measured interval` — **the whole interval, by
default** — leaving the jitter margin untouched as real protection against a late
packet. That is the render floor this converges to: `delay − interval = 4×jitter`,
exactly (`NetComponents.hpp`'s own derivation).

**Measured effect, at the framework's default 20Hz send rate (50ms interval):**

| Link | Before (no spend) | After (interval spent) | What's left |
|---|---|---|---|
| Clean, near-zero jitter | ~50ms rendered staleness | ~0ms (`NetInterpolationTests.cpp`'s steady-link case: effective delay `< 10ms`) | Real one-way transit only |
| Realistic jitter (30ms/70ms alternating interval — `NetInterpolationTests.cpp`'s jitter scenarios) | ~90ms | ~40ms | One-way transit + the kept `4×jitter` margin |

**This is not zero latency, and it cannot be.** A cross-continent round trip is
~30ms of light and cable each way at minimum; nothing in software removes that.
What was removed is the delay *the engine itself* was adding on top of the network
by rendering everyone a fixed, un-spent buffer behind real time. The number left
over is exactly the two things above: physical one-way transit, plus the jitter
margin this framework still keeps as insurance against a late packet.

## Failure modes

Every mechanism that hides latency has a cost. All of it is opt-outable per entity.

- **A curving entity's extrapolation undershoots continuously**, not just on a
  reversal — linear projection from the last two samples cuts the inside of any
  curve. The error is bounded to at most one send interval's worth of curvature
  (re-anchored the instant the next real sample lands) and never compounds across
  samples. For an entity that turns often and hard, lower
  `extrapolationBudgetFraction` (spend less of the interval, keep more buffered
  margin) rather than turning extrapolation off outright.
- **A misprediction is corrected, never snapped.** When a new sample disagrees with
  what the trend had projected (the clearest case: a direction change), the
  difference fades to exactly zero over `kCorrectionWindowSeconds` (0.15s,
  `NetInterpolation.hpp`) instead of popping the entity onto the new position in
  one frame.
- **Past `maxExtrapolationSeconds` (default 0.15s — three send intervals at 20Hz),
  the entity freezes**, holding the position the projection had reached at the cap.
  It does not keep sliding into the distance past its last real observation, and it
  does not jump back to the raw last sample either — both would be their own
  visible artifact. A fast, mostly-straight mover (a thrown projectile) can afford
  raising this per-entity; a character that jukes at close range usually cannot.
- **Lag-compensated hit validation (`Net.TryRewind`) can let a victim be hit up to
  300ms (`kMaxRewindSeconds`, `NetRewind.hpp`) after they believe they have already
  moved out of the way** — "shot behind cover." This is the accepted cost of
  validating a shot against what the shooter's own screen showed rather than the
  victim's current, more up-to-date position; the 300ms cap bounds it to a slice of
  the past, never an unbounded one. It is opt-in per validation call — nothing
  starts rewinding anything just because `TryRewind` exists in a project's scripts.
- **The shooter's own last-mile jitter is never actually measured** — it can't be,
  from the host. `TryRewind`'s view-delay estimate substitutes half the shooter's
  round trip (RTT is not perfectly symmetric — halving it is already an estimate)
  plus the delay *this host* renders the victim with, which shares the victim's
  sender but has nothing to do with the shooter's own receive jitter for that
  stream (`NetRewind.cpp`'s `EstimateViewDelaySeconds`). That approximation is
  capped, not corrected, and the direction it's biased in — the shooter usually
  saw the victim's history at least as compensated as this admits, rarely
  more — is exactly why it's characterized as erring toward
  under-compensating rather than over-compensating the shooter's claim.
- **A connection whose estimated view delay would exceed the cap is clamped, never
  refused.** A struggling link degrades to a smaller compensation window; it does
  not stop working.

## What an author writes

The **Multiplayer 2D**/**Multiplayer 3D** project templates already wire the
pieces below — a `Lobby` scene running a `MultiplayerLobby : NetConnectMenu`
subclass that builds its own UI, an `Arena` scene running a
`MultiplayerSession : NetSessionDirector` subclass — so a new multiplayer project
starts from this, not from a blank scene.

1. **Connect UI.** Subclass `NetConnectMenu` on an entity in the menu scene — a
   scene can only attach a type compiled into the *project's own* script
   assembly, and `NetConnectMenu` lives in the compile-only SDK reference, so
   attaching it by name directly (`type = 'NetConnectMenu'`) fails at load with
   "Unknown C# script type"; a subclass is what a project's build actually
   produces. Set `ArenaScene` in the subclass's constructor (`PlayerPrefab` is a
   `NetSessionDirector` field, set in the *session* subclass instead — see the
   next item), call
   `base.OnUpdate(deltaTime)` first from any `OnUpdate` override so hosting/joining
   still ticks, then build whatever UI reads the inherited `StatusText` /
   `HostedRoomCode` / `IsHosting` / `IsJoining` / `IsBusy` and calls the inherited
   `Host(code?, port?, maxConnections?)` / `Join(code)` / `PlaySolo()`. It already
   handles the `Net.ReplicationReady` false-on-menu/true-in-arena discipline, the
   join timeout, and cancel — the part that's easy to get subtly wrong rather than
   merely tedious.
2. **Session/spawning.** Subclass `NetSessionDirector` on a scene entity in the
   gameplay scene; an empty subclass is a complete, working session. It spawns
   `PlayerPrefab` per connection at `SpawnN` markers, keeps the roster, and decides
   what a dropped link means (a bounded reconnect vs. sending everyone back to
   `ReturnScene`).
3. **Replicated state.** Mark a script field `[Replicated]` for anything the owner
   should broadcast (float/int/bool/`Vector3`/string/enum only).
4. **RPCs.** `[NetRpc(NetRpcTarget.Server, MaxPerSecond = 8, Burst = 4)]` on
   anything a client's input drives — a fire button, a hit report, a chat line.
   Leaving `MaxPerSecond` at its default (0, unlimited) is a decision the engine
   makes you notice (see [Security](#security-posture)), not one it makes for you.
5. **Owner-dependent setup.** Override `OnOwnershipChanged(uint owner, bool
   isOwner)` instead of polling `Net.IsOwner` every frame — it fires exactly once,
   the frame ownership is decided (including offline and host-owned entities, on
   the same frame as `OnAttach`).
6. **Instant local feedback.** `Net.SpawnPredicted(prefab, position)` for a
   purely-local stand-in shown before the round trip that creates the real,
   replicated entity completes — a no-op on the host and offline, where there's no
   round trip to hide.
7. **Fair hit validation.** `Net.TryRewind(entity, viewerConnection, out position,
   out rotation, out appliedDelay)` on the host, inside the `[NetRpc(Server)]`
   handler that judges a hit claim — see [Failure modes](#failure-modes) for its cost.

A minimal weapon script, combining several of the above:

```csharp
using System.Numerics;
using AetherCore;

namespace AetherGame;

public sealed class Turret : EntityScript
{
    [Replicated] public int Ammo = 30;

    public override void OnOwnershipChanged(uint owner, bool isOwner)
    {
        Self.Component("Sprite Renderer")
            .SetVector4("tint", isOwner ? Vector4.One : new Vector4(0.6f, 0.6f, 0.6f, 1f));
    }

    // Called from input, on the owner only.
    private void OnFireButton()
    {
        Net.SpawnPredicted("shell", Self.Position);   // on screen now
        Net.CallServer(Self, nameof(RequestFire));    // authoritative copy follows
    }

    [NetRpc(NetRpcTarget.Server, MaxPerSecond = 4, Burst = 4)]
    public void RequestFire()
    {
        if (Ammo <= 0) return;
        Ammo--;
        Net.Spawn("shell", Self.Position, owner: Net.OwnerOf(Self));
    }

    // Host-side hit judgment. `victimConnectionText` travels as a string - RPC
    // arguments are at most one string wide - and is resolved back to an entity
    // via a per-project roster helper, the same technique Whisper's own
    // Projectile.ReportHit uses (PlayerCombat.PlayerOwnedBy).
    [NetRpc(NetRpcTarget.Server, MaxPerSecond = 20)]
    public void ReportHit(string victimConnectionText)
    {
        if (!uint.TryParse(victimConnectionText, out uint victimConnection)) return;
        Entity victim = PlayerRoster.PlayerOwnedBy(victimConnection);
        uint shooter = Net.OwnerOf(Self);

        Vector3 checkPosition = Net.TryRewind(victim, shooter, out Vector3 pos, out _, out _)
            ? pos              // where the shooter's screen actually showed them
            : victim.Position; // no history to rewind - fall back to live position
        // ... range/pacing checks against checkPosition.
    }
}
```

`Turret` is attached to the caller's own entity — its `RequestFire`/`ReportHit`
methods are exactly the shape the [RPC rule](#the-authority-model-and-the-rpc-rule-that-trips-everyone)
above requires; putting either on a scene-placed turret entity nobody owns would
compile and silently do nothing for a client that tries to fire it.

## Security posture

- **The host validates, it does not trust.** Every `[NetRpc(Server)]` call already
  arrives pre-checked for direction and ownership (see the RPC rule above) — that
  is enforcement, not a suggestion a handler still has to implement.
- **Rate-limiting a handler is the author's job.** `MaxPerSecond`/`Burst` default to
  unlimited, and the engine logs one diagnostic per assembly load naming every
  `[NetRpc(Server)]` method that declares none, so "no limit" is a decision an
  author has to notice rather than one that's invisible until an audit finds it
  (`ScriptRegistry.cs`).
- **TURN credentials are long-term, today.** A username/password compiled into a
  shipped client is readable by any player who inspects the binary or captures a
  packet — fine for "the studio's own relay, and everyone playing the game already
  has the credential," never fine the moment the credential is meant to gate
  anything else. Per-session, short-lived credentials are the standard fix and are
  **not implemented** by this engine yet — see
  [multiplayer-relay.md](multiplayer-relay.md#where-the-credentials-must-not-go)
  for what a project has to build itself to get that.
- **The rendezvous server is a public, unauthenticated UDP endpoint by design** —
  every datagram it receives is hostile input, and it is built to that assumption
  rather than trusting its network. It accepts only its own `AECR2` line grammar
  (a room code, a capability token, and a candidate blob no game data ever rides
  inside); anything else, including the old pre-token `AECR1` protocol, is dropped
  unread. A peer must complete a one-round-trip token handshake — proof it can
  receive at the address it claims — before the server will store or forward
  anything on its behalf; an unauthenticated datagram earns exactly one small
  reply (its own token) and never room data, which is what keeps an open port from
  becoming a reflection amplifier. Hard per-source-IP rate/size caps (datagram size,
  outbound bytes per second, token replies per second) and hard table caps (rooms,
  peers per room, tracked source IPs) bound a flood to degraded service rather than
  an unbounded resource drain — see `tools/rendezvous/README.md`'s "Why the token
  round trip" and "Abuse bounds" sections for the exact numbers, and
  `tools/rendezvous/RendezvousServerLogic.hpp` for the enforcement itself.

## Where to go next

| You want | Look at |
|---|---|
| TURN relay setup, coturn config, credential handling in depth | [multiplayer-relay.md](multiplayer-relay.md) |
| Standing up a rendezvous server for free (Docker, which free hosts actually carry UDP) | [tools/rendezvous/README.md](../tools/rendezvous/README.md) |
| The full `Net` API surface | `managed/AetherCore/Net.cs`, `NetAttributes.cs`, `NetConnectMenu.cs`, `NetSessionDirector.cs`, `NetPrediction.cs` |
| A working, if pre-lag-compensation, example project | `projects/Whisper` — see `PlayerCombat.cs`/`Projectile.cs` for the two-hop RPC shape (owner → host → other owner) this doc's example follows |
| Extrapolation/interpolation tuning internals | `src/app/net/NetComponents.hpp` (`NetworkTransform`), `NetInterpolation.hpp`/`.cpp` |
| Lag compensation internals | `src/app/net/NetRewind.hpp`/`.cpp` |

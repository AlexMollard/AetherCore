# Whisper — Networked Multiplayer Framework and Testbed

Date: 2026-07-27
Status: Approved

## Motivation

AetherCore has no networking. The goal is a reusable, UE5-shaped replication
framework — mark a component field replicated and it synchronises — plus a
standalone project, **Whisper**, that proves it with four playable capabilities.

Whisper is deliberately a new project rather than a branch of INKBOUND: INKBOUND
is a single-player game with ink mechanics, save profiles and a level-select hub,
none of which belong in a network testbed. Whisper borrows only what is
self-contained.

## Goals

1. A player can enter an IP address to join, or host a session.
2. At least 4 concurrent players; more is better.
3. In-game chat; every player sees what the others write.
4. Players see each other moving, with each player's name above their head.

**Acceptance:** four instances (one editor Play host + three standalone clients)
connect over `127.0.0.1`, run around a shared arena, see each other move
smoothly with correct name tags, and hold a chat conversation.

## Non-goals

- **NAT traversal / hosting without port forwarding.** Deferred to its own spec.
  The intended path is a vendor transport — Discord's Social SDK (lobbies +
  relayed P2P) rather than Steam or Epic. The transport layer is deliberately
  narrow so it can be swapped without touching replication.
- **Voice chat.** A later direction, most likely once a 3D scene exists (the
  project is named for it). The channel model below reserves room: voice wants
  its own unreliable channel and must never share ordering with state snapshots.
- **Rollback / reconciliation.** Prediction corrects by easing, not rewinding.
- **Delta compression against acknowledged state**, update prioritisation, or
  bandwidth budgeting. Per-field change detection only.
- **Dedicated server.** One player hosts and plays.

## Decisions and rationale

| Decision | Choice | Why |
|---|---|---|
| Transport | ENet, direct IP (LAN or forwarded port) | Already vendored (`CMake/Dependencies.cmake:286`) and proven in `ControlServer.cpp`. NAT is a separate problem with separate infrastructure. |
| Authority | Host-authoritative | One player hosts and plays; the host's simulation is truth. |
| Local feel | Predict + smooth correction | Full rollback needs deterministic `b2World_Step` and hand-rolled Box2D snapshot/restore — the largest piece of the project, before anything is visible. Easing gets most of the feel for a fraction of the work and does not block upgrading later. |
| Framework scope | Core + relevancy + interpolation | Identity, replicated fields, spawn/despawn, ownership, authority, RPCs, relevancy filtering, interpolated transforms. |
| Layering | Transport in engine, replication in app | Forced by where the reflection registry lives (see below). |

## Architecture

### Layering

```
src/engine/net/       Transport. ENet host/peer, channels, connection lifecycle.
                      No knowledge of entities or components.
src/app/net/          Replication framework. NetworkIdentity, reflection-driven
                      field sync, spawn/despawn, ownership, RPCs, relevancy.
managed/AetherCore/Net.cs   The C# surface projects use.
projects/Whisper/     The testbed game.
```

The split is not stylistic. The reflection registry that makes "mark it
replicated" cheap lives in `src/app/scene/reflection/`, but ENet has no business
knowing about components. `src/app/net/` is picked up by the existing recursive
glob in `src/app/CMakeLists.txt:1` and is **not** on the runtime exclusion list
(only `debug/`, `editor/`, `imgui/`, `launcher/`, `project/` are), so it compiles
into both Editor and GameRuntime automatically.

`ENet` is currently linked to Editor, Launcher and aether-ctl only. `Engine` must
link it so GameRuntime inherits it. Note that `ControlServer.cpp` lives under
`editor/` and is excluded from the shipped runtime — the game transport cannot
reuse it and must not live there.

### Transport (engine)

```cpp
class NetworkSubsystem
{
    bool Host(std::uint16_t port, int maxPeers);
    bool Connect(std::string_view host, std::uint16_t port);
    void Disconnect();
    void Send(ConnectionId peer, int channel, bool reliable, std::span<const std::byte>);
    void Broadcast(int channel, bool reliable, std::span<const std::byte>);
    void Poll();                       // drains ENet into the event queue
    std::span<const NetEvent> Events() const;  // Connected | Disconnected | Data
};
```

**Polled on the main thread** inside the network tick, not on a worker thread.
`ControlServer` can afford a thread because it hands work across a queue and
touches nothing live; replication reads and writes the ECS every tick, and a
worker would reintroduce exactly the threading hazards the render-frame
extraction exists to prevent.

Channels:

- **0 — reliable ordered:** spawn, despawn, RPCs, chat.
- **1 — unreliable sequenced:** state snapshots. A dropped snapshot is superseded
  by the next one, so retransmitting it is worse than dropping it.
- **2 — reserved** for voice, so it never shares ordering with state.

### Replication (app)

**Marking fields.** `FieldMeta` gains a `replicated` bit and an `AE_FIELD_REP(...)`
macro sits beside the existing `AE_FIELD_N`. At startup the framework walks the
component catalog once and builds a flat schema of `(componentId, fieldIndex,
FieldType)`. Marking a field replicated is a one-line change in the same
declaration that already drives MCP, the inspector and the serializer.

**Identity.** `NetworkIdentity { netId, ownerConnection, spawnSource }`. `netId`
is stable and host-assigned.

**Sync.** Each tick the host walks entities carrying `NetworkIdentity`, reads
each replicated field through `FieldDesc::get` (`Reflection.hpp:83`), compares
against the last sent value, and writes only what changed. Clients apply through
`FieldDesc::set`. Values move as `FieldValue` variants tagged by `FieldType`.

**Spawn/despawn.** Host-authoritative. `Net.Spawn(prefab, owner)` instantiates on
the host and broadcasts `netId + prefab path + initial transform`; clients
instantiate the same prefab. Scene-placed replicated entities are assigned ids
during scene load, in the order the entities appear in the scene file — which is
deterministic and identical on every machine loading the same scene, so both
sides agree with no handshake. (ECS iteration order is *not* deterministic and
must never be used for this.)

**Ownership and authority.** `Net.HasAuthority(entity)` (am I the host?) and
`Net.IsOwner(entity)` (is this my player?). Input is only read for owned
entities.

**RPCs.** `[NetRpc(Server | Client | Multicast)]` on `EntityScript` methods,
discovered by the same managed reflection that already finds scripts.

**Relevancy.** Per-connection radius filter; entities outside a client's radius
stop being replicated to it.

**Interpolation.** A `NetworkTransform` component buffers incoming snapshots and
renders remote entities ~100 ms in the past. This is what makes other players
read as smooth rather than teleporting between packets.

### Session lifecycle

The host owns the session. On **client disconnect**, the host despawns that
player's entity and broadcasts the despawn plus a chat notice. On **host
disconnect**, the session ends: clients see the transport drop, tear down all
replicated entities, and return to the title screen with a message. There is no
host migration — that is a genuinely hard feature and nothing in the four goals
needs it.

Player identity is a replicated field. The name typed on the join screen is sent
once on connect and stored on a `PlayerInfo` component alongside the player
entity, replicated like any other field — so name tags, chat attribution and the
disconnect notice all read from the same place rather than each tracking names
separately.

### Prediction and correction

The local player runs the real `PlayerController` against a real Box2D body with
zero input latency. Authoritative positions arrive continuously; when the local
position diverges past a threshold, the body eases toward the authoritative one
over several frames rather than snapping. No rewind, no determinism requirement,
no physics snapshotting.

## The Whisper project

Borrowed from INKBOUND:

- `PlayerController.cs` — verified to reference no other INKBOUND script, so it
  lifts unchanged.
- Player sprites from `assets/textures/player/`.
- `assets/textures/world/tileset_16x16.png` + its sprite atlas, painted into a
  compact arena (ground, platforms, walls). Not `sandbox.tilemap`, which is built
  around ink mechanics and single-player pacing.

New:

- **Title → Host/Join screen.** IP address and player name entered with the
  `UITextBox` shipped on 2026-07-27 (`content_type = host` is exactly this case).
- **Arena scene** with spawn points.
- **Chat.** A text box plus a scrollback log, on the reliable channel.
- **Name tags.** Canvas text positioned above each player. Needs one new engine
  export, `Camera.WorldToScreen` — `ScreenToWorld` exists (`Camera.cs:73`) but
  not its inverse.

## Testing

The framework's core is pure functions over plain data, which is what makes it
testable without sockets — the same shape that worked for `UiTextEdit`:

- Schema construction from the component catalog.
- Snapshot serialise/deserialise round-trip for every `FieldType`.
- Per-field change detection.
- Interpolation buffer (ordering, gaps, late packets).
- Relevancy filtering.

Above that:

- **Loopback integration test:** a host and a client in one process over
  `127.0.0.1`, asserting connect, spawn, field sync and disconnect.
- **Manual:** editor Play hosts; standalone GameRuntime instances join.

## Risks and accepted tradeoffs

- **Per-field `std::function` calls every tick.** Fine for 4 players and a
  handful of replicated components; not what a 64-player game would ship. The
  schema is a flat table specifically so swapping in cached offsets later is a
  contained change.
- **`AE_FIELD_REP` touches the reflection macros**, which every component in the
  engine already uses. Small change, load-bearing file.
- **Relevancy is unobservable at this scale.** Four players in one arena are
  always relevant to each other. It is in scope by request and cheap on top of
  the schema, but nothing in the testbed can demonstrate it working.
- **Prediction without rollback drifts under real latency.** On a LAN it is
  invisible. Over the internet, fast direction changes will show correction
  easing. Accepted; the upgrade path is left open.

## Implementation phasing

Two plans, in order:

1. **Framework** — transport, identity, schema, sync, spawn/despawn, ownership,
   RPCs, relevancy, interpolation, `Camera.WorldToScreen`, plus the unit and
   loopback tests.
2. **Whisper** — project scaffold, borrowed assets, arena, host/join screen,
   chat, name tags, prediction tuning.

Designing both against each other is the point: the framework API is validated by
a real consumer instead of being speculated at.

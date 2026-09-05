# Multiplayer relay (TURN)

Part of the [multiplayer reference](multiplayer.md) — start there for the connect
ladder, the authority model and the latency budget; this page goes deep on the
last rung of that ladder only: how a match survives a symmetric NAT, and how to
point the engine at a relay without tying the project to any vendor.

## Why a relay at all

The connect ladder first asks the router directly for a port
(`PortMapping.hpp`: UPnP-IGD, NAT-PMP, PCP). That fails outright on a router
that doesn't speak any of the three — common on carrier-grade NAT, which is
usually symmetric too. The next rung, a UDP hole punch (`NatTraversal.hpp`,
using the STUN client in `StunMessage.hpp`), needs no router cooperation but
relies on a narrower assumption: that the NAT hands out the *same* public
mapping for a local port no matter who it's talking to, so telling a peer
that mapping through signaling is enough for both sides to punch through to
it.

A symmetric NAT breaks exactly that assumption, by design — it hands out a
*different* public mapping per destination, so the mapping a peer learned by
talking to the signaling server is simply not the one that will be open when
the other peer's packet arrives. No amount of retrying or configuring the
punch fixes this; it is a property of the NAT, not a bug in the punch.
`NatTraversal` already detects this and fails with an explicit message
rather than retrying forever (see the comment in `NatTraversal.hpp`). The
only fix left is a third machine both sides can reach normally, which relays
packets between them — a TURN server.

## Why TURN and not a vendor SDK

TURN (Traversal Using Relays around NAT) is a plain IETF protocol —
[RFC 5766](https://www.rfc-editor.org/rfc/rfc5766) and its update
[RFC 8656](https://www.rfc-editor.org/rfc/rfc8656) — built directly on STUN
([RFC 5389](https://www.rfc-editor.org/rfc/rfc5389)). It defines a wire
format, not a service: any compliant server works, so "who runs the relay" is
a config string, not a code dependency. That means:

- No Epic/EOS SDK, no Discord SDK, no third-party account of any kind baked
  into the engine.
- The relay can be a self-hosted [coturn](https://github.com/coturn/coturn)
  instance (BSD-licensed), a friend's box, or any commercial TURN provider —
  the engine speaks the same protocol to all of them.
- Switching relays is changing a host, port, and credential in settings, not
  shipping a new build.

## Standing up coturn

[coturn](https://github.com/coturn/coturn) is the reference open-source TURN/STUN
server. A minimal, working `turnserver.conf` for long-term-credential auth
(one fixed username/password pair, which is what this engine's TURN settings
expect):

```ini
# Port coturn listens on for TURN/STUN requests (UDP and TCP).
listening-port=3478

# Bind to all interfaces; set to the box's real address if it has more than one.
listening-ip=0.0.0.0

# Long-term credential mechanism: authenticate with a fixed username/password
# instead of a REST API or shared secret.
lt-cred-mech

# Realm the credential below belongs to. Required by lt-cred-mech.
realm=example.com

# One static user. Format is username:password.
user=myuser:mypassword

# Range of ports coturn may open per-relayed-session to actually forward
# game traffic. Must be allowed through any firewall in front of the box.
min-port=49152
max-port=65535

# Add the STUN/TURN FINGERPRINT attribute to responses.
fingerprint

# No cert on hand: plain UDP/TCP only, no TLS/DTLS listener.
no-tls
no-dtls
```

Run it with `turnserver -c /etc/turnserver.conf`, or point systemd's shipped
unit at that path. Firewall rules need:

- **UDP 3478** (and TCP 3478, coturn accepts both) — the control port peers
  first talk to.
- **UDP `min-port`–`max-port`** (`49152–65535` above) — one port per active
  relayed session, opened for the lifetime of that session.

Everything above is a directive documented in coturn's own
[`examples/etc/turnserver.conf`](https://github.com/coturn/coturn/blob/master/examples/etc/turnserver.conf)
and [wiki](https://github.com/coturn/coturn/wiki/turnserver); nothing here is
engine-specific. TLS, the REST/time-limited credential mechanism, and
multi-realm setups are real coturn features this doc does not cover — the
long-term-credential shape above is the one the engine's settings target.

## Pointing the engine at it

`EngineSettings::Network` (`src/engine/utils/EngineSettings.hpp`) holds both
NAT-traversal servers, reflected through the same `SettingsService` cascade
as every other setting (compiled default → shipped `EngineSettings.toml` →
per-user `UserSettings.toml`):

| Key | Meaning | Default |
|---|---|---|
| `network.stunHost` / `network.stunPort` | STUN server for the public-address lookup the punch needs. One-shot, carries no game traffic, so a public default is harmless. | `stun.l.google.com` / `19302` |
| `network.turnHost` / `network.turnPort` | TURN relay host and port. | empty / `3478` |
| `network.turnUsername` / `network.turnPassword` | Long-term credential matching the `user=` line in `turnserver.conf`. | empty |
| `network.allowRelay` | Whether the connect ladder is permitted to fall back to the relay at all. | `false` |

To use the config above from a shipped or per-user `.toml`:

```toml
[network]
turnHost = "turn.example.com"
turnPort = 3478
turnUsername = "myuser"
turnPassword = "mypassword"
allowRelay = true
```

`network.turnHost` defaults to empty and `network.allowRelay` defaults to
`false` — both must be set deliberately. No default ships a relay nobody
chose to run.

## Configuring a relay from a script

`settings.toml`/`EngineSettings.toml` above is the right place for a relay a
project always wants available — a self-hosted coturn box the studio runs,
say. A game that lets a *player* type in a relay (or that mints one per
match) instead sets it at runtime, through `Net`:

```csharp
// Before HostWithCode / JoinByCode - a title or settings screen, typically.
Net.ConfigureRelay("turn.example.com", 3478, "myuser", "mypassword");

// Show the fallback only when there is actually something to fall back to.
if (Net.RelayConfigured)
{
    ShowRelayFallbackNotice();
}
```

`ConfigureRelay` writes straight through the same `network.turnHost` /
`turnPort` / `turnUsername` / `turnPassword` / `allowRelay` settings the
`.toml` files configure (`SettingsService` is the single source of truth for
both) — there is no separate runtime copy, so whatever a script sets here is
exactly what the connect ladder reads next time it reaches the `Relaying`
rung. `RelayConfigured` reports whether the ladder currently has one to try
at all: a non-empty host *and* `allowRelay` on.

`allow` defaults to `true` on the call itself, but that is a convenience for
the common case of "the player just typed in a relay and wants it used" —
the *engine* default (`allowRelay = false` until something sets it) is still
off, and it is still worth surfacing the relay to the player as a fallback
they are opting into rather than something that just silently starts costing
someone bandwidth.

### Where the credentials must not go

**Never compile a shared TURN password into a shipped client.** Every copy
of the game embeds the same string, and a player who wants it needs nothing
more than a hex editor or a packet capture of the game calling
`ConfigureRelay` to read it straight back out — a "long-term credential" is
long-term for the *server*, but it is not secret from the *players* it was
shipped to. That is a fine model for a relay the studio itself runs, where
"everyone playing this game" and "everyone with the credential" are the same
set on purpose. It stops being fine the moment the credential is meant to
gate anything else (rate-limiting one player's abuse, a paid relay tier, …),
because every player already has it.

The standard fix — which this engine does **not** implement yet — is a
short-lived, per-session credential: a matchmaking or lobby service the
players already trust mints a TURN username/password (or a REST-style
time-limited credential, which coturn also supports) that is valid for one
session and one pair of peers, and hands it to both ends over whatever
secure channel that service already uses. A project that needs this has to
build that minting step itself and call `ConfigureRelay` with the result;
the long-term-credential `turnserver.conf` in this doc is the simpler
"studio runs the relay for everyone" case, not a substitute for it.

## The cost, and why it's last

Every byte of a relayed match passes through the TURN box twice — once in,
once out — which is bandwidth (and, on a paid provider, money) spent by
whoever runs it, unlike a punched connection where packets go peer-to-peer.
That's why the relay is meant to be the *last* rung of the connect ladder
(`NetTraversalSession::TraversalState`: `Mapping` → `Signaling`/`Punching` →
`Relaying` → `Connecting`), tried only after a router port mapping and a hole
punch have both genuinely failed — never in parallel with the punch, because
someone's bandwidth is on the line — and why `allowRelay` defaults off:
turning it on is an explicit choice to let a match spend someone's
bandwidth to work around a NAT that can't be punched. `Relaying` is skipped
straight to `Failed` when no relay is configured or `allowRelay` is off, so
a project that never touches any of this behaves exactly as it always has.

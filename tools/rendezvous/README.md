# aether-rendezvous

A tiny, long-running UDP server that pairs NAT-traversal peers by a six-character
room code and forwards the candidate blob each side publishes. It never sees game
traffic - only these few-hundred-byte blobs - which is exactly what lets it run
unattended on a cheap VPS or a free-tier instance.

## Running it

```
aether-rendezvous [--port N]
```

`--port` defaults to `24701`. Open that one UDP port to the internet on whatever
host you run this on; nothing else needs to be reachable. `Ctrl+C` (or `SIGTERM`)
shuts it down cleanly.

## Wire protocol (AECR2)

One datagram per line. `<blob>` is whatever `aether::net::EncodeCandidates()`
produced; it contains spaces, so it is always the remainder of the line, never
split on whitespace itself. A trailing CRLF is tolerated as well as a bare LF.

```
client -> server: AECR2 <ROOMCODE> <TOKEN> <blob>\n
server -> client: AECR2 <ROOMCODE> <TOKEN> <BODY>\n
```

`<TOKEN>` is 16 uppercase hex digits, and `-` means "none yet". Every server
line carries the **recipient's own** token, so a client recovers it from any
datagram it receives. `<BODY>` is either `-` (handshake only) or one other
verified peer's blob.

A peer joins by sending its blob with `-` as the token. The server mints a
random token for that `(room, source address)`, stores nothing else, and replies
with just that token. The client re-sends its blob carrying the token; from then
on the server stores and forwards its blob, and answers each publish with the
blobs of the room's other verified peers. Retrying with `-` at any time re-issues
the same token, so a lost reply or a restarted client self-heals.

**AECR1 clients do not work against this server** (and vice versa): the old
protocol had no token round trip, which made the server both a UDP reflection
amplifier and trivially poisonable by spoofed-source datagrams. AECR1 lines are
dropped unread; both ends fail fast rather than half-working.

## Why the token round trip

The server is public and unauthenticated by design, so every datagram is hostile
input. The token costs a peer one round trip and buys one thing: proof that it
can receive at the address it claims. That proof is what the abuse bounds hang on:

- **No reflection/amplification.** An unauthenticated datagram earns exactly one
  small reply - its own token - and never room data, no matter how full the room.
  A spoofed-source flood therefore reflects inert ~34-byte lines at a claimed
  victim, capped at 8 per second per claimed host IP.
- **No blob poisoning.** A datagram without the right token changes no stored
  state: not the peer's blob, and not its liveness timestamp, so a spoofed packet
  can neither replace a victim's candidates with garbage nor keep a poisoned
  entry alive.
- **No data to unproven addresses.** Blobs flow only between verified peers, so
  a spoofer cannot aim other players' ip:port pairs at a third party just by
  claiming its address.

## Abuse bounds

- Datagrams over 1200 bytes are dropped unparsed.
- 4096 rooms, 16 peers per room, 4096 tracked source IPs - hard caps. At a cap
  the server evicts flood filler first, then the stalest state, so a flood
  degrades garbage rather than locking real sessions out.
- Unverified peers (never completed the handshake) expire after 10 s; verified
  peers after 120 s of silence.
- Per source IP, per one-second window: at most 32 KiB of outbound traffic the
  datagrams from that IP triggered, and at most 8 token replies. An honest
  two-peer room costs a few hundred bytes per publish and one token per session.
- Rooms, peers and source-IP counters are printed once every 30 s; nothing logs
  per datagram, so a flood cannot fill the disk.

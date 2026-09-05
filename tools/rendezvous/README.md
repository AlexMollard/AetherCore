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

## Deploying for free (Docker + Oracle Cloud)

`Dockerfile` builds a small image (debian-slim plus the compiled binary - no
compiler, no build tools, no source left in it) via `docker/CMakeLists.txt`, a
standalone CMake project that fetches only ENet. It deliberately does NOT configure
the CMakeLists.txt beside this file, which is meant to be `add_subdirectory()`'d from
the AetherCore engine root and would drag in a required Vulkan SDK this UDP-only
server never touches - see that file's own comment. From this directory:

    docker compose up -d --build

builds and starts it; `restart: unless-stopped` in `docker-compose.yml` is what
brings it back after a host reboot with nothing further for you to do.

### Which free host: Oracle Cloud Always Free (verified 2026-09-05)

The deciding constraint is UDP: this server carries none of its traffic over
HTTP(S), and several widely-recommended "free tier" hosts either cannot route raw
UDP to a container at all or no longer have a free tier by which to try. Checked on
the date above:

- **Fly.io** has removed its free tier for new accounts entirely - its own docs now
  say "there is no free account/free tier on Fly.io", only a time-limited trial.
- **Railway** has no standing free tier either: a one-time trial credit, then a
  $5/month minimum.
- **Render**'s free service types (Web Services, Private Services, Background
  Workers) either accept only HTTP(S) or accept no public inbound traffic at all -
  none of them can bind a public UDP port.
- **Oracle Cloud Always Free** is a genuine standing allocation, not a trial: real
  compute VMs (two `VM.Standard.E2.1.Micro` AMD instances, or `VM.Standard.A1.Flex`
  Arm instances totalling 2 OCPUs/12 GB), each with a public IPv4 and full control of
  both the VM's own firewall and Oracle's Security List - a UDP ingress rule on any
  port is entirely yours to add, because it is a real VM rather than a
  request-shaped platform. This server's own load (a few hundred bytes per publish,
  one stats line every 30 s - see Abuse bounds below) is nowhere near what even the
  smallest Always Free shape's bandwidth can carry.

  **One caveat worth knowing before you pick this.** Oracle reclaims an Always Free
  compute instance it judges idle: 95th-percentile CPU under 20% AND network under
  20%, measured over a rolling 7-day window (Oracle's own Always Free Resources
  documentation). A rendezvous server between occasional play sessions can plausibly
  read as idle by that measure over a quiet week. There is no honest way to promise
  otherwise short of generating fake traffic, which this project will not do -
  if reclaimed, redeploying is the same handful of steps below.

### Step by step

1. **Create the VM.** OCI Console -> Compute -> Instances -> Create Instance. Pick
   an Always Free-eligible shape (`VM.Standard.E2.1.Micro` is the simplest choice)
   and the Ubuntu image. Under "Show advanced options" -> Management, paste
   `docker/oci-cloud-init.sh` into the cloud-init script box - it installs Docker
   and opens the VM's OWN firewall for UDP 24701 on first boot.
2. **Open the port at the network level too.** The cloud-init script only reaches
   the VM's own firewall; Oracle's Security List sits in front of that as a separate
   gate nothing inside the VM can configure. In the Console, open the VCN this
   instance's subnet belongs to (Networking -> Virtual Cloud Networks -> your VCN ->
   Security Lists -> Default Security List) and add an Ingress Rule: source CIDR
   `0.0.0.0/0`, IP protocol UDP, destination port range `24701`.
3. **Get the code onto the VM.** SSH in with the key you supplied at instance
   creation, then `git clone` your checkout of this repository, or `scp -r` just
   this `tools/rendezvous/` directory - the standalone build needs nothing else.
4. **Start it:** `cd tools/rendezvous && docker compose up -d --build`.
5. **Confirm it is listening:** `docker compose logs -f` should print
   `[rendezvous] listening on 0.0.0.0:24701 (UDP)` immediately, then a
   `rooms=... peers=... sources=...` line every 30 s.
6. **Point a game at it.** The VM's public IP (shown on the instance page) is what
   goes into `network.rendezvousHost` - see `../../docs/multiplayer.md` and
   `../../projects/Whisper/ProjectSettings.toml` for where that setting actually
   feeds into a running game.

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

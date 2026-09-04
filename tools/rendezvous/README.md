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

## Wire protocol

One datagram per line. `<blob>` is whatever `aether::net::EncodeCandidates()`
produced; it contains spaces, so it is always the remainder of the line, never
split on whitespace itself.

```
client -> server: AECR1 <ROOMCODE> <blob>\n
server -> client: AECR1 <ROOMCODE> <blob>\n   (the blob of a DIFFERENT peer in that room)
```

On receiving a blob the server stores it for that `(room, sender)`, forwards it to
every other peer currently in the room, and sends that sender the latest blob of
each of those peers, so a late joiner catches up in one round trip. Rooms and
peers expire after 120 seconds of inactivity, and room/peer counts plus datagram
length are hard-capped so an open port cannot be used to grow this process's
memory or flood its log.

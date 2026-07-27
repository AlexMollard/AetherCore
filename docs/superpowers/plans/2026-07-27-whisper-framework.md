# Whisper Replication Framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A reusable, UE5-shaped networking layer for AetherCore — mark a C++ component field or a C# script field replicated and it synchronises across the network — with host/join over ENet, replicated spawn, ownership, RPCs, relevancy and interpolation.

**Architecture:** Transport lives in `src/engine/net/` and knows nothing about entities. The replication framework lives in `src/app/net/`, beside the reflection registry that drives it. Replication is schema-driven: a one-time walk of the component catalog and the managed script tables produces a flat table of replicated fields, and every tick the host reads those fields through the existing `FieldDesc::get`, sends what changed, and clients apply through `FieldDesc::set`.

**Tech Stack:** C++20, ENet, EnTT, GLM, doctest, CMake/Ninja, C#/CoreCLR interop.

**Spec:** `docs/superpowers/specs/2026-07-27-whisper-multiplayer-design.md`

**This is plan 1 of 2.** Plan 2 (`2026-07-27-whisper-game.md`) builds the Whisper testbed on top of this.

## Global Constraints

- **Layering is load-bearing.** `src/engine/net/` must not include anything from `src/app`, must not know what an `Entity` or a component is, and must compile with only ENet + the engine. `src/app/net/` may use the reflection registry.
- **`ControlServer.cpp` lives under `src/app/editor/` and is EXCLUDED from GameRuntime** (`src/app/CMakeLists.txt:14`). The game transport cannot reuse it and must not live there. `src/app/net/` is not on any exclusion list, so it builds into both Editor and GameRuntime automatically.
- **`Engine` must link `enet`.** It currently is linked only to Editor, Launcher and aether-ctl (`src/app/CMakeLists.txt:82`, `:307`, `tools/control-client/CMakeLists.txt:6`).
- **Poll ENet on the main thread**, inside the network systems. Not on a worker thread — replication touches the ECS every tick.
- **Channels:** 0 = reliable ordered (spawn, despawn, RPC, chat). 1 = unreliable sequenced (state snapshots). 2 = reserved for voice; do not use.
- **Host-authoritative.** Clients never author replicated state except through RPCs. Prediction corrects by easing, never by rewinding.
- **Net ids for scene-placed entities are assigned in scene-file load order.** ECS iteration order is NOT deterministic and must never be used for this.
- Code style: TABS for indentation, Allman braces, `m_` member prefix, `[[nodiscard]]` on pure queries. C# uses 4 spaces with XML doc comments.
- Commit style: plain imperative subject, optional flat bullet body. NO `feat:`/`fix:` prefixes, no scopes, no emoji, no `Co-Authored-By` or tool-attribution trailers.
- Build tree: `build/ninja-clang`. Engine, app and test sources are globbed with `CONFIGURE_DEPENDS` — new files need no CMake edit, but the first build after adding one triggers a reconfigure.
- **A running editor locks `build/ninja-clang/data/scripts/managed`.** Rebuilds then fail the staging copy with "Permission denied" and silently leave `EngineTests` unrelinked, so test counts look stale. Close the editor before building.
- Baseline suite before this plan: **349 test cases / 2404 assertions**.

## File Structure

**Created — engine (transport, no entity knowledge)**
- `src/engine/net/NetTypes.hpp` — `ConnectionId`, `NetEvent`, channel constants, `NetRole`.
- `src/engine/net/NetworkSubsystem.hpp` / `.cpp` — ENet host/client lifecycle, send, poll.

**Created — app (replication)**
- `src/app/net/NetComponents.hpp` — `NetworkIdentity`, `NetworkTransform`, `NetPlayer`.
- `src/app/net/NetSerialize.hpp` / `.cpp` — `ByteWriter`/`ByteReader` and `FieldValue` codec. Pure.
- `src/app/net/ReplicationSchema.hpp` / `.cpp` — builds the replicated-field table from the component catalog. Pure given a catalog.
- `src/app/net/NetSnapshot.hpp` / `.cpp` — snapshot build/apply + change detection.
- `src/app/net/NetSpawn.hpp` / `.cpp` — spawn/despawn messages and client-side instantiation.
- `src/app/net/NetRelevancy.hpp` / `.cpp` — per-connection radius filter. Pure.
- `src/app/net/NetInterpolation.hpp` / `.cpp` — snapshot buffer and interpolation. Pure.
- `src/app/net/NetworkSystems.hpp` / `.cpp` — `NetworkReceiveSystem`, `NetworkSendSystem`.
- `src/app/net/NetSession.hpp` / `.cpp` — session state: role, connections, netId allocation, entity↔netId maps.
- `src/app/scripting/interop/NetExports.cpp` — C# interop.

**Created — managed**
- `managed/AetherCore/Net.cs` — public C# API.
- `managed/AetherCore/NetAttributes.cs` — `[Replicated]`, `[NetRpc]`.

**Created — tests**
- `tests/net/NetSerializeTests.cpp`, `ReplicationSchemaTests.cpp`, `NetSnapshotTests.cpp`, `NetInterpolationTests.cpp`, `NetRelevancyTests.cpp`, `NetLoopbackTests.cpp`.

**Modified**
- `src/engine/CMakeLists.txt` — link `enet`.
- `src/app/scene/reflection/Reflection.hpp` — `FieldMeta::replicated`, `AE_FIELD_REP` macro.
- `src/app/scene/reflection/CoreComponents.reflect.cpp` — mark transform fields replicated.
- `src/app/Application.cpp:265-320` — register the two network systems.
- `src/app/scene/SceneSerializerApply.cpp` — assign scene-load net ids.
- `managed/AetherCore.Interop/ScriptRegistry.cs` — `[Replicated]` discovery + get/set by index.
- `managed/AetherCore/Internal/Native.cs`, `managed/AetherCore/Camera.cs`.
- `src/app/scripting/interop/CameraExports.cpp` — `WorldToScreen`.

---

### Task 1: ENet transport

**Files:**
- Create: `src/engine/net/NetTypes.hpp`, `src/engine/net/NetworkSubsystem.hpp`, `src/engine/net/NetworkSubsystem.cpp`
- Modify: `src/engine/CMakeLists.txt`
- Test: `tests/net/NetLoopbackTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `using aether::net::ConnectionId = std::uint32_t;` (`0` = invalid)
  - `enum class NetRole : std::uint8_t { Offline, Host, Client }`
  - `struct NetEvent { enum class Kind { Connected, Disconnected, Data } kind; ConnectionId peer; int channel; std::vector<std::byte> data; }`
  - `constexpr int kChannelReliable = 0; constexpr int kChannelSnapshot = 1; constexpr int kChannelCount = 3;`
  - `class NetworkSubsystem` with `Host(port, maxPeers) -> bool`, `Connect(host, port) -> bool`, `Disconnect()`, `Send(peer, channel, reliable, span)`, `Broadcast(channel, reliable, span)`, `Poll()`, `Events() -> std::span<const NetEvent>`, `Role()`, `IsActive()`, `LocalConnectionId()`

- [ ] **Step 1: Write the failing loopback test**

Create `tests/net/NetLoopbackTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "net/NetworkSubsystem.hpp"

using namespace aether;

namespace
{
	// Pump both ends until `done` or the deadline. ENet needs several service calls
	// to complete a handshake, so a single Poll() is never enough.
	bool PumpUntil(net::NetworkSubsystem& a, net::NetworkSubsystem& b, auto done, int maxMs = 2000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			a.Poll();
			b.Poll();
			if (done())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}
} // namespace

TEST_CASE("A client connects to a host over loopback and exchanges a payload")
{
	net::NetworkSubsystem host;
	net::NetworkSubsystem client;

	REQUIRE(host.Host(24681, 4));
	CHECK(host.Role() == net::NetRole::Host);

	REQUIRE(client.Connect("127.0.0.1", 24681));

	net::ConnectionId hostSawPeer = 0;
	const bool connected = PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: host.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Connected)
			        {
				        hostSawPeer = e.peer;
			        }
		        }
		        return hostSawPeer != 0;
	        });
	REQUIRE(connected);

	const std::string payload = "hello";
	host.Send(hostSawPeer, net::kChannelReliable, true,
	        std::as_bytes(std::span<const char>{payload.data(), payload.size()}));

	std::string received;
	const bool gotData = PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: client.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Data)
			        {
				        received.assign(reinterpret_cast<const char*>(e.data.data()), e.data.size());
			        }
		        }
		        return !received.empty();
	        });
	REQUIRE(gotData);
	CHECK(received == "hello");

	client.Disconnect();
	host.Disconnect();
	CHECK(host.Role() == net::NetRole::Offline);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `net/NetworkSubsystem.hpp` not found.

- [ ] **Step 3: Write NetTypes.hpp**

Create `src/engine/net/NetTypes.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <vector>

namespace aether::net
{
	// A remote peer. 0 is never a valid connection, so it doubles as "none".
	using ConnectionId = std::uint32_t;
	inline constexpr ConnectionId kInvalidConnection = 0;

	enum class NetRole : std::uint8_t
	{
		Offline,
		Host,
		Client
	};

	// Channel 0 is reliable-ordered: anything whose loss would desync state
	// (spawn, despawn, RPC, chat). Channel 1 is unreliable-sequenced: state
	// snapshots, where a dropped packet is superseded by the next one and
	// retransmitting it is worse than dropping it. Channel 2 is reserved for
	// voice so it can never share ordering with state - do not use it here.
	inline constexpr int kChannelReliable = 0;
	inline constexpr int kChannelSnapshot = 1;
	inline constexpr int kChannelVoiceReserved = 2;
	inline constexpr int kChannelCount = 3;

	struct NetEvent
	{
		enum class Kind : std::uint8_t
		{
			Connected,
			Disconnected,
			Data
		};

		Kind kind = Kind::Data;
		ConnectionId peer = kInvalidConnection;
		int channel = 0;
		std::vector<std::byte> data;
	};
} // namespace aether::net
```

- [ ] **Step 4: Write NetworkSubsystem.hpp**

Create `src/engine/net/NetworkSubsystem.hpp`:

```cpp
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/NetTypes.hpp"

struct _ENetHost;
struct _ENetPeer;

namespace aether::net
{
	// ENet transport. Deliberately knows nothing about entities, components or
	// replication - it moves bytes between peers and reports connection events.
	// The replication layer in src/app/net sits on top of this.
	//
	// Polled on the MAIN thread from the network systems, not on a worker: unlike
	// the editor's ControlServer (which hands work across a queue and touches
	// nothing live), replication reads and writes the ECS every tick.
	class NetworkSubsystem
	{
	public:
		NetworkSubsystem() = default;
		~NetworkSubsystem();

		NetworkSubsystem(const NetworkSubsystem&) = delete;
		NetworkSubsystem& operator=(const NetworkSubsystem&) = delete;

		bool Host(std::uint16_t port, int maxPeers);
		bool Connect(std::string_view host, std::uint16_t port);
		void Disconnect();

		void Send(ConnectionId peer, int channel, bool reliable, std::span<const std::byte> bytes);
		void Broadcast(int channel, bool reliable, std::span<const std::byte> bytes);

		// Drains ENet into Events(). Call once per frame before reading events;
		// each call clears the previous frame's events.
		void Poll();

		[[nodiscard]] std::span<const NetEvent> Events() const
		{
			return m_events;
		}

		[[nodiscard]] NetRole Role() const
		{
			return m_role;
		}

		[[nodiscard]] bool IsActive() const
		{
			return m_role != NetRole::Offline;
		}

		// On a client, the id the host assigned us (0 until connected). On a host, 0.
		[[nodiscard]] ConnectionId LocalConnectionId() const
		{
			return m_localId;
		}

		void SetLocalConnectionId(ConnectionId id)
		{
			m_localId = id;
		}

		[[nodiscard]] std::string LastError() const
		{
			return m_lastError;
		}

	private:
		_ENetPeer* PeerFor(ConnectionId id) const;

		_ENetHost* m_host = nullptr;
		_ENetPeer* m_serverPeer = nullptr; // client only
		NetRole m_role = NetRole::Offline;
		ConnectionId m_localId = kInvalidConnection;
		ConnectionId m_nextPeerId = 1;
		std::vector<NetEvent> m_events;
		std::string m_lastError;
		bool m_enetAcquired = false;
	};
} // namespace aether::net
```

- [ ] **Step 5: Write NetworkSubsystem.cpp**

Create `src/engine/net/NetworkSubsystem.cpp`. Mirror `src/app/editor/ControlServer.cpp:22-44` for the refcounted `enet_initialize`/`enet_deinitialize` pair — two subsystems in one process (a loopback test, or an editor hosting while a tool connects) must not deinitialise ENet out from under each other.

```cpp
#include "net/NetworkSubsystem.hpp"

#include <cstring>
#include <mutex>

#include <enet/enet.h>

#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		// ENet's global init is process-wide and not refcounted by the library.
		// Same pattern as editor/ControlServer.cpp so two hosts in one process
		// (loopback tests, editor + game) cannot tear each other down.
		std::mutex g_enetInitMutex;
		int g_enetRefCount = 0;

		bool AcquireEnet()
		{
			const std::lock_guard<std::mutex> lock(g_enetInitMutex);
			if (g_enetRefCount == 0 && enet_initialize() != 0)
			{
				return false;
			}
			++g_enetRefCount;
			return true;
		}

		void ReleaseEnet()
		{
			const std::lock_guard<std::mutex> lock(g_enetInitMutex);
			if (g_enetRefCount > 0 && --g_enetRefCount == 0)
			{
				enet_deinitialize();
			}
		}
	} // namespace

	NetworkSubsystem::~NetworkSubsystem()
	{
		Disconnect();
	}

	bool NetworkSubsystem::Host(std::uint16_t port, int maxPeers)
	{
		Disconnect();
		if (!AcquireEnet())
		{
			m_lastError = "enet_initialize failed";
			return false;
		}
		m_enetAcquired = true;

		ENetAddress address{};
		address.host = ENET_HOST_ANY;
		address.port = port;
		m_host = enet_host_create(&address, static_cast<std::size_t>(maxPeers), kChannelCount, 0, 0);
		if (m_host == nullptr)
		{
			m_lastError = "enet_host_create failed (port in use?)";
			ReleaseEnet();
			m_enetAcquired = false;
			return false;
		}
		m_role = NetRole::Host;
		m_localId = kInvalidConnection;
		m_nextPeerId = 1;
		AE_INFO(LogCategory::App, "Hosting on port {}", port);
		return true;
	}

	bool NetworkSubsystem::Connect(std::string_view host, std::uint16_t port)
	{
		Disconnect();
		if (!AcquireEnet())
		{
			m_lastError = "enet_initialize failed";
			return false;
		}
		m_enetAcquired = true;

		m_host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
		if (m_host == nullptr)
		{
			m_lastError = "enet_host_create failed";
			ReleaseEnet();
			m_enetAcquired = false;
			return false;
		}

		ENetAddress address{};
		const std::string hostStr{host};
		if (enet_address_set_host(&address, hostStr.c_str()) != 0)
		{
			m_lastError = "could not resolve '" + hostStr + "'";
			enet_host_destroy(m_host);
			m_host = nullptr;
			ReleaseEnet();
			m_enetAcquired = false;
			return false;
		}
		address.port = port;

		m_serverPeer = enet_host_connect(m_host, &address, kChannelCount, 0);
		if (m_serverPeer == nullptr)
		{
			m_lastError = "no available peers";
			enet_host_destroy(m_host);
			m_host = nullptr;
			ReleaseEnet();
			m_enetAcquired = false;
			return false;
		}
		m_role = NetRole::Client;
		return true;
	}

	void NetworkSubsystem::Disconnect()
	{
		if (m_host != nullptr)
		{
			if (m_serverPeer != nullptr)
			{
				enet_peer_disconnect_now(m_serverPeer, 0);
				m_serverPeer = nullptr;
			}
			enet_host_destroy(m_host);
			m_host = nullptr;
		}
		if (m_enetAcquired)
		{
			ReleaseEnet();
			m_enetAcquired = false;
		}
		m_role = NetRole::Offline;
		m_localId = kInvalidConnection;
		m_events.clear();
	}

	_ENetPeer* NetworkSubsystem::PeerFor(ConnectionId id) const
	{
		if (m_host == nullptr)
		{
			return nullptr;
		}
		for (std::size_t i = 0; i < m_host->peerCount; ++i)
		{
			ENetPeer* peer = &m_host->peers[i];
			if (peer->state == ENET_PEER_STATE_CONNECTED && static_cast<ConnectionId>(reinterpret_cast<std::uintptr_t>(peer->data)) == id)
			{
				return peer;
			}
		}
		return nullptr;
	}

	void NetworkSubsystem::Send(ConnectionId peer, int channel, bool reliable, std::span<const std::byte> bytes)
	{
		if (m_host == nullptr || bytes.empty())
		{
			return;
		}
		ENetPeer* target = m_role == NetRole::Client ? m_serverPeer : PeerFor(peer);
		if (target == nullptr)
		{
			return;
		}
		ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(),
		        reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
		enet_peer_send(target, static_cast<enet_uint8>(channel), packet);
	}

	void NetworkSubsystem::Broadcast(int channel, bool reliable, std::span<const std::byte> bytes)
	{
		if (m_host == nullptr || bytes.empty())
		{
			return;
		}
		ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(),
		        reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
		enet_host_broadcast(m_host, static_cast<enet_uint8>(channel), packet);
	}

	void NetworkSubsystem::Poll()
	{
		m_events.clear();
		if (m_host == nullptr)
		{
			return;
		}

		ENetEvent event{};
		while (enet_host_service(m_host, &event, 0) > 0)
		{
			switch (event.type)
			{
			case ENET_EVENT_TYPE_CONNECT:
			{
				// The host stamps each peer with a small stable id; the client's single
				// peer is the server and never needs one.
				const ConnectionId id = m_role == NetRole::Host ? m_nextPeerId++ : kInvalidConnection;
				event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
				m_events.push_back(NetEvent{.kind = NetEvent::Kind::Connected, .peer = id});
				break;
			}
			case ENET_EVENT_TYPE_DISCONNECT:
			{
				const auto id = static_cast<ConnectionId>(reinterpret_cast<std::uintptr_t>(event.peer->data));
				event.peer->data = nullptr;
				m_events.push_back(NetEvent{.kind = NetEvent::Kind::Disconnected, .peer = id});
				break;
			}
			case ENET_EVENT_TYPE_RECEIVE:
			{
				const auto id = static_cast<ConnectionId>(reinterpret_cast<std::uintptr_t>(event.peer->data));
				NetEvent e{.kind = NetEvent::Kind::Data, .peer = id, .channel = event.channelID};
				e.data.resize(event.packet->dataLength);
				std::memcpy(e.data.data(), event.packet->data, event.packet->dataLength);
				m_events.push_back(std::move(e));
				enet_packet_destroy(event.packet);
				break;
			}
			default:
				break;
			}
		}
	}
} // namespace aether::net
```

- [ ] **Step 6: Link ENet to Engine**

In `src/engine/CMakeLists.txt`, add `enet` to the Engine target's linked libraries. It must be **PUBLIC** so GameRuntime and EngineTests inherit the include directory that `CMake/Dependencies.cmake:295` exports on the target:

```cmake
target_link_libraries(Engine PUBLIC enet)
```

- [ ] **Step 7: Run the test**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="A client connects*"
```

Expected: PASS. If it hangs or times out, the port may be in use — the test uses 24681 specifically to avoid the editor's 8787.

- [ ] **Step 8: Run the whole suite and commit**

```bash
./build/ninja-clang/tests/EngineTests.exe
```

Expected: 350 cases passing.

```bash
git add src/engine/net src/engine/CMakeLists.txt tests/net/NetLoopbackTests.cpp
git commit -m "Add an ENet transport subsystem to the engine"
```

---

### Task 2: Byte codec for reflected field values

**Files:**
- Create: `src/app/net/NetSerialize.hpp`, `src/app/net/NetSerialize.cpp`
- Test: `tests/net/NetSerializeTests.cpp`

**Interfaces:**
- Consumes: `aether::reflect::FieldValue`, `FieldType` (`src/app/scene/reflection/Reflection.hpp:17,56`).
- Produces:
  - `class ByteWriter` — `U8/U16/U32/I32/F32/Str(std::string_view)/Bytes(span)`, `Take() -> std::vector<std::byte>`, `Size()`
  - `class ByteReader` — `U8/U16/U32/I32/F32/Str/Bytes(n)`, `Ok()`, `Remaining()`
  - `void WriteFieldValue(ByteWriter&, const reflect::FieldValue&)`
  - `reflect::FieldValue ReadFieldValue(ByteReader&, reflect::FieldType)`

`ByteReader` must never throw or read out of bounds on malformed input — a client can send anything. Every read past the end sets a sticky `Ok() == false` and returns a zero value.

- [ ] **Step 1: Write the failing tests**

Create `tests/net/NetSerializeTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include "net/NetSerialize.hpp"

using namespace aether;
using namespace aether::reflect;

TEST_CASE("ByteWriter and ByteReader round-trip primitives")
{
	net::ByteWriter w;
	w.U8(7);
	w.U16(1234);
	w.U32(0xDEADBEEF);
	w.I32(-42);
	w.F32(1.5f);
	w.Str("hello");

	const std::vector<std::byte> bytes = w.Take();
	net::ByteReader r{bytes};

	CHECK(r.U8() == 7);
	CHECK(r.U16() == 1234);
	CHECK(r.U32() == 0xDEADBEEF);
	CHECK(r.I32() == -42);
	CHECK(r.F32() == doctest::Approx(1.5f));
	CHECK(r.Str() == "hello");
	CHECK(r.Ok());
	CHECK(r.Remaining() == 0);
}

TEST_CASE("ByteReader fails safely past the end instead of reading garbage")
{
	net::ByteWriter w;
	w.U8(1);
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	CHECK(r.U8() == 1);
	CHECK(r.Ok());

	CHECK(r.U32() == 0u); // past the end
	CHECK_FALSE(r.Ok());
	CHECK(r.Str().empty()); // still safe once failed
	CHECK_FALSE(r.Ok());
}

TEST_CASE("ByteReader rejects a string length that overruns the buffer")
{
	// A hostile peer claims a 1 GB string in a 5-byte packet.
	net::ByteWriter w;
	w.U32(1024u * 1024u * 1024u);
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	CHECK(r.Str().empty());
	CHECK_FALSE(r.Ok());
}

TEST_CASE("FieldValue round-trips for every replicable type")
{
	auto roundTrip = [](FieldValue in, FieldType type)
	{
		net::ByteWriter w;
		net::WriteFieldValue(w, in);
		const std::vector<std::byte> bytes = w.Take();
		net::ByteReader r{bytes};
		FieldValue out = net::ReadFieldValue(r, type);
		CHECK(r.Ok());
		return out;
	};

	FieldValue f;
	f.type = FieldType::Float;
	f.num = 2.5;
	CHECK(roundTrip(f, FieldType::Float).num == doctest::Approx(2.5));

	FieldValue i;
	i.type = FieldType::Int;
	i.num = -17;
	CHECK(roundTrip(i, FieldType::Int).num == doctest::Approx(-17));

	FieldValue b;
	b.type = FieldType::Bool;
	b.boolean = true;
	CHECK(roundTrip(b, FieldType::Bool).boolean);

	FieldValue v3;
	v3.type = FieldType::Vec3;
	v3.vec = {1.f, 2.f, 3.f, 0.f};
	const FieldValue outV3 = roundTrip(v3, FieldType::Vec3);
	CHECK(outV3.vec.x == doctest::Approx(1.f));
	CHECK(outV3.vec.z == doctest::Approx(3.f));

	FieldValue c4;
	c4.type = FieldType::Color4;
	c4.vec = {0.1f, 0.2f, 0.3f, 0.4f};
	CHECK(roundTrip(c4, FieldType::Color4).vec.w == doctest::Approx(0.4f));

	FieldValue e;
	e.type = FieldType::Enum;
	e.enumValue = 3;
	CHECK(roundTrip(e, FieldType::Enum).enumValue == 3);

	FieldValue s;
	s.type = FieldType::String;
	s.str = "player one";
	CHECK(roundTrip(s, FieldType::String).str == "player one");
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `net/NetSerialize.hpp` not found.

- [ ] **Step 3: Write the header**

Create `src/app/net/NetSerialize.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "scene/reflection/Reflection.hpp"

namespace aether::net
{
	// Little-endian byte cursor. Deliberately not a general serialization library:
	// it exists so replication packets have one obvious, bounds-checked encoding.
	class ByteWriter
	{
	public:
		void U8(std::uint8_t v);
		void U16(std::uint16_t v);
		void U32(std::uint32_t v);
		void I32(std::int32_t v);
		void F32(float v);
		void Str(std::string_view v); // u32 length + bytes
		void Bytes(std::span<const std::byte> v);

		[[nodiscard]] std::size_t Size() const
		{
			return m_data.size();
		}

		[[nodiscard]] std::vector<std::byte> Take()
		{
			return std::move(m_data);
		}

		[[nodiscard]] std::span<const std::byte> View() const
		{
			return m_data;
		}

	private:
		std::vector<std::byte> m_data;
	};

	// Bounds-checked reader. A remote peer can send anything, so every read past the
	// end sets a sticky failure and returns a zero value rather than reading garbage
	// or throwing. Callers check Ok() once after parsing rather than at every field.
	class ByteReader
	{
	public:
		explicit ByteReader(std::span<const std::byte> data)
		      : m_data(data)
		{
		}

		std::uint8_t U8();
		std::uint16_t U16();
		std::uint32_t U32();
		std::int32_t I32();
		float F32();
		std::string Str();
		std::vector<std::byte> Bytes(std::size_t n);

		[[nodiscard]] bool Ok() const
		{
			return m_ok;
		}

		[[nodiscard]] std::size_t Remaining() const
		{
			return m_ok ? m_data.size() - m_cursor : 0;
		}

	private:
		bool Want(std::size_t n);

		std::span<const std::byte> m_data;
		std::size_t m_cursor = 0;
		bool m_ok = true;
	};

	// FieldValue codec. The type tag is NOT written - the schema already agrees on it
	// at both ends, so writing it per field would be pure overhead on every snapshot.
	void WriteFieldValue(ByteWriter& w, const reflect::FieldValue& value);
	[[nodiscard]] reflect::FieldValue ReadFieldValue(ByteReader& r, reflect::FieldType type);

	// True for types replication can carry. EntityRef and List are excluded: an entity
	// id is meaningless across machines (it must go through a netId), and a list is a
	// variable-size structure whose change detection would need its own design.
	[[nodiscard]] bool IsReplicableFieldType(reflect::FieldType type);
} // namespace aether::net
```

- [ ] **Step 4: Write the implementation**

Create `src/app/net/NetSerialize.cpp`:

```cpp
#include "net/NetSerialize.hpp"

#include <cstring>

namespace aether::net
{
	namespace
	{
		template<typename T>
		void Append(std::vector<std::byte>& out, T v)
		{
			const auto* p = reinterpret_cast<const std::byte*>(&v);
			out.insert(out.end(), p, p + sizeof(T));
		}

		// A single packet is never legitimately this large; anything claiming to be is
		// either corrupt or hostile.
		constexpr std::uint32_t kMaxStringBytes = 64u * 1024u;
	} // namespace

	void ByteWriter::U8(std::uint8_t v)
	{
		m_data.push_back(static_cast<std::byte>(v));
	}

	void ByteWriter::U16(std::uint16_t v)
	{
		Append(m_data, v);
	}

	void ByteWriter::U32(std::uint32_t v)
	{
		Append(m_data, v);
	}

	void ByteWriter::I32(std::int32_t v)
	{
		Append(m_data, v);
	}

	void ByteWriter::F32(float v)
	{
		Append(m_data, v);
	}

	void ByteWriter::Str(std::string_view v)
	{
		U32(static_cast<std::uint32_t>(v.size()));
		const auto* p = reinterpret_cast<const std::byte*>(v.data());
		m_data.insert(m_data.end(), p, p + v.size());
	}

	void ByteWriter::Bytes(std::span<const std::byte> v)
	{
		m_data.insert(m_data.end(), v.begin(), v.end());
	}

	bool ByteReader::Want(std::size_t n)
	{
		if (!m_ok || m_cursor + n > m_data.size())
		{
			m_ok = false;
			return false;
		}
		return true;
	}

	std::uint8_t ByteReader::U8()
	{
		if (!Want(1))
		{
			return 0;
		}
		return static_cast<std::uint8_t>(m_data[m_cursor++]);
	}

	std::uint16_t ByteReader::U16()
	{
		if (!Want(sizeof(std::uint16_t)))
		{
			return 0;
		}
		std::uint16_t v = 0;
		std::memcpy(&v, m_data.data() + m_cursor, sizeof(v));
		m_cursor += sizeof(v);
		return v;
	}

	std::uint32_t ByteReader::U32()
	{
		if (!Want(sizeof(std::uint32_t)))
		{
			return 0;
		}
		std::uint32_t v = 0;
		std::memcpy(&v, m_data.data() + m_cursor, sizeof(v));
		m_cursor += sizeof(v);
		return v;
	}

	std::int32_t ByteReader::I32()
	{
		if (!Want(sizeof(std::int32_t)))
		{
			return 0;
		}
		std::int32_t v = 0;
		std::memcpy(&v, m_data.data() + m_cursor, sizeof(v));
		m_cursor += sizeof(v);
		return v;
	}

	float ByteReader::F32()
	{
		if (!Want(sizeof(float)))
		{
			return 0.f;
		}
		float v = 0.f;
		std::memcpy(&v, m_data.data() + m_cursor, sizeof(v));
		m_cursor += sizeof(v);
		return v;
	}

	std::string ByteReader::Str()
	{
		const std::uint32_t len = U32();
		if (!m_ok || len > kMaxStringBytes || !Want(len))
		{
			m_ok = false;
			return {};
		}
		std::string out(reinterpret_cast<const char*>(m_data.data() + m_cursor), len);
		m_cursor += len;
		return out;
	}

	std::vector<std::byte> ByteReader::Bytes(std::size_t n)
	{
		if (!Want(n))
		{
			return {};
		}
		std::vector<std::byte> out(m_data.begin() + static_cast<std::ptrdiff_t>(m_cursor),
		        m_data.begin() + static_cast<std::ptrdiff_t>(m_cursor + n));
		m_cursor += n;
		return out;
	}

	bool IsReplicableFieldType(reflect::FieldType type)
	{
		using reflect::FieldType;
		switch (type)
		{
		case FieldType::Float:
		case FieldType::Int:
		case FieldType::UInt:
		case FieldType::Bool:
		case FieldType::Vec2:
		case FieldType::Vec3:
		case FieldType::Vec4:
		case FieldType::Color3:
		case FieldType::Color4:
		case FieldType::Enum:
		case FieldType::String:
			return true;
		case FieldType::EntityRef:
		case FieldType::List:
			return false;
		}
		return false;
	}

	void WriteFieldValue(ByteWriter& w, const reflect::FieldValue& value)
	{
		using reflect::FieldType;
		switch (value.type)
		{
		case FieldType::Float:
			w.F32(static_cast<float>(value.num));
			break;
		case FieldType::Int:
			w.I32(static_cast<std::int32_t>(value.num));
			break;
		case FieldType::UInt:
			w.U32(static_cast<std::uint32_t>(value.num));
			break;
		case FieldType::Bool:
			w.U8(value.boolean ? 1u : 0u);
			break;
		case FieldType::Vec2:
			w.F32(value.vec.x);
			w.F32(value.vec.y);
			break;
		case FieldType::Vec3:
		case FieldType::Color3:
			w.F32(value.vec.x);
			w.F32(value.vec.y);
			w.F32(value.vec.z);
			break;
		case FieldType::Vec4:
		case FieldType::Color4:
			w.F32(value.vec.x);
			w.F32(value.vec.y);
			w.F32(value.vec.z);
			w.F32(value.vec.w);
			break;
		case FieldType::Enum:
			w.I32(value.enumValue);
			break;
		case FieldType::String:
			w.Str(value.str);
			break;
		case FieldType::EntityRef:
		case FieldType::List:
			break; // not replicable; the schema never includes these
		}
	}

	reflect::FieldValue ReadFieldValue(ByteReader& r, reflect::FieldType type)
	{
		using reflect::FieldType;
		reflect::FieldValue v;
		v.type = type;
		switch (type)
		{
		case FieldType::Float:
			v.num = r.F32();
			break;
		case FieldType::Int:
			v.num = r.I32();
			break;
		case FieldType::UInt:
			v.num = r.U32();
			break;
		case FieldType::Bool:
			v.boolean = r.U8() != 0;
			break;
		case FieldType::Vec2:
			v.vec.x = r.F32();
			v.vec.y = r.F32();
			break;
		case FieldType::Vec3:
		case FieldType::Color3:
			v.vec.x = r.F32();
			v.vec.y = r.F32();
			v.vec.z = r.F32();
			break;
		case FieldType::Vec4:
		case FieldType::Color4:
			v.vec.x = r.F32();
			v.vec.y = r.F32();
			v.vec.z = r.F32();
			v.vec.w = r.F32();
			break;
		case FieldType::Enum:
			v.enumValue = r.I32();
			break;
		case FieldType::String:
			v.str = r.Str();
			break;
		case FieldType::EntityRef:
		case FieldType::List:
			break;
		}
		return v;
	}
} // namespace aether::net
```

- [ ] **Step 5: Add the test include path**

`EngineTests` already includes `src/app` (`tests/CMakeLists.txt:44`, `target_include_directories(EngineTests PRIVATE "${CMAKE_SOURCE_DIR}/src/app")`), so `#include "net/NetSerialize.hpp"` resolves. The new `.cpp` under `src/app/net/` is NOT globbed into `EngineTests` — add it to the explicit app-TU list in `tests/CMakeLists.txt` alongside the reflection TUs:

```cmake
    "${CMAKE_SOURCE_DIR}/src/app/net/NetSerialize.cpp"
```

- [ ] **Step 6: Run the tests**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*ByteWriter*,*ByteReader*,*FieldValue round-trips*"
```

Expected: 4 cases PASS.

- [ ] **Step 7: Commit**

```bash
git add src/app/net tests/net/NetSerializeTests.cpp tests/CMakeLists.txt
git commit -m "Add a bounds-checked byte codec for replicated field values"
```

---

### Task 3: Replicated-field schema from the reflection registry

**Files:**
- Modify: `src/app/scene/reflection/Reflection.hpp` (`FieldMeta` ~line 43, macros ~line 452)
- Modify: `src/app/scene/reflection/CoreComponents.reflect.cpp`
- Create: `src/app/net/ReplicationSchema.hpp`, `src/app/net/ReplicationSchema.cpp`
- Test: `tests/net/ReplicationSchemaTests.cpp`

**Interfaces:**
- Consumes: `reflect::ComponentTypes()` (`Reflection.hpp:141`), `IsReplicableFieldType` (Task 2).
- Produces:
  - `FieldMeta::replicated` (bool, default false)
  - `AE_FIELD_REP(name, member, TypeTag)` macro
  - `struct ReplicatedField { std::uint16_t componentIndex; std::uint16_t fieldIndex; reflect::FieldType type; }`
  - `struct ReplicationSchema { std::vector<ReplicatedField> fields; }`
  - `ReplicationSchema BuildReplicationSchema(const std::vector<reflect::ComponentType>&)`

The schema is a **flat table**, deliberately: it is the thing a later optimisation replaces with cached member offsets, and a flat table makes that a contained change.

Component and field indices are indices into `ComponentTypes()` and its `fields` vector. Both ends must agree, which they do because both run the same binary's static registration. A version mismatch between host and client is out of scope for this plan.

- [ ] **Step 1: Write the failing test**

Create `tests/net/ReplicationSchemaTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <algorithm>

#include "net/ReplicationSchema.hpp"
#include "scene/reflection/Reflection.hpp"

using namespace aether;

TEST_CASE("The schema contains only fields marked replicated")
{
	const net::ReplicationSchema schema = net::BuildReplicationSchema(reflect::ComponentTypes());

	REQUIRE_FALSE(schema.fields.empty()); // TransformComponent's position is marked

	for (const net::ReplicatedField& f: schema.fields)
	{
		const reflect::ComponentType& ct = reflect::ComponentTypes()[f.componentIndex];
		REQUIRE(f.fieldIndex < ct.fields.size());
		const reflect::FieldDesc& fd = ct.fields[f.fieldIndex];
		CHECK(fd.meta.replicated);
		CHECK(fd.type == f.type);
		CHECK(net::IsReplicableFieldType(fd.type));
	}
}

TEST_CASE("Transform position and rotation are replicated")
{
	const net::ReplicationSchema schema = net::BuildReplicationSchema(reflect::ComponentTypes());

	auto hasField = [&](std::string_view component, std::string_view field)
	{
		return std::any_of(schema.fields.begin(), schema.fields.end(),
		        [&](const net::ReplicatedField& f)
		        {
			        const reflect::ComponentType& ct = reflect::ComponentTypes()[f.componentIndex];
			        return ct.name == component && ct.fields[f.fieldIndex].name == field;
		        });
	};

	CHECK(hasField("Transform", "position"));
	CHECK(hasField("Transform", "rotation"));
}

TEST_CASE("A replicable-type check keeps EntityRef and List out of the schema")
{
	// Guards the rule rather than the current data: if someone marks an EntityRef
	// replicated, the schema must drop it rather than emit an unserializable field.
	CHECK_FALSE(net::IsReplicableFieldType(reflect::FieldType::EntityRef));
	CHECK_FALSE(net::IsReplicableFieldType(reflect::FieldType::List));
	CHECK(net::IsReplicableFieldType(reflect::FieldType::Vec3));
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `net/ReplicationSchema.hpp` not found.

- [ ] **Step 3: Add the replicated flag and macro**

In `src/app/scene/reflection/Reflection.hpp`, add to `FieldMeta` (after `bool serialize = true;`):

```cpp
		// Marks the field for network replication. One flag here is what makes
		// "mark it replicated" a one-line change in the same declaration that
		// already drives MCP, the inspector and the serializer.
		bool replicated = false;
```

Beside `AE_FIELD_N` (~line 452), add:

```cpp
// Replicated counterpart of AE_FIELD_N: identical, plus the network schema picks it up.
#define AE_FIELD_REP(name, member, TypeTag) b.Field(name, ::aether::reflect::FieldType::TypeTag, &C::member, ::aether::reflect::FieldMeta{.replicated = true});
```

- [ ] **Step 4: Mark the transform fields**

In `src/app/scene/reflection/CoreComponents.reflect.cpp`, find the `TransformComponent` block and change its `position` and `rotation` fields from `AE_FIELD_N` to `AE_FIELD_REP`, leaving the field names and type tags exactly as they are. Do **not** mark `scale` — nothing in Whisper changes it, and every replicated field costs bandwidth on every entity.

- [ ] **Step 5: Write the schema builder**

Create `src/app/net/ReplicationSchema.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <vector>

#include "net/NetSerialize.hpp"
#include "scene/reflection/Reflection.hpp"

namespace aether::net
{
	// One replicated field, addressed by its position in the component catalog.
	// Both ends run the same binary's static registration, so the indices agree
	// without a handshake.
	struct ReplicatedField
	{
		std::uint16_t componentIndex = 0;
		std::uint16_t fieldIndex = 0;
		reflect::FieldType type = reflect::FieldType::Float;
	};

	// A flat table, deliberately: this is the structure a later optimisation swaps
	// for cached member offsets, and flat keeps that change contained.
	struct ReplicationSchema
	{
		std::vector<ReplicatedField> fields;
	};

	[[nodiscard]] ReplicationSchema BuildReplicationSchema(const std::vector<reflect::ComponentType>& catalog);
} // namespace aether::net
```

Create `src/app/net/ReplicationSchema.cpp`:

```cpp
#include "net/ReplicationSchema.hpp"

namespace aether::net
{
	ReplicationSchema BuildReplicationSchema(const std::vector<reflect::ComponentType>& catalog)
	{
		ReplicationSchema schema;
		for (std::size_t c = 0; c < catalog.size(); ++c)
		{
			const reflect::ComponentType& type = catalog[c];
			for (std::size_t f = 0; f < type.fields.size(); ++f)
			{
				const reflect::FieldDesc& field = type.fields[f];
				// A field can be marked replicated but hold a type replication cannot
				// carry (an entity ref means nothing on another machine). Drop it here
				// rather than emit a field the codec would silently skip.
				if (!field.meta.replicated || !IsReplicableFieldType(field.type))
				{
					continue;
				}
				schema.fields.push_back(ReplicatedField{
				        .componentIndex = static_cast<std::uint16_t>(c),
				        .fieldIndex = static_cast<std::uint16_t>(f),
				        .type = field.type,
				});
			}
		}
		return schema;
	}
} // namespace aether::net
```

- [ ] **Step 6: Add the TU to the test target**

In `tests/CMakeLists.txt`, beside the `NetSerialize.cpp` entry:

```cmake
    "${CMAKE_SOURCE_DIR}/src/app/net/ReplicationSchema.cpp"
```

- [ ] **Step 7: Run the tests**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*schema*,*replicated*,*replicable-type*"
```

Expected: 3 cases PASS. If "Transform position and rotation are replicated" fails, the component's registered display name is not `"Transform"` — check the `AE_COMPONENT` line in `CoreComponents.reflect.cpp` and use the actual name in both the test and this step.

- [ ] **Step 8: Run the whole suite and commit**

```bash
./build/ninja-clang/tests/EngineTests.exe
```

Expected: all pass. The scene serializer round-trip tests exercise `FieldMeta`, so a mistake in the struct surfaces here.

```bash
git add src/app/net src/app/scene/reflection tests/net/ReplicationSchemaTests.cpp tests/CMakeLists.txt
git commit -m "Build a replicated-field schema from the component catalog

- Add FieldMeta::replicated and the AE_FIELD_REP macro
- Mark transform position and rotation replicated"
```

---

### Task 4: Session state and network identity

**Files:**
- Create: `src/app/net/NetComponents.hpp`, `src/app/net/NetSession.hpp`, `src/app/net/NetSession.cpp`
- Modify: `src/app/scene/reflection/MoreComponents.reflect.cpp`
- Test: `tests/net/NetSessionTests.cpp`

**Interfaces:**
- Consumes: `NetRole`, `ConnectionId` (Task 1).
- Produces:
  - `struct NetworkIdentity { std::uint32_t netId; ConnectionId owner; std::string spawnPrefab; bool scenePlaced; }`
  - `struct NetworkTransform { float interpolationDelaySeconds = 0.1f; float correctionRate = 12.f; float snapDistance = 4.f; }`
  - `struct NetPlayer { std::string displayName; }`
  - `class NetSession` — `Role()`, `SetRole()`, `AllocateNetId() -> std::uint32_t`, `Bind(netId, Entity)`, `Unbind(netId)`, `EntityFor(netId) -> Entity`, `NetIdFor(Entity) -> std::uint32_t`, `Clear()`, `Connections()`, `AddConnection`, `RemoveConnection`, `LocalConnection()`

`NetPlayer` is a **framework** component, not game code: every multiplayer project wants a display name for a connected player, and putting it here is what lets name tags, chat attribution and disconnect notices read one source.

- [ ] **Step 1: Write the failing test**

Create `tests/net/NetSessionTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include "net/NetSession.hpp"
#include "scene/World.hpp"

using namespace aether;

TEST_CASE("NetSession allocates monotonic ids and maps them both ways")
{
	net::NetSession session;
	World w;

	const Entity a = w.Create();
	const Entity b = w.Create();

	const std::uint32_t idA = session.AllocateNetId();
	const std::uint32_t idB = session.AllocateNetId();
	CHECK(idA != 0);
	CHECK(idB != idA);

	session.Bind(idA, a);
	session.Bind(idB, b);

	CHECK(session.EntityFor(idA) == a);
	CHECK(session.EntityFor(idB) == b);
	CHECK(session.NetIdFor(a) == idA);
	CHECK(session.NetIdFor(b) == idB);
}

TEST_CASE("Unbinding removes both directions")
{
	net::NetSession session;
	World w;
	const Entity e = w.Create();

	const std::uint32_t id = session.AllocateNetId();
	session.Bind(id, e);
	session.Unbind(id);

	CHECK_FALSE(session.EntityFor(id).IsValid());
	CHECK(session.NetIdFor(e) == 0);
}

TEST_CASE("An unknown net id resolves to an invalid entity rather than a stale one")
{
	net::NetSession session;
	CHECK_FALSE(session.EntityFor(12345).IsValid());
	CHECK(session.NetIdFor(Entity{}) == 0);
}

TEST_CASE("Connections are tracked and cleared with the session")
{
	net::NetSession session;
	session.SetRole(net::NetRole::Host);
	session.AddConnection(1);
	session.AddConnection(2);
	CHECK(session.Connections().size() == 2);

	session.RemoveConnection(1);
	CHECK(session.Connections().size() == 1);

	session.Clear();
	CHECK(session.Connections().empty());
	CHECK(session.Role() == net::NetRole::Offline);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `net/NetSession.hpp` not found.

- [ ] **Step 3: Write the components**

Create `src/app/net/NetComponents.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "net/NetTypes.hpp"

namespace aether::net
{
	// Marks an entity as replicated and carries its network identity. `netId` is
	// host-assigned and stable for the entity's lifetime; `owner` is the connection
	// allowed to drive it (0 = the host owns it).
	struct NetworkIdentity
	{
		std::uint32_t netId = 0;
		ConnectionId owner = kInvalidConnection;
		// Prefab this entity was spawned from, so a joining client can recreate it.
		// Empty for scene-placed entities, which both ends already have.
		std::string spawnPrefab;
		bool scenePlaced = false;
	};

	// Smoothing for a replicated entity's transform. On remote entities the buffer
	// is rendered `interpolationDelaySeconds` in the past so motion is smooth
	// between packets; on the locally-owned (predicted) entity the authoritative
	// position is eased in at `correctionRate` instead of snapping - unless the
	// error exceeds `snapDistance`, where easing would look worse than a cut.
	struct NetworkTransform
	{
		float interpolationDelaySeconds = 0.1f;
		float correctionRate = 12.f;
		float snapDistance = 4.f;
	};

	// A connected player's display name. Framework-level rather than game-level:
	// name tags, chat attribution and disconnect notices all read this one field
	// instead of each tracking names separately.
	struct NetPlayer
	{
		std::string displayName;
	};
} // namespace aether::net
```

- [ ] **Step 4: Write the session**

Create `src/app/net/NetSession.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "net/NetTypes.hpp"
#include "scene/Entity.hpp"

namespace aether::net
{
	// Who we are on the network and what net ids map to what entities. Holds no
	// sockets - the transport is separate - so it is trivially testable.
	class NetSession
	{
	public:
		[[nodiscard]] NetRole Role() const
		{
			return m_role;
		}

		void SetRole(NetRole role)
		{
			m_role = role;
		}

		[[nodiscard]] bool IsHost() const
		{
			return m_role == NetRole::Host;
		}

		[[nodiscard]] ConnectionId LocalConnection() const
		{
			return m_localConnection;
		}

		void SetLocalConnection(ConnectionId id)
		{
			m_localConnection = id;
		}

		// Host-only. Ids are never reused within a session, so a late packet
		// referencing a despawned entity resolves to nothing rather than to
		// whatever entity happened to reuse the id.
		std::uint32_t AllocateNetId()
		{
			return m_nextNetId++;
		}

		void Bind(std::uint32_t netId, Entity entity);
		void Unbind(std::uint32_t netId);

		[[nodiscard]] Entity EntityFor(std::uint32_t netId) const;
		[[nodiscard]] std::uint32_t NetIdFor(Entity entity) const;

		void AddConnection(ConnectionId id);
		void RemoveConnection(ConnectionId id);

		[[nodiscard]] const std::vector<ConnectionId>& Connections() const
		{
			return m_connections;
		}

		void Clear();

	private:
		NetRole m_role = NetRole::Offline;
		ConnectionId m_localConnection = kInvalidConnection;
		std::uint32_t m_nextNetId = 1;
		std::unordered_map<std::uint32_t, Entity> m_byNetId;
		std::unordered_map<std::uint32_t, std::uint32_t> m_netIdByEntity; // Entity::id -> netId
		std::vector<ConnectionId> m_connections;
	};
} // namespace aether::net
```

Create `src/app/net/NetSession.cpp`:

```cpp
#include "net/NetSession.hpp"

#include <algorithm>

namespace aether::net
{
	void NetSession::Bind(std::uint32_t netId, Entity entity)
	{
		m_byNetId[netId] = entity;
		m_netIdByEntity[entity.id] = netId;
	}

	void NetSession::Unbind(std::uint32_t netId)
	{
		if (const auto it = m_byNetId.find(netId); it != m_byNetId.end())
		{
			m_netIdByEntity.erase(it->second.id);
			m_byNetId.erase(it);
		}
	}

	Entity NetSession::EntityFor(std::uint32_t netId) const
	{
		const auto it = m_byNetId.find(netId);
		return it == m_byNetId.end() ? Entity{} : it->second;
	}

	std::uint32_t NetSession::NetIdFor(Entity entity) const
	{
		const auto it = m_netIdByEntity.find(entity.id);
		return it == m_netIdByEntity.end() ? 0u : it->second;
	}

	void NetSession::AddConnection(ConnectionId id)
	{
		if (std::find(m_connections.begin(), m_connections.end(), id) == m_connections.end())
		{
			m_connections.push_back(id);
		}
	}

	void NetSession::RemoveConnection(ConnectionId id)
	{
		m_connections.erase(std::remove(m_connections.begin(), m_connections.end(), id), m_connections.end());
	}

	void NetSession::Clear()
	{
		m_role = NetRole::Offline;
		m_localConnection = kInvalidConnection;
		m_nextNetId = 1;
		m_byNetId.clear();
		m_netIdByEntity.clear();
		m_connections.clear();
	}
} // namespace aether::net
```

- [ ] **Step 5: Reflect the components for authoring**

In `src/app/scene/reflection/MoreComponents.reflect.cpp`, add to the alias block near the top:

```cpp
using NetworkIdentityComponent = aether::net::NetworkIdentity;
using NetworkTransformComponent = aether::net::NetworkTransform;
using NetPlayerComponent = aether::net::NetPlayer;
```

Add `#include "net/NetComponents.hpp"` to the file's includes, and after the UI component blocks:

```cpp
AE_COMPONENT(NetworkIdentityComponent, "Network Identity", "Networking", ICON_FA_TOWER_BROADCAST)
AE_COMPONENT_END()

AE_COMPONENT(NetworkTransformComponent, "Network Transform", "Networking", ICON_FA_ARROWS_LEFT_RIGHT)
AE_FIELD_N("interpolation_delay", interpolationDelaySeconds, Float)
AE_FIELD_N("correction_rate", correctionRate, Float)
AE_FIELD_N("snap_distance", snapDistance, Float)
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(NetPlayerComponent, "Net Player", "Networking", ICON_FA_USER)
AE_FIELD_REP("display_name", displayName, String)
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()
```

`NetworkIdentity` reflects **no fields**: `netId` and `owner` are runtime state assigned by the host, and serializing them into a scene would restore stale ids. It is registered only so the component can be added from the palette and MCP. Verify `ICON_FA_TOWER_BROADCAST`, `ICON_FA_ARROWS_LEFT_RIGHT` and `ICON_FA_USER` exist in `src/app/debug/Icons.hpp`; substitute any that do not and note the substitution in your report.

- [ ] **Step 6: Add the TU and run**

Add to `tests/CMakeLists.txt`:

```cmake
    "${CMAKE_SOURCE_DIR}/src/app/net/NetSession.cpp"
```

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*NetSession*,*net id*,*Connections are tracked*"
```

Expected: 4 cases PASS.

- [ ] **Step 7: Run the whole suite and commit**

```bash
./build/ninja-clang/tests/EngineTests.exe
```

```bash
git add src/app/net src/app/scene/reflection/MoreComponents.reflect.cpp tests/net/NetSessionTests.cpp tests/CMakeLists.txt
git commit -m "Add network identity components and session state"
```

---

### Task 5: Snapshot build, apply and change detection

**Files:**
- Create: `src/app/net/NetSnapshot.hpp`, `src/app/net/NetSnapshot.cpp`
- Test: `tests/net/NetSnapshotTests.cpp`

**Interfaces:**
- Consumes: `ReplicationSchema`, `ReplicatedField` (Task 3); `ByteWriter`/`ByteReader`, `WriteFieldValue`/`ReadFieldValue` (Task 2); `NetworkIdentity`, `NetSession` (Task 4).
- Produces:
  - `struct FieldKey { std::uint32_t netId; std::uint16_t componentIndex; std::uint16_t fieldIndex; }` with `operator==` and a `std::hash` specialisation
  - `class SnapshotCache` — `bool Changed(FieldKey, const reflect::FieldValue&)` (records the new value and returns whether it differed), `void Clear()`, `void Forget(std::uint32_t netId)`
  - `std::vector<std::byte> BuildSnapshot(World&, const ReplicationSchema&, const std::vector<reflect::ComponentType>&, NetSession&, SnapshotCache&, const std::vector<Entity>& relevant)`
  - `void ApplySnapshot(World&, const ReplicationSchema&, const std::vector<reflect::ComponentType>&, NetSession&, std::span<const std::byte>)`

Wire format: `u16 fieldCount`, then per field `u32 netId, u16 componentIndex, u16 fieldIndex, <value>`. A snapshot with zero changed fields is not sent at all.

`ApplySnapshot` must skip unknown net ids, out-of-range indices and entities missing the component, rather than trusting the packet.

- [ ] **Step 1: Write the failing tests**

Create `tests/net/NetSnapshotTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include "net/NetSnapshot.hpp"
#include "net/NetSession.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"

using namespace aether;

namespace
{
	// A host world and a client world, with one replicated entity bound to the same
	// net id on both sides - the minimal setup replication actually operates on.
	struct TwoWorlds
	{
		World host;
		World client;
		net::NetSession hostSession;
		net::NetSession clientSession;
		Entity hostEntity;
		Entity clientEntity;
		net::ReplicationSchema schema = net::BuildReplicationSchema(reflect::ComponentTypes());

		TwoWorlds()
		{
			hostEntity = host.Create();
			host.Emplace<TransformComponent>(hostEntity);
			host.Emplace<net::NetworkIdentity>(hostEntity).netId = 1;
			hostSession.Bind(1, hostEntity);

			clientEntity = client.Create();
			client.Emplace<TransformComponent>(clientEntity);
			client.Emplace<net::NetworkIdentity>(clientEntity).netId = 1;
			clientSession.Bind(1, clientEntity);
		}
	};
} // namespace

TEST_CASE("A changed replicated field reaches the client world")
{
	TwoWorlds tw;
	net::SnapshotCache cache;

	tw.host.TryGet<TransformComponent>(tw.hostEntity)->position = {5.f, 6.f, 7.f};

	const std::vector<std::byte> packet = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {tw.hostEntity});
	REQUIRE_FALSE(packet.empty());

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, packet);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	CHECK(t->position.x == doctest::Approx(5.f));
	CHECK(t->position.z == doctest::Approx(7.f));
}

TEST_CASE("An unchanged field produces no snapshot at all")
{
	TwoWorlds tw;
	net::SnapshotCache cache;

	tw.host.TryGet<TransformComponent>(tw.hostEntity)->position = {1.f, 0.f, 0.f};

	// First build sends everything; the second has nothing to say.
	const std::vector<std::byte> first = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {tw.hostEntity});
	CHECK_FALSE(first.empty());

	const std::vector<std::byte> second = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {tw.hostEntity});
	CHECK(second.empty());
}

TEST_CASE("Only entities in the relevant set are included")
{
	TwoWorlds tw;
	net::SnapshotCache cache;
	tw.host.TryGet<TransformComponent>(tw.hostEntity)->position = {9.f, 9.f, 9.f};

	// Relevant set is empty, so nothing is sent even though the field changed.
	const std::vector<std::byte> packet = net::BuildSnapshot(
	        tw.host, tw.schema, reflect::ComponentTypes(), tw.hostSession, cache, {});
	CHECK(packet.empty());
}

TEST_CASE("A snapshot naming an unknown net id is ignored, not applied blindly")
{
	TwoWorlds tw;

	net::ByteWriter w;
	w.U16(1);        // one field
	w.U32(9999);     // net id nobody has
	w.U16(0);
	w.U16(0);
	w.F32(1.f);
	const std::vector<std::byte> hostile = w.Take();

	// Must not crash, and must leave the client world untouched.
	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, hostile);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	CHECK(t->position.x == doctest::Approx(0.f));
}

TEST_CASE("A truncated snapshot is rejected without applying a partial field")
{
	TwoWorlds tw;

	net::ByteWriter w;
	w.U16(1);
	w.U32(1);
	w.U16(0);
	// component index written, field index and value missing
	const std::vector<std::byte> truncated = w.Take();

	net::ApplySnapshot(tw.client, tw.schema, reflect::ComponentTypes(), tw.clientSession, truncated);

	const auto* t = tw.client.TryGet<TransformComponent>(tw.clientEntity);
	REQUIRE(t != nullptr);
	CHECK(t->position.x == doctest::Approx(0.f));
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `net/NetSnapshot.hpp` not found.

- [ ] **Step 3: Write the header**

Create `src/app/net/NetSnapshot.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "net/NetSerialize.hpp"
#include "net/NetSession.hpp"
#include "net/ReplicationSchema.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	// Identifies one replicated field on one networked entity.
	struct FieldKey
	{
		std::uint32_t netId = 0;
		std::uint16_t componentIndex = 0;
		std::uint16_t fieldIndex = 0;

		friend bool operator==(const FieldKey& a, const FieldKey& b)
		{
			return a.netId == b.netId && a.componentIndex == b.componentIndex && a.fieldIndex == b.fieldIndex;
		}
	};
} // namespace aether::net

template<>
struct std::hash<aether::net::FieldKey>
{
	std::size_t operator()(const aether::net::FieldKey& k) const noexcept
	{
		return (static_cast<std::size_t>(k.netId) * 1315423911u) ^ (static_cast<std::size_t>(k.componentIndex) << 16)
		       ^ static_cast<std::size_t>(k.fieldIndex);
	}
};

namespace aether::net
{
	// Last value sent per field, so a snapshot carries only what actually changed.
	// This is per-field change detection, NOT delta-against-acknowledged: a dropped
	// snapshot is not resent, the next change simply includes the field again.
	class SnapshotCache
	{
	public:
		// Records `value` and returns true if it differs from the last recorded one
		// (or if this field has never been seen).
		bool Changed(const FieldKey& key, const reflect::FieldValue& value);
		void Forget(std::uint32_t netId);
		void Clear();

	private:
		std::unordered_map<FieldKey, reflect::FieldValue> m_last;
	};

	// Host side. Returns an empty vector when nothing changed - callers must not
	// send an empty packet.
	[[nodiscard]] std::vector<std::byte> BuildSnapshot(World& world, const ReplicationSchema& schema,
	        const std::vector<reflect::ComponentType>& catalog, NetSession& session, SnapshotCache& cache,
	        const std::vector<Entity>& relevant);

	// Client side. Ignores unknown net ids, out-of-range indices, entities missing
	// the component, and truncated packets - a peer can send anything.
	void ApplySnapshot(World& world, const ReplicationSchema& schema,
	        const std::vector<reflect::ComponentType>& catalog, NetSession& session, std::span<const std::byte> packet);
} // namespace aether::net
```

- [ ] **Step 4: Write the implementation**

Create `src/app/net/NetSnapshot.cpp`:

```cpp
#include "net/NetSnapshot.hpp"

#include "net/NetComponents.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	namespace
	{
		// FieldValue has no operator==; compare only the members the field's type uses.
		bool SameValue(const reflect::FieldValue& a, const reflect::FieldValue& b)
		{
			using reflect::FieldType;
			if (a.type != b.type)
			{
				return false;
			}
			switch (a.type)
			{
			case FieldType::Float:
			case FieldType::Int:
			case FieldType::UInt:
				return a.num == b.num;
			case FieldType::Bool:
				return a.boolean == b.boolean;
			case FieldType::Vec2:
			case FieldType::Vec3:
			case FieldType::Vec4:
			case FieldType::Color3:
			case FieldType::Color4:
				return a.vec == b.vec;
			case FieldType::Enum:
				return a.enumValue == b.enumValue;
			case FieldType::String:
				return a.str == b.str;
			case FieldType::EntityRef:
			case FieldType::List:
				return true; // never replicated
			}
			return false;
		}
	} // namespace

	bool SnapshotCache::Changed(const FieldKey& key, const reflect::FieldValue& value)
	{
		const auto it = m_last.find(key);
		if (it != m_last.end() && SameValue(it->second, value))
		{
			return false;
		}
		m_last[key] = value;
		return true;
	}

	void SnapshotCache::Forget(std::uint32_t netId)
	{
		for (auto it = m_last.begin(); it != m_last.end();)
		{
			it = it->first.netId == netId ? m_last.erase(it) : std::next(it);
		}
	}

	void SnapshotCache::Clear()
	{
		m_last.clear();
	}

	std::vector<std::byte> BuildSnapshot(World& world, const ReplicationSchema& schema,
	        const std::vector<reflect::ComponentType>& catalog, NetSession& session, SnapshotCache& cache,
	        const std::vector<Entity>& relevant)
	{
		ByteWriter body;
		std::uint16_t count = 0;

		for (const Entity entity: relevant)
		{
			const std::uint32_t netId = session.NetIdFor(entity);
			if (netId == 0)
			{
				continue;
			}
			for (const ReplicatedField& field: schema.fields)
			{
				const reflect::ComponentType& type = catalog[field.componentIndex];
				const void* component = type.tryGetRawConst(world, entity);
				if (component == nullptr)
				{
					continue;
				}
				const reflect::FieldValue value = type.fields[field.fieldIndex].get(component);
				const FieldKey key{.netId = netId, .componentIndex = field.componentIndex, .fieldIndex = field.fieldIndex};
				if (!cache.Changed(key, value))
				{
					continue;
				}
				body.U32(netId);
				body.U16(field.componentIndex);
				body.U16(field.fieldIndex);
				WriteFieldValue(body, value);
				++count;
			}
		}

		if (count == 0)
		{
			return {};
		}

		ByteWriter packet;
		packet.U16(count);
		packet.Bytes(body.View());
		return packet.Take();
	}

	void ApplySnapshot(World& world, const ReplicationSchema& schema,
	        const std::vector<reflect::ComponentType>& catalog, NetSession& session, std::span<const std::byte> packet)
	{
		(void) schema; // indices are validated against the catalog directly
		ByteReader r{packet};
		const std::uint16_t count = r.U16();

		for (std::uint16_t i = 0; i < count; ++i)
		{
			const std::uint32_t netId = r.U32();
			const std::uint16_t componentIndex = r.U16();
			const std::uint16_t fieldIndex = r.U16();
			if (!r.Ok() || componentIndex >= catalog.size())
			{
				return; // malformed past this point; the rest of the packet is unparseable
			}

			const reflect::ComponentType& type = catalog[componentIndex];
			if (fieldIndex >= type.fields.size())
			{
				return;
			}
			const reflect::FieldDesc& field = type.fields[fieldIndex];

			// The value must be consumed even when the target is unknown, or the
			// cursor desyncs and every remaining field in the packet is garbage.
			const reflect::FieldValue value = ReadFieldValue(r, field.type);
			if (!r.Ok())
			{
				return;
			}

			const Entity entity = session.EntityFor(netId);
			if (!entity.IsValid())
			{
				continue;
			}
			void* component = type.tryGetRaw(world, entity);
			if (component == nullptr)
			{
				continue;
			}
			field.set(component, value);
		}
	}
} // namespace aether::net
```

- [ ] **Step 5: Add the TU and run the tests**

Add to `tests/CMakeLists.txt`:

```cmake
    "${CMAKE_SOURCE_DIR}/src/app/net/NetSnapshot.cpp"
```

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*snapshot*,*replicated field reaches*,*relevant set*"
```

Expected: 5 cases PASS.

- [ ] **Step 6: Run the whole suite and commit**

```bash
./build/ninja-clang/tests/EngineTests.exe
```

```bash
git add src/app/net tests/net/NetSnapshotTests.cpp tests/CMakeLists.txt
git commit -m "Replicate changed component fields between worlds"
```

---

### Task 6: Relevancy filtering

**Files:**
- Create: `src/app/net/NetRelevancy.hpp`, `src/app/net/NetRelevancy.cpp`
- Test: `tests/net/NetRelevancyTests.cpp`

**Interfaces:**
- Consumes: `NetworkIdentity` (Task 4).
- Produces:
  - `struct RelevancySettings { float radius = 60.f; bool enabled = true; }`
  - `std::vector<Entity> RelevantFor(World&, ConnectionId viewer, glm::vec3 viewerPos, const RelevancySettings&)`

An entity is relevant to a connection when it is within `radius` of that connection's view position, **or** when the connection owns it (you always receive your own entity, however far it has travelled — otherwise a client that falls out of the world can never be told where it is).

- [ ] **Step 1: Write the failing tests**

Create `tests/net/NetRelevancyTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <algorithm>

#include "net/NetRelevancy.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
	Entity MakeNetworked(World& w, glm::vec3 pos, net::ConnectionId owner)
	{
		const Entity e = w.Create();
		w.Emplace<TransformComponent>(e).position = pos;
		auto& id = w.Emplace<net::NetworkIdentity>(e);
		id.netId = static_cast<std::uint32_t>(e.id);
		id.owner = owner;
		return e;
	}

	bool Contains(const std::vector<Entity>& v, Entity e)
	{
		return std::find(v.begin(), v.end(), e) != v.end();
	}
} // namespace

TEST_CASE("Only entities inside the radius are relevant")
{
	World w;
	const Entity near = MakeNetworked(w, {5.f, 0.f, 0.f}, 0);
	const Entity far = MakeNetworked(w, {500.f, 0.f, 0.f}, 0);

	net::RelevancySettings settings;
	settings.radius = 60.f;

	const std::vector<Entity> relevant = net::RelevantFor(w, 1, glm::vec3{0.f}, settings);
	CHECK(Contains(relevant, near));
	CHECK_FALSE(Contains(relevant, far));
}

TEST_CASE("A connection always receives the entity it owns, however distant")
{
	World w;
	const Entity mine = MakeNetworked(w, {9999.f, 0.f, 0.f}, 7);

	net::RelevancySettings settings;
	settings.radius = 10.f;

	const std::vector<Entity> relevant = net::RelevantFor(w, 7, glm::vec3{0.f}, settings);
	CHECK(Contains(relevant, mine));
}

TEST_CASE("Disabling relevancy returns every networked entity")
{
	World w;
	const Entity a = MakeNetworked(w, {0.f, 0.f, 0.f}, 0);
	const Entity b = MakeNetworked(w, {10000.f, 0.f, 0.f}, 0);

	net::RelevancySettings settings;
	settings.enabled = false;
	settings.radius = 1.f;

	const std::vector<Entity> relevant = net::RelevantFor(w, 1, glm::vec3{0.f}, settings);
	CHECK(Contains(relevant, a));
	CHECK(Contains(relevant, b));
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `net/NetRelevancy.hpp` not found.

- [ ] **Step 3: Implement**

Create `src/app/net/NetRelevancy.hpp`:

```cpp
#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "net/NetComponents.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	struct RelevancySettings
	{
		float radius = 60.f;
		bool enabled = true;
	};

	// Entities the given connection should receive this tick. A connection always
	// receives what it owns regardless of distance - otherwise a player who falls
	// out of the world can never be told where they are.
	[[nodiscard]] std::vector<Entity> RelevantFor(World& world, ConnectionId viewer, glm::vec3 viewerPos,
	        const RelevancySettings& settings);
} // namespace aether::net
```

Create `src/app/net/NetRelevancy.cpp`:

```cpp
#include "net/NetRelevancy.hpp"

#include <entt/entt.hpp>

#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	std::vector<Entity> RelevantFor(World& world, ConnectionId viewer, glm::vec3 viewerPos,
	        const RelevancySettings& settings)
	{
		std::vector<Entity> out;
		const float radiusSq = settings.radius * settings.radius;

		world.View<NetworkIdentity, TransformComponent>().each(
		        [&](entt::entity ent, NetworkIdentity& id, TransformComponent& transform)
		        {
			        const Entity e = World::FromEntt(ent);
			        if (!settings.enabled || id.owner == viewer)
			        {
				        out.push_back(e);
				        return;
			        }
			        const glm::vec3 d = transform.position - viewerPos;
			        if (glm::dot(d, d) <= radiusSq)
			        {
				        out.push_back(e);
			        }
		        });
		return out;
	}
} // namespace aether::net
```

- [ ] **Step 4: Add the TU, run, commit**

Add to `tests/CMakeLists.txt`:

```cmake
    "${CMAKE_SOURCE_DIR}/src/app/net/NetRelevancy.cpp"
```

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*relevant*,*radius*,*relevancy*"
```

Expected: 3 cases PASS. Then the full suite:

```bash
./build/ninja-clang/tests/EngineTests.exe
```

```bash
git add src/app/net tests/net/NetRelevancyTests.cpp tests/CMakeLists.txt
git commit -m "Filter replicated entities by per-connection relevancy"
```

---

### Task 7: Snapshot interpolation buffer

**Files:**
- Create: `src/app/net/NetInterpolation.hpp`, `src/app/net/NetInterpolation.cpp`
- Test: `tests/net/NetInterpolationTests.cpp`

**Interfaces:**
- Consumes: nothing (pure).
- Produces:
  - `struct TransformSample { float time; glm::vec3 position; glm::vec3 rotation; }`
  - `class InterpolationBuffer` — `Push(TransformSample)`, `Sample(float renderTime) -> std::optional<TransformSample>`, `Clear()`, `Size()`
  - `glm::vec3 EaseToward(glm::vec3 current, glm::vec3 target, float rate, float dt, float snapDistance)`

The buffer renders remote entities in the past. `Sample` interpolates between the two samples bracketing `renderTime`; before the first sample it returns the first, after the last it returns the last (holding position is better than extrapolating into a wall). Out-of-order samples are inserted in time order — UDP reorders.

- [ ] **Step 1: Write the failing tests**

Create `tests/net/NetInterpolationTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include "net/NetInterpolation.hpp"

using namespace aether;

TEST_CASE("Sampling between two snapshots interpolates position")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 0.f, .position = {0.f, 0.f, 0.f}});
	buf.Push({.time = 1.f, .position = {10.f, 0.f, 0.f}});

	const auto mid = buf.Sample(0.5f);
	REQUIRE(mid.has_value());
	CHECK(mid->position.x == doctest::Approx(5.f));

	const auto quarter = buf.Sample(0.25f);
	REQUIRE(quarter.has_value());
	CHECK(quarter->position.x == doctest::Approx(2.5f));
}

TEST_CASE("Sampling outside the buffer holds the end rather than extrapolating")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 1.f, .position = {1.f, 0.f, 0.f}});
	buf.Push({.time = 2.f, .position = {2.f, 0.f, 0.f}});

	const auto before = buf.Sample(0.f);
	REQUIRE(before.has_value());
	CHECK(before->position.x == doctest::Approx(1.f));

	const auto after = buf.Sample(99.f);
	REQUIRE(after.has_value());
	CHECK(after->position.x == doctest::Approx(2.f));
}

TEST_CASE("An empty buffer yields nothing")
{
	net::InterpolationBuffer buf;
	CHECK_FALSE(buf.Sample(0.f).has_value());
}

TEST_CASE("Out-of-order samples are stored in time order")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 2.f, .position = {20.f, 0.f, 0.f}});
	buf.Push({.time = 1.f, .position = {10.f, 0.f, 0.f}}); // arrived late

	const auto mid = buf.Sample(1.5f);
	REQUIRE(mid.has_value());
	CHECK(mid->position.x == doctest::Approx(15.f));
}

TEST_CASE("The buffer discards samples far older than the render window")
{
	net::InterpolationBuffer buf;
	for (int i = 0; i < 200; ++i)
	{
		buf.Push({.time = static_cast<float>(i) * 0.05f, .position = {static_cast<float>(i), 0.f, 0.f}});
	}
	// Unbounded growth over a long session is a leak; the buffer keeps a bounded window.
	CHECK(buf.Size() <= 64);
}

TEST_CASE("EaseToward converges, and snaps past the snap distance")
{
	const glm::vec3 eased = net::EaseToward({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 10.f, 1.f / 60.f, 4.f);
	CHECK(eased.x > 0.f);
	CHECK(eased.x < 1.f); // moved toward, not all the way

	const glm::vec3 snapped = net::EaseToward({0.f, 0.f, 0.f}, {100.f, 0.f, 0.f}, 10.f, 1.f / 60.f, 4.f);
	CHECK(snapped.x == doctest::Approx(100.f)); // beyond snap distance: cut, don't glide
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `net/NetInterpolation.hpp` not found.

- [ ] **Step 3: Implement**

Create `src/app/net/NetInterpolation.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace aether::net
{
	struct TransformSample
	{
		float time = 0.f;
		glm::vec3 position{0.f};
		glm::vec3 rotation{0.f};
	};

	// Holds recent authoritative samples for one remote entity so it can be rendered
	// slightly in the past, which is what turns discrete packets into smooth motion.
	class InterpolationBuffer
	{
	public:
		// Inserts in time order: UDP delivers out of order, and a late sample still
		// carries information about the interval it belongs to.
		void Push(const TransformSample& sample);

		// Interpolates between the samples bracketing `renderTime`. Outside the
		// buffer's range it holds the nearest end - extrapolating a player forward
		// sends them through walls, and a brief freeze reads better than a rubber-band.
		[[nodiscard]] std::optional<TransformSample> Sample(float renderTime) const;

		void Clear();

		[[nodiscard]] std::size_t Size() const
		{
			return m_samples.size();
		}

	private:
		// A few seconds at typical send rates. Bounded so a long session cannot grow
		// this without limit.
		static constexpr std::size_t kMaxSamples = 64;
		std::vector<TransformSample> m_samples;
	};

	// Frame-rate independent ease toward an authoritative position. Past
	// `snapDistance` it cuts instead: gliding a player across a large error looks
	// far worse than a single jump, and usually means they were teleported anyway.
	[[nodiscard]] glm::vec3 EaseToward(glm::vec3 current, glm::vec3 target, float rate, float dt, float snapDistance);
} // namespace aether::net
```

Create `src/app/net/NetInterpolation.cpp`:

```cpp
#include "net/NetInterpolation.hpp"

#include <algorithm>
#include <cmath>

namespace aether::net
{
	void InterpolationBuffer::Push(const TransformSample& sample)
	{
		const auto at = std::lower_bound(m_samples.begin(), m_samples.end(), sample.time,
		        [](const TransformSample& s, float t) { return s.time < t; });
		m_samples.insert(at, sample);

		if (m_samples.size() > kMaxSamples)
		{
			m_samples.erase(m_samples.begin(), m_samples.begin() + static_cast<std::ptrdiff_t>(m_samples.size() - kMaxSamples));
		}
	}

	std::optional<TransformSample> InterpolationBuffer::Sample(float renderTime) const
	{
		if (m_samples.empty())
		{
			return std::nullopt;
		}
		if (renderTime <= m_samples.front().time)
		{
			return m_samples.front();
		}
		if (renderTime >= m_samples.back().time)
		{
			return m_samples.back();
		}

		for (std::size_t i = 1; i < m_samples.size(); ++i)
		{
			const TransformSample& b = m_samples[i];
			if (b.time < renderTime)
			{
				continue;
			}
			const TransformSample& a = m_samples[i - 1];
			const float span = b.time - a.time;
			const float t = span > 1e-6f ? (renderTime - a.time) / span : 0.f;
			TransformSample out;
			out.time = renderTime;
			out.position = glm::mix(a.position, b.position, t);
			out.rotation = glm::mix(a.rotation, b.rotation, t);
			return out;
		}
		return m_samples.back();
	}

	void InterpolationBuffer::Clear()
	{
		m_samples.clear();
	}

	glm::vec3 EaseToward(glm::vec3 current, glm::vec3 target, float rate, float dt, float snapDistance)
	{
		const glm::vec3 delta = target - current;
		if (glm::dot(delta, delta) >= snapDistance * snapDistance)
		{
			return target;
		}
		// Exponential decay, so the result does not depend on frame rate.
		const float t = 1.f - std::exp(-std::max(rate, 0.f) * std::max(dt, 0.f));
		return current + delta * t;
	}
} // namespace aether::net
```

- [ ] **Step 4: Add the TU, run, commit**

Add to `tests/CMakeLists.txt`:

```cmake
    "${CMAKE_SOURCE_DIR}/src/app/net/NetInterpolation.cpp"
```

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*interpolat*,*Sampling*,*EaseToward*,*buffer*"
```

Expected: 6 cases PASS. Then the full suite.

```bash
git add src/app/net tests/net/NetInterpolationTests.cpp tests/CMakeLists.txt
git commit -m "Add a snapshot interpolation buffer and eased correction"
```

---

### Task 8: Spawn and despawn replication

**Files:**
- Create: `src/app/net/NetSpawn.hpp`, `src/app/net/NetSpawn.cpp`
- Modify: `src/app/scene/SceneSerializerApply.cpp`
- Test: `tests/net/NetSpawnTests.cpp`

**Interfaces:**
- Consumes: `NetSession`, `NetworkIdentity` (Task 4); `ByteWriter`/`ByteReader` (Task 2).
- Produces:
  - `enum class NetMessage : std::uint8_t { Snapshot = 1, Spawn = 2, Despawn = 3, Rpc = 4, Welcome = 5, ScriptFields = 6 }`
  - `std::vector<std::byte> EncodeSpawn(std::uint32_t netId, ConnectionId owner, std::string_view prefab, glm::vec3 position)`
  - `struct SpawnMessage { std::uint32_t netId; ConnectionId owner; std::string prefab; glm::vec3 position; }`
  - `std::optional<SpawnMessage> DecodeSpawn(ByteReader&)`
  - `std::vector<std::byte> EncodeDespawn(std::uint32_t netId)`
  - `std::optional<std::uint32_t> DecodeDespawn(ByteReader&)`
  - `void AssignScenePlacedNetIds(World&, NetSession&)`

Every reliable-channel packet begins with a `NetMessage` byte so the receive system can dispatch. **`AssignScenePlacedNetIds` must iterate in scene-load order, not ECS order** — see the Global Constraints.

- [ ] **Step 1: Write the failing tests**

Create `tests/net/NetSpawnTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include "net/NetSpawn.hpp"

using namespace aether;

TEST_CASE("A spawn message round-trips")
{
	const std::vector<std::byte> bytes = net::EncodeSpawn(42, 3, "player", {1.f, 2.f, 3.f});

	net::ByteReader r{bytes};
	CHECK(static_cast<net::NetMessage>(r.U8()) == net::NetMessage::Spawn);

	const auto msg = net::DecodeSpawn(r);
	REQUIRE(msg.has_value());
	CHECK(msg->netId == 42);
	CHECK(msg->owner == 3);
	CHECK(msg->prefab == "player");
	CHECK(msg->position.y == doctest::Approx(2.f));
}

TEST_CASE("A despawn message round-trips")
{
	const std::vector<std::byte> bytes = net::EncodeDespawn(7);

	net::ByteReader r{bytes};
	CHECK(static_cast<net::NetMessage>(r.U8()) == net::NetMessage::Despawn);

	const auto netId = net::DecodeDespawn(r);
	REQUIRE(netId.has_value());
	CHECK(*netId == 7);
}

TEST_CASE("A truncated spawn message decodes to nothing rather than garbage")
{
	net::ByteWriter w;
	w.U8(static_cast<std::uint8_t>(net::NetMessage::Spawn));
	w.U32(1); // netId only; the rest is missing
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	CHECK(static_cast<net::NetMessage>(r.U8()) == net::NetMessage::Spawn);
	CHECK_FALSE(net::DecodeSpawn(r).has_value());
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `net/NetSpawn.hpp` not found.

- [ ] **Step 3: Implement the messages**

Create `src/app/net/NetSpawn.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "net/NetSerialize.hpp"
#include "net/NetSession.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	// Every reliable-channel packet starts with one of these so the receive system
	// can dispatch without a second framing layer.
	enum class NetMessage : std::uint8_t
	{
		Snapshot = 1,
		Spawn = 2,
		Despawn = 3,
		Rpc = 4,
		Welcome = 5,
		ScriptFields = 6,
	};

	struct SpawnMessage
	{
		std::uint32_t netId = 0;
		ConnectionId owner = kInvalidConnection;
		std::string prefab;
		glm::vec3 position{0.f};
	};

	[[nodiscard]] std::vector<std::byte> EncodeSpawn(std::uint32_t netId, ConnectionId owner, std::string_view prefab,
	        glm::vec3 position);
	[[nodiscard]] std::optional<SpawnMessage> DecodeSpawn(ByteReader& r);

	[[nodiscard]] std::vector<std::byte> EncodeDespawn(std::uint32_t netId);
	[[nodiscard]] std::optional<std::uint32_t> DecodeDespawn(ByteReader& r);

	// Gives every scene-placed NetworkIdentity a deterministic id. Iterates in
	// SCENE-LOAD order (the order entities appear in the scene file), which is
	// identical on every machine loading the same scene - so host and client agree
	// with no handshake. ECS iteration order is NOT deterministic and must never
	// be used here.
	void AssignScenePlacedNetIds(World& world, NetSession& session);
} // namespace aether::net
```

Create `src/app/net/NetSpawn.cpp`:

```cpp
#include "net/NetSpawn.hpp"

#include <entt/entt.hpp>

#include "net/NetComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	std::vector<std::byte> EncodeSpawn(std::uint32_t netId, ConnectionId owner, std::string_view prefab, glm::vec3 position)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Spawn));
		w.U32(netId);
		w.U32(owner);
		w.Str(prefab);
		w.F32(position.x);
		w.F32(position.y);
		w.F32(position.z);
		return w.Take();
	}

	std::optional<SpawnMessage> DecodeSpawn(ByteReader& r)
	{
		SpawnMessage msg;
		msg.netId = r.U32();
		msg.owner = r.U32();
		msg.prefab = r.Str();
		msg.position.x = r.F32();
		msg.position.y = r.F32();
		msg.position.z = r.F32();
		if (!r.Ok() || msg.netId == 0)
		{
			return std::nullopt;
		}
		return msg;
	}

	std::vector<std::byte> EncodeDespawn(std::uint32_t netId)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Despawn));
		w.U32(netId);
		return w.Take();
	}

	std::optional<std::uint32_t> DecodeDespawn(ByteReader& r)
	{
		const std::uint32_t netId = r.U32();
		if (!r.Ok() || netId == 0)
		{
			return std::nullopt;
		}
		return netId;
	}

	void AssignScenePlacedNetIds(World& world, NetSession& session)
	{
		// SceneNodeComponent::index is the entity's position in the scene file, which
		// is what makes this deterministic across machines. Sorting by it - rather
		// than walking the view - is the whole point.
		std::vector<std::pair<int, Entity>> ordered;
		world.View<NetworkIdentity, SceneNodeComponent>().each(
		        [&](entt::entity ent, NetworkIdentity&, SceneNodeComponent& node)
		        { ordered.emplace_back(node.index, World::FromEntt(ent)); });

		std::sort(ordered.begin(), ordered.end(),
		        [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : a.second.id < b.second.id; });

		for (const auto& [index, entity]: ordered)
		{
			auto* identity = world.TryGet<NetworkIdentity>(entity);
			if (identity == nullptr || identity->netId != 0)
			{
				continue;
			}
			identity->netId = session.AllocateNetId();
			identity->scenePlaced = true;
			session.Bind(identity->netId, entity);
		}
	}
} // namespace aether::net
```

**Before implementing `AssignScenePlacedNetIds`, verify `SceneNodeComponent` has a stable per-scene index field.** Run:

```bash
grep -n "struct SceneNodeComponent" -A 12 src/engine/scene/Components.hpp
```

If it has no index-like member, use the entity creation order recorded during scene load instead: add a `std::vector<Entity>` out-parameter to the scene apply path and pass it here. Report which route you took.

- [ ] **Step 4: Add the TU, run, commit**

Add to `tests/CMakeLists.txt`:

```cmake
    "${CMAKE_SOURCE_DIR}/src/app/net/NetSpawn.cpp"
```

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*spawn message*,*despawn message*"
```

Expected: 3 cases PASS. Then the full suite.

```bash
git add src/app/net tests/net/NetSpawnTests.cpp tests/CMakeLists.txt
git commit -m "Encode replicated spawn and despawn messages"
```

---

### Task 9: Replicated C# script fields

**Files:**
- Create: `managed/AetherCore/NetAttributes.cs`
- Modify: `managed/AetherCore.Interop/ScriptRegistry.cs:60-140`
- Create: `src/app/net/NetScriptFields.hpp`, `src/app/net/NetScriptFields.cpp`
- Modify: `managed/AetherCore/Internal/Native.cs`

**Interfaces:**
- Consumes: `ByteWriter`/`ByteReader` (Task 2), `NetSession` (Task 4), `NetMessage::ScriptFields` (Task 8).
- Produces:
  - C#: `[Replicated]` and `[NetRpc(NetRpcTarget)]` attributes; `enum NetRpcTarget { Server, Client, Multicast }`
  - Managed exports: `aether_net_script_field_count(scriptType)`, `aether_net_script_field_get(entityId, scriptType, index, buf, len)`, `aether_net_script_field_set(entityId, scriptType, index, buf, len)`
  - C++: `std::vector<std::byte> BuildScriptFieldPacket(...)`, `void ApplyScriptFieldPacket(...)`

This is what makes the framework UE5-shaped rather than engine-only: a project replicates its **own** state by marking a field on its own script, exactly as UE5 replicates a `UPROPERTY` on your class.

`ScriptRegistry.BuildProps` (`ScriptRegistry.cs:101`) already builds a typed `Prop[]` per script type with a `FieldInfo` and a `PropertyType`. Replication reuses that table: add a `Replicated` bool to `Prop`, set from the attribute, and expose a get/set-by-replicated-index path.

- [ ] **Step 1: Write the attributes**

Create `managed/AetherCore/NetAttributes.cs`:

```csharp
using System;

namespace AetherCore;

/// <summary>
/// Marks a public script field for network replication. The host writes it, every
/// client reads it — the same shape as marking a C++ component field with
/// AE_FIELD_REP, so a project replicates its own state without adding an engine
/// component. Only the host may meaningfully change a replicated field; a client
/// write is overwritten by the next snapshot.
/// </summary>
[AttributeUsage(AttributeTargets.Field, Inherited = true)]
public sealed class ReplicatedAttribute : Attribute
{
}

/// <summary>Where a <see cref="NetRpcAttribute"/> method runs.</summary>
public enum NetRpcTarget
{
    /// <summary>Called on a client, executed on the host.</summary>
    Server = 0,
    /// <summary>Called on the host, executed on the owning client.</summary>
    Client = 1,
    /// <summary>Called on the host, executed on every client.</summary>
    Multicast = 2,
}

/// <summary>
/// Marks a script method as a remote procedure call. Invoking it locally sends the
/// call to the target instead of (or as well as) running it here.
/// </summary>
[AttributeUsage(AttributeTargets.Method, Inherited = true)]
public sealed class NetRpcAttribute : Attribute
{
    public NetRpcAttribute(NetRpcTarget target) => Target = target;

    public NetRpcTarget Target { get; }
}
```

- [ ] **Step 2: Discover replicated fields in the script registry**

In `managed/AetherCore.Interop/ScriptRegistry.cs`, add to the `Prop` class (~line 60):

```csharp
        /// <summary>Field carries [Replicated]; the network layer syncs it host to client.</summary>
        public bool Replicated;
```

In `BuildProps` (~line 101), when constructing each `Prop`, set it:

```csharp
                props.Add(new Prop
                {
                    Name = field.Name,
                    Type = pt,
                    Field = field,
                    ComponentType = componentType,
                    Replicated = field.IsDefined(typeof(ReplicatedAttribute), inherit: true),
                });
```

Then add a replicated-only view and typed accessors beside the existing property exports. Follow the file's established `[UnmanagedCallersOnly]` + `Utf8` marshalling style exactly:

```csharp
    // The replicated subset of a type's props, in a stable order both ends agree on.
    // Built once per type alongside s_props so the network path never re-reflects.
    private static readonly Dictionary<string, Prop[]> s_replicated = new(StringComparer.Ordinal);
```

Populate `s_replicated[typeName]` in the same place `s_props[typeName]` is populated, filtering on `Replicated`. Export the count, and a get/set pair that serialises one field by its index in that filtered array. Use the same value encoding as `NetSerialize` writes for the equivalent `FieldType` (float → 4 bytes, int → 4 bytes, bool → 1 byte, Vector3 → 3 floats, string → u32 length + bytes) so the C++ side can decode with `ReadFieldValue`.

- [ ] **Step 3: Write the C++ side**

Create `src/app/net/NetScriptFields.hpp` and `.cpp` exposing:

```cpp
	// Host: reads every replicated script field on every relevant entity and packs
	// changed ones. Client: applies them. Layout mirrors the component snapshot:
	// u16 count, then per field u32 netId, u32 scriptTypeHash, u16 fieldIndex, <value>.
	[[nodiscard]] std::vector<std::byte> BuildScriptFieldPacket(World& world, NetSession& session,
	        SnapshotCache& cache, const std::vector<Entity>& relevant);
	void ApplyScriptFieldPacket(World& world, NetSession& session, std::span<const std::byte> packet);
```

The script type is identified by a 32-bit hash of its name rather than an index, because script assemblies reload independently of the C++ binary and an index would silently shift. Use the same hash on both sides; `std::hash<std::string_view>` is not stable across runs, so write an explicit FNV-1a.

- [ ] **Step 4: Test the round-trip**

Add to `tests/net/NetSerializeTests.cpp` a test that the script-field value encoding matches `WriteFieldValue`/`ReadFieldValue` for each supported type, so a change to one encoding cannot silently diverge from the other:

```cpp
TEST_CASE("Script field encoding matches the component field codec")
{
	// The managed side writes values with the same layout WriteFieldValue produces.
	// If these ever diverge, replicated script fields decode as garbage - so pin it.
	net::ByteWriter w;
	reflect::FieldValue v;
	v.type = reflect::FieldType::Vec3;
	v.vec = {1.f, 2.f, 3.f, 0.f};
	net::WriteFieldValue(w, v);
	CHECK(w.Size() == 12); // exactly three floats, no type tag, no padding

	net::ByteWriter s;
	reflect::FieldValue str;
	str.type = reflect::FieldType::String;
	str.str = "abc";
	net::WriteFieldValue(s, str);
	CHECK(s.Size() == 4 + 3); // u32 length + bytes
}
```

- [ ] **Step 5: Build both managed and native, run, commit**

```bash
cmake --build build/ninja-clang --target Editor GameRuntime EngineTests && ./build/ninja-clang/tests/EngineTests.exe
```

```bash
git add managed/AetherCore/NetAttributes.cs managed/AetherCore.Interop/ScriptRegistry.cs managed/AetherCore/Internal/Native.cs src/app/net tests/net/NetSerializeTests.cpp
git commit -m "Replicate script fields marked with the Replicated attribute"
```

---

### Task 10: RPCs

**Files:**
- Create: `src/app/net/NetRpc.hpp`, `src/app/net/NetRpc.cpp`
- Modify: `managed/AetherCore.Interop/ScriptRegistry.cs`, `managed/AetherCore/Internal/Native.cs`
- Test: `tests/net/NetRpcTests.cpp`

**Interfaces:**
- Consumes: `ByteWriter`/`ByteReader` (Task 2), `NetMessage::Rpc` (Task 8).
- Produces:
  - `std::vector<std::byte> EncodeRpc(std::uint32_t netId, std::uint32_t scriptTypeHash, std::uint16_t methodIndex, std::span<const std::byte> args)`
  - `struct RpcMessage { std::uint32_t netId; std::uint32_t scriptTypeHash; std::uint16_t methodIndex; std::vector<std::byte> args; }`
  - `std::optional<RpcMessage> DecodeRpc(ByteReader&)`
  - C#: `Net.CallServer(entity, methodName, params object[] args)` and the dispatch path that invokes a `[NetRpc]` method by index.

- [ ] **Step 1: Write the failing test**

Create `tests/net/NetRpcTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include "net/NetRpc.hpp"

using namespace aether;

TEST_CASE("An RPC round-trips with its argument blob intact")
{
	const std::vector<std::byte> args{std::byte{1}, std::byte{2}, std::byte{3}};
	const std::vector<std::byte> bytes = net::EncodeRpc(11, 0xABCDEF01u, 2, args);

	net::ByteReader r{bytes};
	CHECK(static_cast<net::NetMessage>(r.U8()) == net::NetMessage::Rpc);

	const auto msg = net::DecodeRpc(r);
	REQUIRE(msg.has_value());
	CHECK(msg->netId == 11);
	CHECK(msg->scriptTypeHash == 0xABCDEF01u);
	CHECK(msg->methodIndex == 2);
	REQUIRE(msg->args.size() == 3);
	CHECK(msg->args[2] == std::byte{3});
}

TEST_CASE("An RPC claiming more argument bytes than it carries is rejected")
{
	net::ByteWriter w;
	w.U8(static_cast<std::uint8_t>(net::NetMessage::Rpc));
	w.U32(1);
	w.U32(1);
	w.U16(0);
	w.U32(1000); // claims 1000 argument bytes
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	r.U8();
	CHECK_FALSE(net::DecodeRpc(r).has_value());
}
```

- [ ] **Step 2: Run to verify it fails, then implement**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `net/NetRpc.hpp` not found.

Create `src/app/net/NetRpc.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "net/NetSerialize.hpp"
#include "net/NetSpawn.hpp" // NetMessage

namespace aether::net
{
	struct RpcMessage
	{
		std::uint32_t netId = 0;
		std::uint32_t scriptTypeHash = 0;
		std::uint16_t methodIndex = 0;
		std::vector<std::byte> args;
	};

	[[nodiscard]] std::vector<std::byte> EncodeRpc(std::uint32_t netId, std::uint32_t scriptTypeHash,
	        std::uint16_t methodIndex, std::span<const std::byte> args);
	[[nodiscard]] std::optional<RpcMessage> DecodeRpc(ByteReader& r);

	// Stable across runs and machines, unlike std::hash. The script type is
	// identified by name rather than index because script assemblies reload
	// independently of the C++ binary, and an index would silently shift.
	[[nodiscard]] std::uint32_t ScriptTypeHash(std::string_view name);
} // namespace aether::net
```

Create `src/app/net/NetRpc.cpp`:

```cpp
#include "net/NetRpc.hpp"

namespace aether::net
{
	std::vector<std::byte> EncodeRpc(std::uint32_t netId, std::uint32_t scriptTypeHash, std::uint16_t methodIndex,
	        std::span<const std::byte> args)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Rpc));
		w.U32(netId);
		w.U32(scriptTypeHash);
		w.U16(methodIndex);
		w.U32(static_cast<std::uint32_t>(args.size()));
		w.Bytes(args);
		return w.Take();
	}

	std::optional<RpcMessage> DecodeRpc(ByteReader& r)
	{
		RpcMessage msg;
		msg.netId = r.U32();
		msg.scriptTypeHash = r.U32();
		msg.methodIndex = r.U16();
		const std::uint32_t argBytes = r.U32();
		if (!r.Ok())
		{
			return std::nullopt;
		}
		// Bytes() is bounds-checked, so a hostile length yields an empty vector and
		// a failed reader rather than an over-read.
		msg.args = r.Bytes(argBytes);
		if (!r.Ok() || msg.netId == 0)
		{
			return std::nullopt;
		}
		return msg;
	}

	std::uint32_t ScriptTypeHash(std::string_view name)
	{
		// FNV-1a 32. Must match the managed side's implementation byte for byte.
		std::uint32_t hash = 2166136261u;
		for (const char c: name)
		{
			hash ^= static_cast<std::uint8_t>(c);
			hash *= 16777619u;
		}
		return hash;
	}
}
```

On the managed side, discover `[NetRpc]` methods in `BuildProps`'s sibling pass over `type.GetMethods(BindingFlags.Public | BindingFlags.Instance)`, store them in a per-type array in declaration order (the index is what goes on the wire), and add a dispatch entry point that invokes method `i` on the script instance attached to the entity.

- [ ] **Step 3: Run and commit**

```bash
cmake --build build/ninja-clang --target Editor GameRuntime EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*RPC*"
```

Expected: 2 cases PASS. Then the full suite.

```bash
git add src/app/net managed tests/net/NetRpcTests.cpp tests/CMakeLists.txt
git commit -m "Add remote procedure calls for script methods"
```

---

### Task 11: Network systems and the C# API

**Files:**
- Create: `src/app/net/NetworkSystems.hpp`, `src/app/net/NetworkSystems.cpp`
- Create: `src/app/scripting/interop/NetExports.cpp`
- Create: `managed/AetherCore/Net.cs`
- Modify: `src/app/Application.cpp:265-320`, `managed/AetherCore/Internal/Native.cs`
- Modify: `src/app/scripting/interop/CameraExports.cpp`, `managed/AetherCore/Camera.cs`

**Interfaces:**
- Consumes: everything from Tasks 1-10.
- Produces:
  - `class NetworkReceiveSystem : public System` and `class NetworkSendSystem : public System`
  - C#: `Net.Host(port)`, `Net.Connect(ip, port)`, `Net.Disconnect()`, `Net.IsHost`, `Net.IsClient`, `Net.IsConnected`, `Net.LocalConnectionId`, `Net.Spawn(prefab, position, owner)`, `Net.Despawn(entity)`, `Net.HasAuthority(entity)`, `Net.IsOwner(entity)`, `Net.SetPlayerName(entity, name)`, `Net.GetPlayerName(entity)`, `Net.LastError`
  - C#: `Camera.WorldToScreen(Vector3) -> Vector2`

**System ordering is the whole point of this task.** `NetworkReceiveSystem` is registered **first**, before physics and scripts, so everything downstream sees this frame's authoritative state. `NetworkSendSystem` is registered **last**, after scripts and particles, so the host broadcasts post-simulation state rather than a half-updated world.

- [ ] **Step 1: Write the systems**

Create `src/app/net/NetworkSystems.hpp` / `.cpp`.

`NetworkReceiveSystem::Update(World&, float dt)`:
1. `transport.Poll()`.
2. For each event: `Connected` → host adds the connection, allocates the client's player netId, sends `Welcome` with their connection id, and replays a `Spawn` for every existing networked entity so the joiner sees the world. `Disconnected` → despawn that connection's owned entities, broadcast the despawns, `session.RemoveConnection`. `Data` → dispatch on the leading `NetMessage` byte.
3. Push received transform state into each remote entity's `InterpolationBuffer` rather than writing the transform directly.
4. Apply interpolation: for entities the local connection does **not** own, sample the buffer at `now - interpolationDelaySeconds` and write the transform. For the owned entity, `EaseToward` the authoritative position.

`NetworkSendSystem::Update(World&, float dt)`:
1. Host only: for each connection, compute `RelevantFor`, `BuildSnapshot`, and send on `kChannelSnapshot` (unreliable) if non-empty. Then `BuildScriptFieldPacket` on the same relevant set, sent reliably.
2. Client only: send local input/owned-entity state to the host as an RPC, per the project's own logic — the framework provides the channel, not the schema.

- [ ] **Step 2: Register the systems in the right order**

In `src/app/Application.cpp`, in the block at lines 265-320:

Register `NetworkReceiveSystem` **before** the first existing `RegisterSystem` call (before `AnimationSystem`), with this comment:

```cpp
				// Before everything: inbound authoritative state must land before physics,
				// scripts or animation read it, or every system spends a frame acting on
				// data the host has already superseded.
```

Register `NetworkSendSystem` **after** the last one (after the particle system), with:

```cpp
				// After everything: the host broadcasts post-simulation state, so clients
				// receive the world as it ended the frame rather than mid-update.
```

- [ ] **Step 3: Add Camera.WorldToScreen**

In `src/app/scripting/interop/CameraExports.cpp`, beside `aether_camera_screen_to_world`:

```cpp
AE_SCRIPT_API Vec2 aether_camera_world_to_screen(Vec3 world)
{
	// Inverse of aether_camera_screen_to_world. Returns render-target pixels with a
	// top-left origin, matching Input.MousePosition and the UI's coordinate space,
	// so a caller can hand the result straight to Ui.SetRect.
	// Behind the camera, returns (-1, -1) so callers can cheaply cull.
	...
}
```

Implement it by projecting through the main camera's view-projection, dividing by w, mapping NDC to the render-target size, and returning `{-1.f, -1.f}` when the clip-space w is <= 0. Mirror the existing `screen_to_world` export's camera lookup exactly.

In `managed/AetherCore/Camera.cs`, beside `ScreenToWorld`:

```csharp
    /// <summary>Project a world position to render-target pixels (top-left origin),
    /// matching <see cref="Input.MousePosition"/> and the UI canvas space. Returns
    /// (-1, -1) when the position is behind the camera.</summary>
    public static Vector2 WorldToScreen(Vector3 worldPos) => Native.aether_camera_world_to_screen(worldPos);
```

- [ ] **Step 4: Write Net.cs**

Create `managed/AetherCore/Net.cs` wrapping the exports, following `Ui.cs`'s section-banner and XML-doc style. Every method must be safe to call when offline (returning `false`/`default`) — a project's title screen calls `Net.IsHost` before anything is connected.

- [ ] **Step 5: Build everything and run the suite**

```bash
cmake --build build/ninja-clang --target CompileShaders EngineAssetsPak Editor GameRuntime EngineTests
./build/ninja-clang/tests/EngineTests.exe
```

Expected: all pass. Building `GameRuntime` is the check that no editor-only dependency crept into `NetExports.cpp`.

- [ ] **Step 6: Commit**

```bash
git add src/app/net src/app/scripting/interop src/app/Application.cpp managed
git commit -m "Tick the network systems and expose them to scripts

- Receive before simulation, send after it
- Add the Net script API and Camera.WorldToScreen"
```

---

### Task 12: Loopback integration test

**Files:**
- Modify: `tests/net/NetLoopbackTests.cpp`

**Interfaces:**
- Consumes: everything.
- Produces: no production code — this task's deliverable is proof the layers compose.

- [ ] **Step 1: Write the end-to-end test**

Append to `tests/net/NetLoopbackTests.cpp` a test that stands up two `NetworkSubsystem`s and two `World`s in one process and asserts, without any rendering or scripting:

1. A client connects and the host records the connection.
2. The host spawns a networked entity, and the client's world gains an entity bound to the same net id.
3. Moving the entity on the host changes the client's copy after a send/receive round.
4. Despawning on the host removes it from the client.
5. Disconnecting the client removes its owned entities from the host.

Drive it by calling the encode/apply functions directly around the real transport — do **not** instantiate `NetworkReceiveSystem`/`NetworkSendSystem`, which need a full `World` system context. This test proves the wire format and the session bookkeeping compose; the systems are proven manually in plan 2.

- [ ] **Step 2: Run and commit**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*loopback*,*connects to a host*"
```

Then the full suite:

```bash
./build/ninja-clang/tests/EngineTests.exe
```

```bash
git add tests/net/NetLoopbackTests.cpp
git commit -m "Prove replication end to end over a loopback connection"
```

---

## Verification Summary

```bash
cmake --build build/ninja-clang --target CompileShaders EngineAssetsPak Editor GameRuntime EngineTests && ./build/ninja-clang/tests/EngineTests.exe
```

Do not report any task complete without pasting the actual output of its verification step. The suite must be strictly above its 349-case baseline at every commit.

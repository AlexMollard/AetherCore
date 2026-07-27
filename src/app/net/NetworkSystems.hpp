#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

#include <glm/glm.hpp>

#include "net/NetInterpolation.hpp"
#include "net/NetTypes.hpp"
#include "scene/Entity.hpp"
#include "scene/System.hpp"

namespace aether::net
{
	class NetworkContext;

	// Drains the transport and lands inbound authoritative state on the world.
	//
	// Registered FIRST, ahead of animation, physics and scripts: everything
	// downstream reads transforms and script fields this frame, and a system that
	// runs before the packet lands spends the whole frame simulating state the host
	// has already superseded.
	class NetworkReceiveSystem final : public System
	{
	public:
		explicit NetworkReceiveSystem(NetworkContext& context)
		      : m_context(context)
		{
		}

		[[nodiscard]] const char* GetName() const override
		{
			return "NetworkReceiveSystem";
		}

		void Update(World& world, float dt) override;

		// The three transport-event handlers Update dispatches to. Public because
		// OnData is the framework's inbound trust boundary - the only place a remote
		// peer's bytes are gated by role - and reaching it through Update means
		// standing up a real socket, a peer and a handshake for every case in that
		// matrix. Calling them directly is precisely what Update does; this is a
		// reachable seam, not a separate test path.
		void OnConnected(World& world, ConnectionId peer);
		void OnDisconnected(World& world, ConnectionId peer);
		void OnData(World& world, ConnectionId peer, std::span<const std::byte> data);

	private:
		// Drops bindings whose entity no longer exists. A net id whose entity died by
		// a route replication never sees - a scene load, a script's Entity.Destroy -
		// would otherwise resolve to a dangling handle that an inbound packet hands
		// straight to try_get.
		void PruneDeadBindings(World& world);

		// Snapshot fields are written straight onto TransformComponent by
		// ApplySnapshot, which is right for the authoritative value and wrong for
		// what should be rendered. These two bracket the apply: the first records
		// where every replicated entity was, the second turns the difference into an
		// interpolation sample (remote entities) or an eased correction (the owned,
		// locally predicted one) and writes the result back.
		void CaptureRenderedTransforms(World& world);
		void ResolveTransforms(World& world, float dt);

		NetworkContext& m_context;

		// Last authoritative pose per net id, kept apart from the rendered transform:
		// a snapshot carries only the fields that changed, so reading "the transform"
		// after an apply that touched position but not rotation would sample the
		// interpolated rotation we ourselves wrote last frame.
		struct RemoteState
		{
			InterpolationBuffer buffer;
			glm::vec3 authoritativePosition{0.f};
			glm::vec3 authoritativeEuler{0.f};
		};

		struct Pose
		{
			// The exact pre-apply matrix, kept alongside the decomposed channels so the
			// owned-entity correction can restore rotation/scale bit-exact instead of
			// rebuilding them from a lossy decompose/recompose round trip.
			glm::mat4 matrix{1.f};
			glm::vec3 position{0.f};
			glm::vec3 euler{0.f};
			glm::vec3 scale{1.f};
		};

		std::unordered_map<std::uint32_t, RemoteState> m_remote;
		// Rebuilt every frame; a member only so the per-frame allocation is amortised.
		std::unordered_map<std::uint32_t, Pose> m_renderedBefore;
	};

	// Broadcasts post-simulation state to every connection.
	//
	// Registered LAST, after scripts and particles, so a client receives the world
	// as it ended the frame rather than half-updated: a snapshot built mid-frame
	// would carry positions physics is about to overwrite and script fields the
	// script has not set yet.
	class NetworkSendSystem final : public System
	{
	public:
		explicit NetworkSendSystem(NetworkContext& context)
		      : m_context(context)
		{
		}

		[[nodiscard]] const char* GetName() const override
		{
			return "NetworkSendSystem";
		}

		void Update(World& world, float dt) override;

	private:
		[[nodiscard]] static glm::vec3 ViewerPosition(World& world, ConnectionId viewer);

		NetworkContext& m_context;

		// Wall-clock time of the next send. Wall clock, not accumulated frame dt, so
		// pausing or time-scaling the game does not silently change the send rate.
		float m_nextSendTime = 0.f;
	};
} // namespace aether::net

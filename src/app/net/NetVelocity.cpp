#include "net/NetVelocity.hpp"

#include "net/NetComponents.hpp"
#include "net/NetSession.hpp"
#include "net/NetworkContext.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	std::vector<std::byte> EncodeVelocitySnapshot(const std::vector<VelocityEntry>& entries)
	{
		ByteWriter w;
		w.U16(static_cast<std::uint16_t>(entries.size()));
		for (const VelocityEntry& entry: entries)
		{
			w.U32(entry.netId);
			w.F32(entry.linear.x);
			w.F32(entry.linear.y);
			w.F32(entry.linear.z);
			w.F32(entry.angular.x);
			w.F32(entry.angular.y);
			w.F32(entry.angular.z);
		}
		return w.Take();
	}

	std::optional<std::vector<VelocityEntry>> DecodeVelocitySnapshot(ByteReader& r)
	{
		const std::uint16_t count = r.U16();
		std::vector<VelocityEntry> entries;
		entries.reserve(count);
		for (std::uint16_t i = 0; i < count; ++i)
		{
			VelocityEntry entry;
			entry.netId = r.U32();
			entry.linear.x = r.F32();
			entry.linear.y = r.F32();
			entry.linear.z = r.F32();
			entry.angular.x = r.F32();
			entry.angular.y = r.F32();
			entry.angular.z = r.F32();
			if (!r.Ok())
			{
				return std::nullopt; // truncated mid-entry - the whole packet is unparseable
			}
			if (entry.netId != 0)
			{
				entries.push_back(entry);
			}
		}
		if (!r.Ok())
		{
			return std::nullopt;
		}
		return entries;
	}

	// BANDWIDTH: 28 B/entity (4 net id + 12 linear + 12 angular) plus a 2-byte
	// count header, against roughly nothing today (velocity is not on the wire at
	// all) - this is pure addition, not a replacement of an existing cost. At the
	// framework's 20 Hz default send rate that is ~560 B/s per qualifying entity
	// per connection; twenty thrown props relevant to one connection at once -
	// plausible in this sandbox's stated design - is ~11.2 KB/s. Linear-only
	// would halve that; angular is kept because a spinning thrown object's
	// rotation is exactly the other half of "extrapolates from what the
	// authority actually knows" the position fix alone would leave unaddressed,
	// and the marginal 12 B/entity is small next to the value.
	//
	// SCOPE, read straight off the entity's own components rather than a new
	// authored flag: only a RigidBodyComponent with motionType == Dynamic
	// qualifies - "physics props yes, a slow-moving (Kinematic, or bodyless) door
	// no" is exactly this test, for free, because it is what those two examples
	// already differ on. A CharacterControllerComponent (a player) is
	// deliberately NOT included: its own player is never extrapolated at all
	// (zero added latency is the whole point - see docs/multiplayer.md's latency
	// budget), and another peer's character's turning-in-place rotation is a
	// distinct problem from "a thrown crate tumbles" this task named - reusing
	// this exact wire shape for it is a reasonable future extension, not
	// something folding it in here for free would get right without its own
	// bandwidth and behaviour review.
	std::vector<std::byte> BuildVelocitySnapshot(World& world, const NetworkContext& context,
	        const std::vector<Entity>& candidates)
	{
		std::vector<VelocityEntry> entries;
		PhysicsSystem* physics = nullptr;
		bool physicsResolved = false;
		for (const Entity entity: candidates)
		{
			const auto* identity = world.TryGet<NetworkIdentity>(entity);
			if (identity == nullptr || identity->netId == 0)
			{
				continue;
			}

			VelocityEntry entry;
			entry.netId = identity->netId;

			if (context.OwnsIdentity(*identity))
			{
				// Ground truth: this peer's own Jolt body IS the authority.
				const auto* rigid = world.TryGet<RigidBodyComponent>(entity);
				if (rigid == nullptr || rigid->motionType != PhysicsMotionType::Dynamic || !rigid->body.IsValid())
				{
					continue; // not a physics-driven Dynamic body - nothing authoritative to send
				}
				if (!physicsResolved)
				{
					// Resolved at most once per call, not per entity - mirrors
					// NetworkContext.cpp's own SetMotionType3D/RebuildBody2D pattern.
					physics = static_cast<PhysicsSystem*>(world.FindSystem("PhysicsSystem"));
					physicsResolved = true;
				}
				if (physics == nullptr)
				{
					continue; // headless build, or no 3D physics system registered
				}
				entry.linear = physics->GetLinearVelocity(rigid->body);
				entry.angular = physics->GetAngularVelocity(rigid->body);
			}
			else
			{
				// Relaying someone else's entity (host only, in practice - a client's
				// own candidate list is always everything IT owns). This peer's local
				// copy of that body is frozen Kinematic (SyncSimulationAuthority), so a
				// live query here would report a kinematic body's velocity, not the
				// true owner's - see NetReceivedVelocity's own comment.
				const auto* received = world.TryGet<NetReceivedVelocity>(entity);
				if (received == nullptr)
				{
					continue; // nothing received yet to relay
				}
				entry.linear = received->linear;
				entry.angular = received->angular;
			}

			entries.push_back(entry);
		}
		if (entries.empty())
		{
			return {};
		}
		return EncodeVelocitySnapshot(entries);
	}

	std::vector<VelocityEntry> ApplyVelocitySnapshot(World& world, const NetSession& session,
	        std::span<const std::byte> payload, const StateWriteGate& gate)
	{
		ByteReader reader{payload};
		const std::optional<std::vector<VelocityEntry>> decoded = DecodeVelocitySnapshot(reader);
		if (!decoded.has_value())
		{
			return {};
		}
		std::vector<VelocityEntry> applied;
		applied.reserve(decoded->size());
		for (const VelocityEntry& entry: *decoded)
		{
			const Entity entity = session.EntityFor(entry.netId);
			if (!entity.IsValid() || !gate.Allows(world, entity))
			{
				continue;
			}
			world.EmplaceOrReplace<NetReceivedVelocity>(entity,
			        NetReceivedVelocity{.linear = entry.linear, .angular = entry.angular});
			applied.push_back(entry);
		}
		return applied;
	}
} // namespace aether::net

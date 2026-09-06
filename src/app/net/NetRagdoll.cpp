#include "net/NetRagdoll.hpp"

#include "net/NetComponents.hpp"
#include "net/NetSession.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	std::vector<std::byte> EncodeRagdollPoses(const std::vector<RagdollPoseEntry>& entries)
	{
		ByteWriter w;
		w.U16(static_cast<std::uint16_t>(entries.size()));
		for (const RagdollPoseEntry& entry: entries)
		{
			w.U32(entry.netId);
			w.U8(static_cast<std::uint8_t>(entry.bones.size()));
			for (const RagdollBonePose& bone: entry.bones)
			{
				w.F32(bone.position.x);
				w.F32(bone.position.y);
				w.F32(bone.position.z);
				w.F32(bone.rotation.x);
				w.F32(bone.rotation.y);
				w.F32(bone.rotation.z);
			}
		}
		return w.Take();
	}

	std::optional<std::vector<RagdollPoseEntry>> DecodeRagdollPoses(ByteReader& r)
	{
		const std::uint16_t count = r.U16();
		std::vector<RagdollPoseEntry> entries;
		entries.reserve(count);
		for (std::uint16_t i = 0; i < count; ++i)
		{
			RagdollPoseEntry entry;
			entry.netId = r.U32();
			const std::uint8_t boneCount = r.U8();
			entry.bones.reserve(boneCount);
			for (std::uint8_t b = 0; b < boneCount; ++b)
			{
				RagdollBonePose bone;
				bone.position.x = r.F32();
				bone.position.y = r.F32();
				bone.position.z = r.F32();
				bone.rotation.x = r.F32();
				bone.rotation.y = r.F32();
				bone.rotation.z = r.F32();
				entry.bones.push_back(bone);
			}
			if (!r.Ok())
			{
				return std::nullopt; // truncated mid-entry - the whole packet is unparseable
			}
			if (entry.netId != 0)
			{
				entries.push_back(std::move(entry));
			}
		}
		if (!r.Ok())
		{
			return std::nullopt;
		}
		return entries;
	}

	// BANDWIDTH, and why this is a dedicated message rather than folding bone
	// poses into the generic reflected-field Snapshot: a per-field write costs a
	// FieldKey (net id + component index + field index, several bytes) PLUS the
	// value on top, paid twice per bone (position, rotation) - roughly 32 B/bone
	// once framing is counted. This format pays it once per bone (24 B: two
	// packed vec3s) plus a 5-byte-per-ragdoll header, so an 11-bone ragdoll (10
	// non-root bones) costs ~245 B here against ~320 B generic, and needs no
	// reflection-schema entry to exist at all - which a generic version would
	// have required, and which this feature cannot touch (see NetOwnership.hpp's
	// sibling comments on the same boundary). At the framework's 20 Hz default
	// send rate that is ~4.9 KB/s per relevant ragdoll per connection; four
	// ragdolls relevant to one connection at once - not an unusual sandbox
	// moment - is under 20 KB/s, the number the alternative (leaving bones frozen
	// forever, the gap this file closes) was weighed against.
	//
	// ponytail: sent in full, every relevant tick, with no per-bone change
	// detection or sleep-state gating - a settled, motionless ragdoll still costs
	// the same as a tumbling one. SnapshotCache-style diffing would need a
	// per-(netId, boneIndex) cache exactly like the generic path already keeps
	// per field; upgrade path is that cache, or gating on Jolt's own sleep flag
	// once a query for it exists, whichever lands first.
	std::vector<std::byte> BuildRagdollPoseSnapshot(World& world, const std::vector<Entity>& candidates)
	{
		std::vector<RagdollPoseEntry> entries;
		for (const Entity entity: candidates)
		{
			const auto* identity = world.TryGet<NetworkIdentity>(entity);
			const auto* ragdoll = world.TryGet<RagdollComponent>(entity);
			if (identity == nullptr || identity->netId == 0 || ragdoll == nullptr)
			{
				continue; // not a ragdoll root, or not yet part of the session
			}

			RagdollPoseEntry entry;
			entry.netId = identity->netId;
			entry.bones.reserve(ragdoll->bones.size());
			bool ok = true;
			for (const Entity bone: ragdoll->bones)
			{
				if (bone == entity)
				{
					continue; // the root - already covered by the ordinary Snapshot/NetworkTransform path
				}
				const auto* transform = world.TryGet<TransformComponent>(bone);
				if (transform == nullptr)
				{
					// A bone with no transform is a corrupted ragdoll, not a partial one -
					// dropping just this bone would shift every later index and misapply
					// pose data onto the WRONG limb on the receiving end (see
					// RagdollPoseEntry's own comment on why the order is trusted at all).
					// Drop the whole entry instead; the next tick tries again.
					ok = false;
					break;
				}
				glm::vec3 pos{};
				glm::vec3 euler{};
				glm::vec3 scale{};
				DecomposeTRS(transform->localToWorld, pos, euler, scale);
				entry.bones.push_back(RagdollBonePose{.position = pos, .rotation = euler});
			}
			if (ok && !entry.bones.empty() && entry.bones.size() <= 0xFF)
			{
				entries.push_back(std::move(entry));
			}
		}
		if (entries.empty())
		{
			return {};
		}
		return EncodeRagdollPoses(entries);
	}

	// NO SMOOTHING. A NetworkIdentity-bearing entity gets an InterpolationBuffer
	// keyed by its own net id (NetworkReceiveSystem::m_remote); a ragdoll bone has
	// no net id of its own to key one by, and standing up eleven-per-ragdoll
	// buffers keyed by (root net id, bone index) is real bookkeeping this v1 does
	// not attempt - RagdollBuilder's own comment already named "full per-bone
	// ragdoll replication" as a separate, larger feature, and per-bone smoothing
	// is the other half of that, not this closing of the "frozen forever" gap.
	// Direct-write is consistent with what SyncSimulationAuthority already made
	// every non-root bone: Kinematic, transform-driven, exactly like
	// PushKinematicTargets already expects to push into Jolt every frame -
	// writing the raw received pose onto TransformComponent is precisely that
	// contract, just satisfied from the network instead of from an animation.
	//
	// ponytail: a bone whose pose has not moved still gets the same received
	// value written back on every tick this fires - one matrix write per bone,
	// negligible next to decoding the packet that carried it. If per-bone
	// interpolation is ever added, this is the function that grows an
	// InterpolationBuffer map.
	void ApplyRagdollPoses(World& world, const NetSession& session, std::span<const std::byte> payload,
	        const StateWriteGate& gate)
	{
		ByteReader reader{payload};
		const std::optional<std::vector<RagdollPoseEntry>> entries = DecodeRagdollPoses(reader);
		if (!entries.has_value())
		{
			return;
		}
		for (const RagdollPoseEntry& entry: *entries)
		{
			const Entity root = session.EntityFor(entry.netId);
			if (!root.IsValid() || !gate.Allows(world, root))
			{
				continue;
			}
			const auto* ragdoll = world.TryGet<RagdollComponent>(root);
			if (ragdoll == nullptr)
			{
				continue;
			}
			std::size_t boneIndex = 0;
			for (const Entity bone: ragdoll->bones)
			{
				if (bone == root)
				{
					continue;
				}
				if (boneIndex >= entry.bones.size())
				{
					break; // this peer's ragdoll has more bones than the packet named
				}
				const RagdollBonePose& pose = entry.bones[boneIndex];
				++boneIndex;
				auto* transform = world.TryGet<TransformComponent>(bone);
				if (transform == nullptr)
				{
					continue;
				}
				glm::vec3 unusedPos{};
				glm::vec3 unusedEuler{};
				glm::vec3 scale{};
				DecomposeTRS(transform->localToWorld, unusedPos, unusedEuler, scale);
				transform->localToWorld = ComposeTransform(pose.position, pose.rotation, scale);
			}
		}
	}
} // namespace aether::net

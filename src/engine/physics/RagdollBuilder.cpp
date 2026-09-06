#include "physics/RagdollBuilder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include "assets/GltfAsset.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		// One entry per ragdoll bone. `nameContains` and `endNameContains` are matched
		// case-insensitively against the skeleton's node names (exact match on the part
		// after the last '_'/':' preferred, falling back to substring - see
		// FindNodeByRole) so "mixamorig_LeftForeArm", "LeftForeArm", and
		// "Bip01_L_ForeArm" (assuming a rig used that exact word) all resolve the same
		// way. This is what keeps the mapping data-driven rather than hardcoded against
		// Human.gltf's specific Mixamo names - a rig naming its joints in a wholly
		// different convention (numbered, non-English, ...) is not covered; that needs
		// its own table, not a smarter matcher.
		//
		// `endNameContains` gives the bone its LENGTH: the capsule runs from this bone's
		// own bind-pose position to the first node matching `endNameContains` (normally
		// its child in the chain). Empty means "no natural end" (Pelvis), which falls
		// back to a small fixed-radius sphere instead of a length-derived capsule.
		//
		// `radiusFraction` is radius as a fraction of the derived segment length - the
		// glTF carries joint positions, not surface thickness, so there is no real width
		// to read here; this is a heuristic standing in for that missing data, worth
		// saying plainly rather than letting it read as a measurement.
		//
		// v1.5 seam: driving this from live physics instead of a spawn-time snapshot
		// would need, per bone, exactly the two things resolved here - `nodeIndex` (which
		// node in `skeleton` this bone corresponds to) and each frame's world transform.
		// A GPU node-override buffer (see the animation pipeline: PoseInit -> SampleClips
		// -> NodeFlatten -> BuildSkinPalette, all device-only buffers today) would read a
		// small (nodeIndex, worldTransform) list uploaded once a frame and substitute it
		// for NodeFlatten's own parent*local composition at exactly those nodes; every
		// non-driving child (fingers, toes, ...) still inherits correctly through
		// NodeFlatten's existing recursive walk. Nothing publishes that list today - nothing
		// reads bone transforms on the CPU anywhere in this engine yet - but the pairing
		// this function already computes (node index + world transform, one per driving
		// bone) is exactly its input, so wiring it up later is additive, not a rebuild.
		struct RagdollBoneDef
		{
			std::string_view role;
			std::string_view parentRole;
			std::string_view nameContains;
			std::string_view endNameContains;
			float radiusFraction;
			float minRadius;
			JointType jointToParent;
			float minLimitDeg;
			float maxLimitDeg;
			float swingLimitDeg;
		};

		constexpr std::array<RagdollBoneDef, 11> kMixamoBoneDefs{{
		        {"Pelvis", "", "hips", "", 0.0f, 0.15f, JointType::Fixed, 0.0f, 0.0f, 0.0f},
		        {"Chest", "Pelvis", "spine", "neck", 0.22f, 0.12f, JointType::SwingTwist, -20.0f, 20.0f, 30.0f},
		        {"Head", "Chest", "neck", "headtop", 0.35f, 0.08f, JointType::SwingTwist, -45.0f, 45.0f, 40.0f},
		        {"LeftUpperArm", "Chest", "leftarm", "leftforearm", 0.18f, 0.05f, JointType::SwingTwist, -80.0f, 80.0f, 80.0f},
		        {"LeftForearm", "LeftUpperArm", "leftforearm", "lefthand", 0.16f, 0.04f, JointType::Hinge, 0.0f, 145.0f, 0.0f},
		        {"RightUpperArm", "Chest", "rightarm", "rightforearm", 0.18f, 0.05f, JointType::SwingTwist, -80.0f, 80.0f, 80.0f},
		        {"RightForearm", "RightUpperArm", "rightforearm", "righthand", 0.16f, 0.04f, JointType::Hinge, 0.0f, 145.0f, 0.0f},
		        {"LeftThigh", "Pelvis", "leftupleg", "leftleg", 0.20f, 0.08f, JointType::SwingTwist, -30.0f, 60.0f, 50.0f},
		        {"LeftShin", "LeftThigh", "leftleg", "leftfoot", 0.16f, 0.06f, JointType::Hinge, -140.0f, 0.0f, 0.0f},
		        {"RightThigh", "Pelvis", "rightupleg", "rightleg", 0.20f, 0.08f, JointType::SwingTwist, -30.0f, 60.0f, 50.0f},
		        {"RightShin", "RightThigh", "rightleg", "rightfoot", 0.16f, 0.06f, JointType::Hinge, -140.0f, 0.0f, 0.0f},
		}};

		// A second, wholly independent naming convention, added after a real CC0 rig
		// (Quaternius "Man", see QuaterniusMan.glb) proved kMixamoBoneDefs' own
		// prediction right: this rig's joints are "Torso"/"Abdomen"/"UpperArm.L"/
		// "LowerArm.L"/"Foot.L", nothing here shares a common substring with the
		// Mixamo table above (except "hips"/"neck", which both rigs happen to spell
		// the same). Deliberately a FULL, separately-verified 11-entry table rather
		// than an alias layered onto kMixamoBoneDefs: bridging individual words
		// across conventions (e.g. treating "LowerArm" as a generic synonym for
		// "Forearm") is exactly the heuristic that binds an arm to a leg on some
		// third rig by coincidence. Verified against the actual asset's node/parent
		// table (Bone -> Body -> Hips -> Abdomen -> Torso -> Neck -> Head; Torso ->
		// Shoulder.{L,R} -> UpperArm -> LowerArm -> Palm; Body -> UpperLeg -> LowerLeg;
		// Foot.{L,R} are IK targets parented to the skeleton ROOT, not to LowerLeg -
		// harmless here because FindNodeByRole matches by name across the whole
		// skeleton, never by hierarchy).
		constexpr std::array<RagdollBoneDef, 11> kQuaterniusBoneDefs{{
		        {"Pelvis", "", "hips", "", 0.0f, 0.15f, JointType::Fixed, 0.0f, 0.0f, 0.0f},
		        {"Chest", "Pelvis", "abdomen", "neck", 0.22f, 0.12f, JointType::SwingTwist, -20.0f, 20.0f, 30.0f},
		        {"Head", "Chest", "neck", "head_end", 0.35f, 0.08f, JointType::SwingTwist, -45.0f, 45.0f, 40.0f},
		        {"LeftUpperArm", "Chest", "upperarm.l", "lowerarm.l", 0.18f, 0.05f, JointType::SwingTwist, -80.0f, 80.0f, 80.0f},
		        {"LeftForearm", "LeftUpperArm", "lowerarm.l", "palm.l", 0.16f, 0.04f, JointType::Hinge, 0.0f, 145.0f, 0.0f},
		        {"RightUpperArm", "Chest", "upperarm.r", "lowerarm.r", 0.18f, 0.05f, JointType::SwingTwist, -80.0f, 80.0f, 80.0f},
		        {"RightForearm", "RightUpperArm", "lowerarm.r", "palm.r", 0.16f, 0.04f, JointType::Hinge, 0.0f, 145.0f, 0.0f},
		        {"LeftThigh", "Pelvis", "upperleg.l", "lowerleg.l", 0.20f, 0.08f, JointType::SwingTwist, -30.0f, 60.0f, 50.0f},
		        {"LeftShin", "LeftThigh", "lowerleg.l", "foot.l", 0.16f, 0.06f, JointType::Hinge, -140.0f, 0.0f, 0.0f},
		        {"RightThigh", "Pelvis", "upperleg.r", "lowerleg.r", 0.20f, 0.08f, JointType::SwingTwist, -30.0f, 60.0f, 50.0f},
		        {"RightShin", "RightThigh", "lowerleg.r", "foot.r", 0.16f, 0.06f, JointType::Hinge, -140.0f, 0.0f, 0.0f},
		}};

		struct RagdollNamingConvention
		{
			std::string_view name;
			std::span<const RagdollBoneDef> bones;
		};

		constexpr std::array<RagdollNamingConvention, 2> kRagdollConventions{{
		        {"Mixamo", kMixamoBoneDefs},
		        {"Quaternius", kQuaterniusBoneDefs},
		}};

		bool EqualsCI(std::string_view a, std::string_view b)
		{
			return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
		}

		bool ContainsCI(std::string_view haystack, std::string_view needle)
		{
			const auto it = std::ranges::search(haystack, needle, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
			return !it.empty();
		}

		// Prefers an exact match on the node's own local name (the part after the last
		// '_'/':' separator) over a plain substring match, so "spine" resolves to the
		// CONTAIN "spine") - see kMixamoBoneDefs' own comment.
		//
		// ponytail: an alias containing '_' or ':' (e.g. "head_end") defeats this
		// exact-match branch entirely - the local-name split compares only the part
		// AFTER the last separator ("end"), which never equals the full alias, so
		// the role always falls through to the plain-substring branch below for it.
		// Harmless on every rig checked so far (verified against QuaterniusMan.glb:
		// nothing else in that skeleton contains "head_end" as a substring), but a
		// future rig where such an alias IS a substring of an unrelated bone would
		// bind to the wrong node with no warning, silently, precisely because the
		// guard that exists to prevent exactly that never fires for underscored
		// aliases. Upgrade path: split the ROLE the same way as the node name
		// before comparing, or forbid '_'/':' in a nameContains/endNameContains
		// value and assert it in ConventionMatches.
		std::optional<std::uint32_t> FindNodeByRole(const assets::GltfAsset& skeleton, std::string_view role)
		{
			std::optional<std::uint32_t> substringMatch;
			for (std::size_t i = 0; i < skeleton.nodes.size(); ++i)
			{
				const std::string_view name = skeleton.nodes[i].name;
				if (!ContainsCI(name, role))
				{
					continue;
				}
				if (!substringMatch.has_value())
				{
					substringMatch = static_cast<std::uint32_t>(i);
				}
				const std::size_t sep = name.find_last_of("_:");
				const std::string_view localName = sep == std::string_view::npos ? name : name.substr(sep + 1);
				if (EqualsCI(localName, role))
				{
					return static_cast<std::uint32_t>(i);
				}
			}
			return substringMatch;
		}

		// A convention only counts if EVERY one of its roles' start bone resolves -
		// see kQuaterniusBoneDefs' own comment for why a partial match (some roles
		// under one convention, the rest under another) is refused rather than
		// blended. `missingOut` is always refreshed (even on success, where it comes
		// back empty) so a caller can log it either way.
		bool ConventionMatches(const assets::GltfAsset& skeleton, std::span<const RagdollBoneDef> defs, std::vector<std::string_view>& missingOut)
		{
			missingOut.clear();
			for (const RagdollBoneDef& def: defs)
			{
				if (!FindNodeByRole(skeleton, def.nameContains).has_value())
				{
					missingOut.push_back(def.role);
				}
			}
			return missingOut.empty();
		}

		std::string JoinNames(std::span<const std::string_view> names)
		{
			std::string joined;
			for (const std::string_view name: names)
			{
				if (!joined.empty())
				{
					joined += ", ";
				}
				joined += name;
			}
			return joined;
		}

		glm::mat4 NodeLocalMatrix(const assets::GltfNode& node)
		{
			if (node.hasMatrix)
			{
				return node.matrix;
			}
			return glm::translate(glm::mat4(1.0f), node.translation) * glm::mat4_cast(node.rotation) * glm::scale(glm::mat4(1.0f), node.scale);
		}

		// Bind-pose world transform, relative to the glTF's own root - memoized since
		// many bones share ancestors (every bone here descends from Hips).
		glm::mat4 ComputeBindWorld(const assets::GltfAsset& skeleton, std::uint32_t nodeIndex, std::vector<std::optional<glm::mat4>>& cache)
		{
			if (cache[nodeIndex].has_value())
			{
				return *cache[nodeIndex];
			}
			const assets::GltfNode& node = skeleton.nodes[nodeIndex];
			const glm::mat4 local = NodeLocalMatrix(node);
			const glm::mat4 world = node.parentIndex >= 0 ? ComputeBindWorld(skeleton, static_cast<std::uint32_t>(node.parentIndex), cache) * local : local;
			cache[nodeIndex] = world;
			return world;
		}

		// A hinge (elbow/knee) axis perpendicular to the limb's own direction, in the
		// plane a standard T/A-pose bind assumes it bends through. Degenerates for a
		// segment parallel to `primaryRef` (a vertical leg against world-up), hence the
		// fallback reference - a rig bound in some other rest pose would need this
		// recomputed differently; not solved generically here.
		glm::vec3 ComputeHingeAxis(const glm::vec3& segmentDir)
		{
			constexpr glm::vec3 kPrimaryRef(0.0f, 1.0f, 0.0f);
			constexpr glm::vec3 kFallbackRef(0.0f, 0.0f, 1.0f);
			const glm::vec3 ref = std::abs(glm::dot(segmentDir, kPrimaryRef)) > 0.9f ? kFallbackRef : kPrimaryRef;
			return glm::normalize(glm::cross(segmentDir, ref));
		}
	} // namespace

	bool SpawnRagdoll(World& world, Entity source, const assets::GltfAsset& skeleton)
	{
		const TransformComponent* sourceTc = world.TryGet<TransformComponent>(source);
		if (sourceTc == nullptr || world.Has<RagdollBoneComponent>(source))
		{
			return false;
		}

		// A convention only counts if it resolves EVERY one of its roles' start bone
		// (see ConventionMatches) - a rig matching neither in full is refused outright
		// rather than silently built from whichever partial set of roles happens to
		// resolve. Before this, an unmapped rig (e.g. QuaterniusMan.glb, which shares
		// only "hips"/"neck" with the Mixamo table) still returned true here: Pelvis
		// and Head resolved by coincidence, every limb role's parentRole lookup then
		// cascaded to a skip, and the caller got a "successful" one-bone ragdoll with
		// no warning at all.
		std::span<const RagdollBoneDef> chosenBones;
		std::vector<std::string_view> missing;
		for (const RagdollNamingConvention& convention: kRagdollConventions)
		{
			if (ConventionMatches(skeleton, convention.bones, missing))
			{
				chosenBones = convention.bones;
				break;
			}
		}
		if (chosenBones.empty())
		{
			std::string report;
			for (const RagdollNamingConvention& convention: kRagdollConventions)
			{
				ConventionMatches(skeleton, convention.bones, missing);
				report += std::format("{}: missing [{}]. ", convention.name, JoinNames(missing));
			}
			std::string nodeNames;
			for (const assets::GltfNode& node: skeleton.nodes)
			{
				if (!node.name.empty())
				{
					nodeNames += node.name + " ";
				}
			}
			AE_WARN(LogCategory::Engine, "SpawnRagdoll: skeleton matches no known naming convention - {}actual bone names: {}", report, nodeNames);
			return false;
		}

		std::vector<std::optional<glm::mat4>> bindCache(skeleton.nodes.size());
		const std::optional<std::uint32_t> hipsIndex = FindNodeByRole(skeleton, chosenBones[0].nameContains);
		if (!hipsIndex.has_value())
		{
			return false;
		}

		// Mixamo-family rigs (this engine's only source of humanoid skeletons so far)
		// are exported at wildly different scales depending on the DCC tool's own
		// export units - some in metres, some (observed directly in Human.gltf) in
		// centimetres, with no accompanying scale node to say which. A standing
		// character's hip height is reliably within a few tens of centimetres of 1 m in
		// EITHER convention, so treating a hip bind-pose Y far outside a plausible
		// "metres" range as centimetres and correcting for it is a safe, cheap guard - a
		// rig at some third, still different scale is not handled and would need this
		// redone as an explicit parameter instead of a guess.
		const float hipsBindY = ComputeBindWorld(skeleton, *hipsIndex, bindCache)[3].y;
		const float unitScale = std::abs(hipsBindY) > 3.0f ? 0.01f : 1.0f;

		const glm::vec3 spawnPos(sourceTc->localToWorld[3]);
		glm::mat3 spawnRotBasis(sourceTc->localToWorld);
		spawnRotBasis[0] = glm::normalize(spawnRotBasis[0]);
		spawnRotBasis[1] = glm::normalize(spawnRotBasis[1]);
		spawnRotBasis[2] = glm::normalize(spawnRotBasis[2]);
		const glm::quat spawnRot = glm::normalize(glm::quat_cast(spawnRotBasis));

		glm::vec3 seedVelocity{0.0f};
		if (const auto* cc = world.TryGet<CharacterControllerComponent>(source))
		{
			seedVelocity = cc->velocity;
			world.Remove<CharacterControllerComponent>(source);
		}

		std::unordered_map<std::string_view, Entity> resolved;
		std::vector<Entity> bones;

		for (const RagdollBoneDef& def: chosenBones)
		{
			const std::optional<std::uint32_t> startIndex = FindNodeByRole(skeleton, def.nameContains);
			if (!startIndex.has_value())
			{
				continue;
			}
			Entity parent{};
			if (!def.parentRole.empty())
			{
				const auto it = resolved.find(def.parentRole);
				if (it == resolved.end())
				{
					continue;
				}
				parent = it->second;
			}

			const glm::vec3 startBind(glm::vec3(ComputeBindWorld(skeleton, *startIndex, bindCache)[3]) * unitScale);
			std::optional<std::uint32_t> endIndex;
			if (!def.endNameContains.empty())
			{
				endIndex = FindNodeByRole(skeleton, def.endNameContains);
			}

			PhysicsShapeType shape = PhysicsShapeType::Capsule;
			float radius = def.minRadius;
			float halfHeight = def.minRadius;
			glm::vec3 midpointBind = startBind;
			glm::quat boneRot{1.0f, 0.0f, 0.0f, 0.0f};
			glm::vec3 bindDir(0.0f, 1.0f, 0.0f);

			if (endIndex.has_value())
			{
				const glm::vec3 endBind(glm::vec3(ComputeBindWorld(skeleton, *endIndex, bindCache)[3]) * unitScale);
				const glm::vec3 segment = endBind - startBind;
				const float length = glm::length(segment);
				if (length > 1e-4f)
				{
					bindDir = segment / length;
					radius = std::max(def.minRadius, length * def.radiusFraction);
					halfHeight = std::max(0.01f, length * 0.5f - radius);
					midpointBind = (startBind + endBind) * 0.5f;
					boneRot = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), bindDir);
				}
			}
			else
			{
				shape = PhysicsShapeType::Sphere;
			}

			const glm::vec3 worldPos = spawnPos + spawnRot * midpointBind;
			const glm::quat worldRot = glm::normalize(spawnRot * boneRot);
			const glm::vec3 visualScale = shape == PhysicsShapeType::Sphere ? glm::vec3(radius * 2.0f) : glm::vec3(radius * 2.0f, (halfHeight + radius) * 2.0f, radius * 2.0f);

			// The skin-drive offset: inverse(this bone's own bind-pose body transform)
			// times the skeleton node's own bind-pose world transform, both computed
			// fresh right here so there is no risk of the two disagreeing about "T-pose"
			// later. bodyBindWorld deliberately excludes visualScale - the capsule's
			// display size must never leak into the skinned mesh's joint transform.
			const glm::mat4 bodyBindWorld = glm::translate(glm::mat4(1.0f), worldPos) * glm::mat4_cast(worldRot);
			const glm::mat4 nodeBindWorld = glm::translate(glm::mat4(1.0f), spawnPos) * glm::mat4_cast(spawnRot) * glm::scale(glm::mat4(1.0f), glm::vec3(unitScale)) * ComputeBindWorld(skeleton, *startIndex, bindCache);
			const glm::mat4 skinNodeOffset = glm::inverse(bodyBindWorld) * nodeBindWorld;

			const Entity bone = def.parentRole.empty() ? source : world.Create();
			world.EmplaceOrReplace<TransformComponent>(bone, TransformComponent{.localToWorld = glm::translate(glm::mat4(1.0f), worldPos) * glm::mat4_cast(worldRot) * glm::scale(glm::mat4(1.0f), visualScale)});
			world.EmplaceOrReplace<RigidBodyComponent>(bone, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic, .initialVelocity = def.parentRole.empty() ? seedVelocity : glm::vec3(0.0f)});
			world.EmplaceOrReplace<ColliderComponent>(bone, ColliderComponent{.shape = shape, .radius = radius, .halfHeight = halfHeight});
			world.EmplaceOrReplace<MeshSourceComponent>(bone, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Primitive, .path = shape == PhysicsShapeType::Sphere ? "sphere" : "cube"});
			world.EmplaceOrReplace<MeshRendererComponent>(bone, MeshRendererComponent{});
			world.EmplaceOrReplace<MaterialInstanceComponent>(bone, MaterialInstanceComponent{.asset = MaterialAsset{.baseColorFactor = {0.85f, 0.65f, 0.55f, 1.0f}, .roughnessFactor = 0.7f}});
			world.EmplaceOrReplace<RagdollBoneComponent>(bone,
			        RagdollBoneComponent{
			                .root = source,
			                .boneName = std::string(def.role),
			                .skinNodeName = std::string(skeleton.nodes[*startIndex].name),
			                .skinNodeOffset = skinNodeOffset,
			        });
			if (bone != source)
			{
				// Parented under `source`, not left at root: the bones ARE this ragdoll,
				// not eleven independent top-level entities - a busy Hierarchy panel was
				// the reported symptom, but the parent is semantically correct regardless,
				// the same way a model's mesh primitives are children of its root. Safe to
				// do purely as bookkeeping: SetParent/InsertChildAt (Hierarchy.hpp) never
				// touches TransformComponent, only HierarchyComponent's parent/children
				// links, so a bone's already-computed WORLD transform (set above) is
				// completely unaffected - unlike SetWorldTransform's delta-to-subtree
				// propagation (TransformEdit.hpp), which this call never goes through.
				ecs::SetParent(world, bone, source);
				// A non-root bone is a brand-new entity this call created (source is
				// caller-provided and never created here, so it is deliberately left
				// alone - whether IT should persist is a separate, pre-existing question
				// this fix does not touch). Every bone this loop creates has no
				// scene-authored provenance at all, so a save mid-Play must never bake it
				// in - confirmed live: a duplicated Player + FirstPersonPlayer pair from
				// an unrelated runtime spawn showed the same failure mode. Marked
				// independently of source's own transient status, since parenting under a
				// persistent (e.g. scene-placed) source must not resurrect these into a save.
				world.Emplace<SceneTransientComponent>(bone);
			}

			if (!def.parentRole.empty())
			{
				const glm::vec3 anchorWorld = spawnPos + spawnRot * startBind;
				// collideConnected = false only disables collision between THIS bone and
				// its direct joint parent - PhysicsSystem::FlushPendingJoints' own
				// per-joint-pair mechanism, not a per-ragdoll group filter. That is enough
				// for a T-pose spawn (only anatomically adjacent bones overlap at rest),
				// but it is a known corner once limbs swing far from it: nothing stops a
				// hand or forearm from colliding with the torso or the opposite arm once
				// the ragdoll is mid-tumble, which can read as a small jitter rather than
				// the limb passing cleanly by. A real fix needs a shared per-ragdoll
				// CollisionGroup/GroupFilter (ignore every bone pair, not just adjacent
				// ones), not attempted here.
				// The child bone's own long axis, in WORLD space - the natural twist axis
				// for a Swing Twist shoulder/hip, and the direction ComputeHingeAxis needs
				// to find a perpendicular bend axis for a Hinge elbow/knee.
				const glm::vec3 worldDir = glm::normalize(spawnRot * bindDir);
				const glm::vec3 axisWorld = def.jointToParent == JointType::Hinge ? ComputeHingeAxis(worldDir) : worldDir;
				world.Emplace<JointComponent>(bone,
				        JointComponent{
				                .type = def.jointToParent,
				                .target = parent,
				                .anchor = anchorWorld,
				                .axis = axisWorld,
				                .minLimit = glm::radians(def.minLimitDeg),
				                .maxLimit = glm::radians(def.maxLimitDeg),
				                .swingLimit = glm::radians(def.swingLimitDeg),
				                .collideConnected = false,
				        });
			}

			resolved.emplace(def.role, bone);
			bones.push_back(bone);
		}

		world.EmplaceOrReplace<RagdollComponent>(source, RagdollComponent{.bones = bones});
		return true;
	}
} // namespace aether

#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Color.hpp"
#include "physics/PhysicsDebugData.hpp"
#include "physics/PhysicsComponents.hpp"

namespace aether
{
	class World;
	void SetDebugRenderingEnabled(bool enabled);
	bool IsDebugRenderingEnabled();
	void SetPhysicsDebugShapesEnabled(bool enabled);
	bool IsPhysicsDebugShapesEnabled();
	// Whether the EDITOR's own gizmos - light volumes, physics shapes - stay on screen while
	// the game is playing. Off by default: pressing Play is a request to see the game, and a
	// screen full of light radii and collider outlines is the one thing standing in the way.
	// The toggle exists because watching collisions resolve in motion is a real reason to
	// want them, and that must stay possible.
	void SetEditorGizmosInPlayEnabled(bool enabled);
	bool AreEditorGizmosInPlayEnabled();

	// Collision-only view: scene rendering (meshes, sprites, tiles) is
	// suppressed and every collider wireframe draws - for reading collision
	// against a plain background. Session-only; never persisted.
	void SetCollisionOnlyViewEnabled(bool enabled);
	bool IsCollisionOnlyViewEnabled();

	enum class PhysicsDebugColorMode : uint8_t
	{
		None,
		ByMotionType,
	};

	// supplied vector. Thread-safe by construction: each thread holds its own
	inline constexpr auto kDefaultDebugColor = glm::vec4(colors::DebugYellow);

	void AddDebugLine(std::vector<DebugVertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugAabb(std::vector<DebugVertex>& out, const glm::vec3& min, const glm::vec3& max, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugBox(std::vector<DebugVertex>& out, const glm::vec3& center, const glm::quat& rotation, const glm::vec3& halfExtents, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugSphere(std::vector<DebugVertex>& out, const glm::vec3& center, float radius, const glm::vec4& color = kDefaultDebugColor, int segments = 16);

	void ExtractPhysicsDebugShapes(const World& world, std::vector<PhysicsDebugInstance>& out, PhysicsDebugColorMode colorMode);
	const std::vector<DebugVertex>& GetPhysicsDebugGeometry(PhysicsShapeType shape);
} // namespace aether

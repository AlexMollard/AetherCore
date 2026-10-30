#include "physics/PhysicsDebugShared.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <glm/gtc/matrix_transform.hpp>

#include "scene/World.hpp"
#include "scene/System.hpp"

namespace aether
{
	namespace
	{
		glm::vec4 GetColorForMotionType(const PhysicsMotionType motionType)
		{
			using colors::DebugRed, colors::DebugGreen, colors::DebugBlue, colors::DebugYellow;
			switch (motionType)
			{
				case PhysicsMotionType::Static:
					return DebugRed;
				case PhysicsMotionType::Kinematic:
					return DebugGreen;
				case PhysicsMotionType::Dynamic:
					return DebugBlue;
			}
			return DebugYellow;
		}

		// The debug overlay draws the collider's true simulation size. Entity
		// scale is visual-only in 3D physics (PhysicsStateComponent::scale now
		// carries the authored entity scale), so the dimensions come from the
		// collider itself, not from the interpolated entity transform.
		glm::vec3 ColliderDebugDimensions(const ColliderComponent& c)
		{
			switch (c.shape)
			{
				case PhysicsShapeType::Box:
					return c.halfExtents * 2.0f;
				case PhysicsShapeType::Sphere:
					return glm::vec3(c.radius * 2.0f);
				case PhysicsShapeType::Capsule:
					return glm::vec3(c.radius * 2.0f, c.halfHeight * 2.0f + c.radius * 2.0f, c.radius * 2.0f);
				case PhysicsShapeType::Cylinder:
					return glm::vec3(c.radius * 2.0f, c.halfHeight * 2.0f, c.radius * 2.0f);
				case PhysicsShapeType::ConvexHull:
				case PhysicsShapeType::Mesh:
					// Mesh-sourced shapes have no single box/capsule dimension to report -
					// the fallback below is what an unhandled case already returned.
					break;
			}
			return glm::vec3(1.0f);
		}

		std::vector<DebugVertex> CreateBoxGeometry()
		{
			const std::array<glm::vec3, 24> kBoxEdges = {
			        glm::vec3{-0.5f, -0.5f, -0.5f},
			        glm::vec3{0.5f, -0.5f, -0.5f},
			        glm::vec3{0.5f, -0.5f, -0.5f},
			        glm::vec3{0.5f, -0.5f, 0.5f},
			        glm::vec3{0.5f, -0.5f, 0.5f},
			        glm::vec3{-0.5f, -0.5f, 0.5f},
			        glm::vec3{-0.5f, -0.5f, 0.5f},
			        glm::vec3{-0.5f, -0.5f, -0.5f},
			        glm::vec3{-0.5f, 0.5f, -0.5f},
			        glm::vec3{0.5f, 0.5f, -0.5f},
			        glm::vec3{0.5f, 0.5f, -0.5f},
			        glm::vec3{0.5f, 0.5f, 0.5f},
			        glm::vec3{0.5f, 0.5f, 0.5f},
			        glm::vec3{-0.5f, 0.5f, 0.5f},
			        glm::vec3{-0.5f, 0.5f, 0.5f},
			        glm::vec3{-0.5f, 0.5f, -0.5f},
			        glm::vec3{-0.5f, -0.5f, -0.5f},
			        glm::vec3{-0.5f, 0.5f, -0.5f},
			        glm::vec3{0.5f, -0.5f, -0.5f},
			        glm::vec3{0.5f, 0.5f, -0.5f},
			        glm::vec3{0.5f, -0.5f, 0.5f},
			        glm::vec3{0.5f, 0.5f, 0.5f},
			        glm::vec3{-0.5f, -0.5f, 0.5f},
			        glm::vec3{-0.5f, 0.5f, 0.5f},
			};

			std::vector<DebugVertex> vertices(24);
			constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
			for (std::size_t i = 0; i < kBoxEdges.size(); ++i)
			{
				vertices[i] = DebugVertex{.position = kBoxEdges[i], .color = kWhite};
			}

			return vertices;
		}

		std::vector<DebugVertex> CreateSphereGeometry()
		{
			std::vector<DebugVertex> vertices;
			vertices.reserve(static_cast<size_t>(64 * 3));

			constexpr int kSegments = 16;
			constexpr float kTubeRadius = 0.5f;
			constexpr float kTwoPi = 6.28318530718f;
			constexpr float kPi = std::numbers::pi_v<float>;
			constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};

			for (int i = 0; i < kSegments; ++i)
			{
				const float theta1 = (static_cast<float>(i) * kTwoPi) / kSegments;
				const float theta2 = (static_cast<float>(i + 1) * kTwoPi) / kSegments;

				for (int j = 0; j < kSegments; ++j)
				{
					const float phi1 = (static_cast<float>(j) * kPi) / kSegments;
					const float phi2 = (static_cast<float>(j + 1) * kPi) / kSegments;

					vertices.push_back(DebugVertex{.position = glm::vec3{kTubeRadius * std::sin(phi1) * std::cos(theta1), kTubeRadius * std::cos(phi1), kTubeRadius * std::sin(phi1) * std::sin(theta1)}, .color = kWhite});
					vertices.push_back(DebugVertex{.position = glm::vec3{kTubeRadius * std::sin(phi1) * std::cos(theta2), kTubeRadius * std::cos(phi1), kTubeRadius * std::sin(phi1) * std::sin(theta2)}, .color = kWhite});

					vertices.push_back(DebugVertex{.position = glm::vec3{kTubeRadius * std::sin(phi2) * std::cos(theta1), kTubeRadius * std::cos(phi2), kTubeRadius * std::sin(phi2) * std::sin(theta1)}, .color = kWhite});
					vertices.push_back(DebugVertex{.position = glm::vec3{kTubeRadius * std::sin(phi2) * std::cos(theta2), kTubeRadius * std::cos(phi2), kTubeRadius * std::sin(phi2) * std::sin(theta2)}, .color = kWhite});
				}
			}

			return vertices;
		}

		std::vector<DebugVertex> CreateCapsuleGeometry()
		{
			std::vector<DebugVertex> vertices;
			constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};

			constexpr int kSegments = 12;
			constexpr int kDomeSteps = 3;
			constexpr float kRadius = 0.5f;
			constexpr float kHalfHeight = 0.5f;
			constexpr float kTwoPi = 6.28318530718f;
			constexpr float kHalfPi = 1.57079632679f;
			for (int i = 0; i < kSegments; ++i)
			{
				const float theta = (static_cast<float>(i) * kTwoPi) / kSegments;
				const float dx = std::cos(theta);
				const float dz = std::sin(theta);

				glm::vec3 prev = glm::vec3{0.0f, kHalfHeight + kRadius, 0.0f};

				for (int j = 1; j <= kDomeSteps; ++j)
				{
					const float phi = (static_cast<float>(j) * kHalfPi) / kDomeSteps;
					const glm::vec3 p{kRadius * std::sin(phi) * dx, kHalfHeight + kRadius * std::cos(phi), kRadius * std::sin(phi) * dz};
					vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
					vertices.push_back(DebugVertex{.position = p, .color = kWhite});
					prev = p;
				}

				for (int j = 0; j < kDomeSteps; ++j)
				{
					const float y = kHalfHeight - static_cast<float>(j + 1) * (2.0f * kHalfHeight) / kDomeSteps;
					const glm::vec3 p{kRadius * dx, y, kRadius * dz};
					vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
					vertices.push_back(DebugVertex{.position = p, .color = kWhite});
					prev = p;
				}

				for (int j = 1; j <= kDomeSteps; ++j)
				{
					const float phi = kHalfPi + (static_cast<float>(j) * kHalfPi) / kDomeSteps;
					const glm::vec3 p{kRadius * std::sin(phi) * dx, -kHalfHeight - kRadius * std::cos(phi), kRadius * std::sin(phi) * dz};
					vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
					vertices.push_back(DebugVertex{.position = p, .color = kWhite});
					prev = p;
				}

				const glm::vec3 southPole{0.0f, -kHalfHeight - kRadius, 0.0f};
				vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
				vertices.push_back(DebugVertex{.position = southPole, .color = kWhite});
			}

			for (int i = 0; i < kSegments; ++i)
			{
				const float theta1 = (static_cast<float>(i) * kTwoPi) / kSegments;
				const float theta2 = (static_cast<float>(i + 1) * kTwoPi) / kSegments;
				const float cx1 = kRadius * std::cos(theta1);
				const float cz1 = kRadius * std::sin(theta1);
				const float cx2 = kRadius * std::cos(theta2);
				const float cz2 = kRadius * std::sin(theta2);

				vertices.push_back(DebugVertex{.position = glm::vec3{cx1, kHalfHeight, cz1}, .color = kWhite});
				vertices.push_back(DebugVertex{.position = glm::vec3{cx2, kHalfHeight, cz2}, .color = kWhite});

				vertices.push_back(DebugVertex{.position = glm::vec3{cx1, -kHalfHeight, cz1}, .color = kWhite});
				vertices.push_back(DebugVertex{.position = glm::vec3{cx2, -kHalfHeight, cz2}, .color = kWhite});
			}

			return vertices;
		}

		std::vector<DebugVertex> CreateCylinderGeometry()
		{
			std::vector<DebugVertex> vertices;
			constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
			constexpr int kSegments = 16;
			constexpr float kRadius = 0.5f;
			constexpr float kHalfHeight = 0.5f;
			constexpr float kTwoPi = 6.28318530718f;

			for (int i = 0; i < kSegments; ++i)
			{
				const float theta1 = (static_cast<float>(i) * kTwoPi) / kSegments;
				const float theta2 = (static_cast<float>(i + 1) * kTwoPi) / kSegments;
				const float cx1 = kRadius * std::cos(theta1);
				const float cz1 = kRadius * std::sin(theta1);
				const float cx2 = kRadius * std::cos(theta2);
				const float cz2 = kRadius * std::sin(theta2);

				vertices.push_back(DebugVertex{.position = glm::vec3{cx1, kHalfHeight, cz1}, .color = kWhite});
				vertices.push_back(DebugVertex{.position = glm::vec3{cx2, kHalfHeight, cz2}, .color = kWhite});
				vertices.push_back(DebugVertex{.position = glm::vec3{cx1, -kHalfHeight, cz1}, .color = kWhite});
				vertices.push_back(DebugVertex{.position = glm::vec3{cx2, -kHalfHeight, cz2}, .color = kWhite});
				vertices.push_back(DebugVertex{.position = glm::vec3{cx1, kHalfHeight, cz1}, .color = kWhite});
				vertices.push_back(DebugVertex{.position = glm::vec3{cx1, -kHalfHeight, cz1}, .color = kWhite});
			}

			return vertices;
		}
	} // namespace

	static bool s_debugRenderingEnabled = false;
	static bool s_physicsDebugShapesEnabled = false;

	void SetDebugRenderingEnabled(bool enabled)
	{
		s_debugRenderingEnabled = enabled;
	}

	bool IsDebugRenderingEnabled()
	{
		return s_debugRenderingEnabled;
	}

	void SetPhysicsDebugShapesEnabled(bool enabled)
	{
		s_physicsDebugShapesEnabled = enabled;
	}

	bool IsPhysicsDebugShapesEnabled()
	{
		return s_physicsDebugShapesEnabled;
	}

	static bool s_editorGizmosInPlay = false;

	void SetEditorGizmosInPlayEnabled(bool enabled)
	{
		s_editorGizmosInPlay = enabled;
	}

	bool AreEditorGizmosInPlayEnabled()
	{
		return s_editorGizmosInPlay;
	}

	static bool s_collisionOnlyViewEnabled = false;

	void SetCollisionOnlyViewEnabled(bool enabled)
	{
		s_collisionOnlyViewEnabled = enabled;
	}

	bool IsCollisionOnlyViewEnabled()
	{
		return s_collisionOnlyViewEnabled;
	}

	void AddDebugLine(std::vector<DebugVertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& color)
	{
		out.push_back(DebugVertex{.position = a, .color = color});
		out.push_back(DebugVertex{.position = b, .color = color});
	}

	void AddDebugAabb(std::vector<DebugVertex>& out, const glm::vec3& min, const glm::vec3& max, const glm::vec4& color)
	{
		const glm::vec3 c000{min.x, min.y, min.z};
		const glm::vec3 c100{max.x, min.y, min.z};
		const glm::vec3 c010{min.x, max.y, min.z};
		const glm::vec3 c110{max.x, max.y, min.z};
		const glm::vec3 c001{min.x, min.y, max.z};
		const glm::vec3 c101{max.x, min.y, max.z};
		const glm::vec3 c011{min.x, max.y, max.z};
		const glm::vec3 c111{max.x, max.y, max.z};

		AddDebugLine(out, c000, c100, color);
		AddDebugLine(out, c100, c101, color);
		AddDebugLine(out, c101, c001, color);
		AddDebugLine(out, c001, c000, color);
		AddDebugLine(out, c010, c110, color);
		AddDebugLine(out, c110, c111, color);
		AddDebugLine(out, c111, c011, color);
		AddDebugLine(out, c011, c010, color);
		AddDebugLine(out, c000, c010, color);
		AddDebugLine(out, c100, c110, color);
		AddDebugLine(out, c101, c111, color);
		AddDebugLine(out, c001, c011, color);
	}

	void AddDebugBox(std::vector<DebugVertex>& out, const glm::vec3& center, const glm::quat& rotation, const glm::vec3& halfExtents, const glm::vec4& color)
	{
		const glm::mat3 rot{rotation};
		const std::array<glm::vec3, 8> corners{
		        center + rot * glm::vec3{-halfExtents.x, -halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, -halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, -halfExtents.y, halfExtents.z},
		        center + rot * glm::vec3{-halfExtents.x, -halfExtents.y, halfExtents.z},
		        center + rot * glm::vec3{-halfExtents.x, halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, halfExtents.y, halfExtents.z},
		        center + rot * glm::vec3{-halfExtents.x, halfExtents.y, halfExtents.z},
		};

		AddDebugLine(out, corners[0], corners[1], color);
		AddDebugLine(out, corners[1], corners[2], color);
		AddDebugLine(out, corners[2], corners[3], color);
		AddDebugLine(out, corners[3], corners[0], color);
		AddDebugLine(out, corners[4], corners[5], color);
		AddDebugLine(out, corners[5], corners[6], color);
		AddDebugLine(out, corners[6], corners[7], color);
		AddDebugLine(out, corners[7], corners[4], color);
		AddDebugLine(out, corners[0], corners[4], color);
		AddDebugLine(out, corners[1], corners[5], color);
		AddDebugLine(out, corners[2], corners[6], color);
		AddDebugLine(out, corners[3], corners[7], color);
	}

	void AddDebugSphere(std::vector<DebugVertex>& out, const glm::vec3& center, float radius, const glm::vec4& color, int segments)
	{
		constexpr float kPi = std::numbers::pi_v<float>;
		const float kTwoPi = 2.0f * kPi;
		const int clamped = segments < 4 ? 4 : segments;

		auto addCircle = [&](const glm::vec3& axisA, const glm::vec3& axisB)
		{
			for (int i = 0; i < clamped; ++i)
			{
				const float t1 = static_cast<float>(i) * kTwoPi / static_cast<float>(clamped);
				const float t2 = static_cast<float>(i + 1) * kTwoPi / static_cast<float>(clamped);
				const glm::vec3 p1 = center + radius * (axisA * std::cos(t1) + axisB * std::sin(t1));
				const glm::vec3 p2 = center + radius * (axisA * std::cos(t2) + axisB * std::sin(t2));
				AddDebugLine(out, p1, p2, color);
			}
		};

		addCircle({1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
		addCircle({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
		addCircle({0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
	}

	void ExtractPhysicsDebugShapes(const World& world, std::vector<PhysicsDebugInstance>& out, PhysicsDebugColorMode colorMode)
	{
		if (!s_physicsDebugShapesEnabled && !s_collisionOnlyViewEnabled)
		{
			return;
		}
		world.View<ColliderComponent, PhysicsStateComponent, RigidBodyComponent>().each(
		        [&](entt::entity /*entity*/, const ColliderComponent& shape, const PhysicsStateComponent& state, const RigidBodyComponent& rigid)
		        {
			        const glm::vec4 tint = colorMode == PhysicsDebugColorMode::ByMotionType ? GetColorForMotionType(rigid.motionType) : glm::vec4(colors::DebugYellow);
			        const glm::mat4 model = glm::translate(glm::mat4(1.0f), state.currPosition) * glm::mat4(state.currRotation) * glm::mat4(glm::scale(glm::mat4(1.0f), ColliderDebugDimensions(shape)));
			        out.push_back({.model = model, .tint = tint, .shape = shape.shape});
		        });
	}

	const std::vector<DebugVertex>& GetPhysicsDebugGeometry(PhysicsShapeType shape)
	{
		switch (shape)
		{
			case PhysicsShapeType::Box:
			{
				static const auto vertices = CreateBoxGeometry();
				return vertices;
			}
			case PhysicsShapeType::Sphere:
			{
				static const auto vertices = CreateSphereGeometry();
				return vertices;
			}
			case PhysicsShapeType::Capsule:
			{
				static const auto vertices = CreateCapsuleGeometry();
				return vertices;
			}
			case PhysicsShapeType::Cylinder:
			{
				static const auto vertices = CreateCylinderGeometry();
				return vertices;
			}
			case PhysicsShapeType::ConvexHull:
			case PhysicsShapeType::Mesh:
				break;
		}
		static const std::vector<DebugVertex> empty;
		return empty;
	}
} // namespace aether

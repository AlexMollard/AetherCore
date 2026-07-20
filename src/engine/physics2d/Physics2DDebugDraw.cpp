#include "physics2d/Physics2DDebugDraw.hpp"

#include <cmath>
#include <glm/gtc/constants.hpp>

#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

namespace aether
{
	namespace
	{
		constexpr glm::vec4 kStaticColor{0.30f, 0.60f, 1.00f, 1.0f};    // blue
		constexpr glm::vec4 kKinematicColor{0.30f, 0.95f, 0.95f, 1.0f}; // cyan
		constexpr glm::vec4 kDynamicColor{1.00f, 0.83f, 0.16f, 1.0f};   // DebugYellow
		constexpr glm::vec4 kTriggerColor{0.25f, 1.00f, 0.35f, 1.0f};   // green

		glm::vec4 ColorFor(const RigidBody2DComponent* rigid, const Collider2DComponent& collider)
		{
			if (collider.isTrigger)
			{
				return kTriggerColor;
			}
			if (rigid == nullptr)
			{
				return kStaticColor;
			}
			switch (rigid->bodyType)
			{
				case Body2DType::Static:
					return kStaticColor;
				case Body2DType::Kinematic:
					return kKinematicColor;
				case Body2DType::Dynamic:
				default:
					return kDynamicColor;
			}
		}

		// Local (pre-rotation) collider point -> world, on the body's XY plane.
		glm::vec3 ToWorld(glm::vec2 local, glm::vec2 origin, float cosA, float sinA, float z)
		{
			return {origin.x + local.x * cosA - local.y * sinA, origin.y + local.x * sinA + local.y * cosA, z};
		}

		void AddLoop(std::vector<DebugVertex>& out, const glm::vec3* points, std::size_t count, const glm::vec4& color)
		{
			for (std::size_t i = 0; i < count; ++i)
			{
				AddDebugLine(out, points[i], points[(i + 1) % count], color);
			}
		}

		void AddArc(std::vector<DebugVertex>& out, glm::vec2 center, float radius, float startAngle, float sweep, int segments, glm::vec2 origin, float cosA, float sinA, float z, const glm::vec4& color)
		{
			glm::vec3 prev = ToWorld(center + radius * glm::vec2{std::cos(startAngle), std::sin(startAngle)}, origin, cosA, sinA, z);
			for (int i = 1; i <= segments; ++i)
			{
				const float a = startAngle + sweep * static_cast<float>(i) / static_cast<float>(segments);
				const glm::vec3 next = ToWorld(center + radius * glm::vec2{std::cos(a), std::sin(a)}, origin, cosA, sinA, z);
				AddDebugLine(out, prev, next, color);
				prev = next;
			}
		}
	} // namespace

	void ExtractPhysics2DDebugLines(const World& world, std::vector<DebugVertex>& out)
	{
		// Tile collision (chain outlines + rect-run boxes) lives in internal
		// per-chunk bodies, not on entities - pull it from the system.
		if (const auto* tileSystem = static_cast<const Physics2DSystem*>(const_cast<World&>(world).FindSystem("Physics2DSystem")))
		{
			constexpr glm::vec4 kTileColor{0.35f, 0.9f, 0.5f, 1.0f};
			constexpr glm::vec4 kOneWayColor{0.30f, 0.70f, 1.0f, 1.0f}; // cyan: one-way platforms/doors
			const auto solidNormal = [](TileOneWay dir) -> glm::vec2
			{
				switch (dir)
				{
					case TileOneWay::Up:
						return {0.0f, 1.0f};
					case TileOneWay::Down:
						return {0.0f, -1.0f};
					case TileOneWay::Left:
						return {-1.0f, 0.0f};
					case TileOneWay::Right:
						return {1.0f, 0.0f};
					default:
						return {0.0f, 0.0f};
				}
			};
			tileSystem->ForEachTileDebugOutline(
			        [&](const std::vector<glm::vec2>& outline, TileOneWay oneWay)
			        {
				        const glm::vec4 color = oneWay == TileOneWay::None ? kTileColor : kOneWayColor;
				        for (std::size_t i = 1; i < outline.size(); ++i)
				        {
					        AddDebugLine(out, {outline[i - 1].x, outline[i - 1].y, 0.01f}, {outline[i].x, outline[i].y, 0.01f}, color);
				        }
				        // One-way boxes get outward ticks along their SOLID edge (the side
				        // that blocks), so you can read direction at a glance. Pick the box
				        // edge whose outward normal best matches the solid direction - works
				        // even for a rotated tilemap.
				        if (oneWay == TileOneWay::None || outline.size() < 4)
				        {
					        return;
				        }
				        const glm::vec2 n = solidNormal(oneWay);
				        glm::vec2 centre{0.0f};
				        for (int k = 0; k < 4; ++k)
				        {
					        centre += outline[static_cast<std::size_t>(k)];
				        }
				        centre *= 0.25f;
				        int bestEdge = 0;
				        float bestDot = -2.0f;
				        for (int k = 0; k < 4; ++k)
				        {
					        const glm::vec2 a = outline[static_cast<std::size_t>(k)];
					        const glm::vec2 b = outline[static_cast<std::size_t>((k + 1) % 4)];
					        const glm::vec2 mid = (a + b) * 0.5f;
					        const glm::vec2 outward = mid - centre;
					        const float len = glm::length(outward);
					        const float d = len > 1e-5f ? glm::dot(outward / len, n) : -2.0f;
					        if (d > bestDot)
					        {
						        bestDot = d;
						        bestEdge = k;
					        }
				        }
				        const glm::vec2 ea = outline[static_cast<std::size_t>(bestEdge)];
				        const glm::vec2 eb = outline[static_cast<std::size_t>((bestEdge + 1) % 4)];
				        constexpr int kTicks = 4;
				        constexpr float kTickLen = 0.14f;
				        for (int t = 1; t <= kTicks; ++t)
				        {
					        const float f = static_cast<float>(t) / static_cast<float>(kTicks + 1);
					        const glm::vec2 p = ea + (eb - ea) * f;
					        const glm::vec2 q = p + n * kTickLen;
					        AddDebugLine(out, {p.x, p.y, 0.01f}, {q.x, q.y, 0.01f}, color);
				        }
			        });
		}

		for (const auto& [enttEntity, collider, transform]: world.View<Collider2DComponent, TransformComponent>().each())
		{
			glm::vec3 pos{};
			glm::vec3 eulerDeg{};
			glm::vec3 scale{};
			DecomposeTRS(transform.localToWorld, pos, eulerDeg, scale);

			const float angle = glm::radians(eulerDeg.z);
			const float cosA = std::cos(angle);
			const float sinA = std::sin(angle);
			const glm::vec2 origin{pos.x, pos.y};
			// Slightly in front of the body depth so lines never z-fight sprites.
			const float z = pos.z + 0.01f;
			const glm::vec2 s{std::max(std::abs(scale.x), 0.001f), std::max(std::abs(scale.y), 0.001f)};
			const glm::vec2 center = collider.offset * s;

			const auto* rigid = world.TryGet<RigidBody2DComponent>(World::FromEntt(enttEntity));
			const glm::vec4 color = ColorFor(rigid, collider);

			switch (collider.shape)
			{
				case Collider2DShape::Box:
				{
					const glm::vec2 half{0.5f * collider.size.x * s.x, 0.5f * collider.size.y * s.y};
					const glm::vec3 corners[4] = {
					        ToWorld(center + glm::vec2{-half.x, -half.y}, origin, cosA, sinA, z),
					        ToWorld(center + glm::vec2{half.x, -half.y}, origin, cosA, sinA, z),
					        ToWorld(center + glm::vec2{half.x, half.y}, origin, cosA, sinA, z),
					        ToWorld(center + glm::vec2{-half.x, half.y}, origin, cosA, sinA, z),
					};
					AddLoop(out, corners, 4, color);
					break;
				}
				case Collider2DShape::Circle:
				{
					const float radius = std::max(collider.radius * std::max(s.x, s.y), 0.001f);
					AddArc(out, center, radius, 0.0f, glm::two_pi<float>(), 24, origin, cosA, sinA, z, color);
					// Radius spoke makes rotation visible.
					AddDebugLine(out, ToWorld(center, origin, cosA, sinA, z), ToWorld(center + glm::vec2{radius, 0.0f}, origin, cosA, sinA, z), color);
					break;
				}
				case Collider2DShape::Capsule:
				{
					const float radius = std::max(collider.radius * s.x, 0.001f);
					const float half = std::max(0.5f * collider.capsuleHeight * s.y - radius, 0.001f);
					AddArc(out, center + glm::vec2{0.0f, half}, radius, 0.0f, glm::pi<float>(), 12, origin, cosA, sinA, z, color);
					AddArc(out, center - glm::vec2{0.0f, half}, radius, glm::pi<float>(), glm::pi<float>(), 12, origin, cosA, sinA, z, color);
					AddDebugLine(out, ToWorld(center + glm::vec2{-radius, -half}, origin, cosA, sinA, z), ToWorld(center + glm::vec2{-radius, half}, origin, cosA, sinA, z), color);
					AddDebugLine(out, ToWorld(center + glm::vec2{radius, -half}, origin, cosA, sinA, z), ToWorld(center + glm::vec2{radius, half}, origin, cosA, sinA, z), color);
					break;
				}
				case Collider2DShape::Polygon:
				{
					if (collider.points.size() < 2)
					{
						break;
					}
					glm::vec3 prev = ToWorld(center + collider.points.back() * s, origin, cosA, sinA, z);
					for (const glm::vec2& p: collider.points)
					{
						const glm::vec3 next = ToWorld(center + p * s, origin, cosA, sinA, z);
						AddDebugLine(out, prev, next, color);
						prev = next;
					}
					break;
				}
			}
		}
	}
} // namespace aether

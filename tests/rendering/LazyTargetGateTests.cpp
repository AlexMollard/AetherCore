// Which render targets a session is allowed to hold, decided from what the frame
// actually drew.
//
// The gates in here arm and disarm ~480 MB of shadow atlas, cascade depth, VSM blur
// scratch memory. Two failure modes matter, and they are not symmetric:
//
//   * Refusing to allocate when the content IS there produces a scene that silently
//     renders without shadows. That is the bug the "gate on content, never on
//     SceneFeatureFlags" rule exists to prevent, so those cases are pinned hardest.
//   * Allocating when the content is NOT there just wastes memory, which is the state
//     this whole change is undoing.
//
// The signal names are deliberately about submitted geometry and extracted lights.
// There is no scene-kind or feature-flag input here and there must never be one: every
// scene in this engine carries every feature flag, so a flag cannot tell you whether a
// scene contains a shadow caster.
#include <doctest/doctest.h>

#include "rendering/LazyTargetGates.hpp"

using namespace aether;

namespace
{
	// Content a 2D scene produces: sprites and tiles never reach the mesh render queues,
	// so both draw counts are zero. Note the shadow-flagged point lights - 2D lighting
	// reuses the same PointLight component, and INKBOUND's levels really do author them
	// with shadow = true. A gate that looked only at the lights would allocate the whole
	// atlas for a scene that cannot put one texel in it.
	[[nodiscard]] RenderContentSignals Scene2D()
	{
		return RenderContentSignals{
		        .shadowCasterDraws = false,
		        .directionalLight = true,
		        .localShadowLights = true,
		};
	}

	// A lit 3D scene with shadow-casting geometry and shadow-casting local lights.
	[[nodiscard]] RenderContentSignals Scene3D()
	{
		return RenderContentSignals{
		        .shadowCasterDraws = true,
		        .directionalLight = true,
		        .localShadowLights = true,
		};
	}

	// Drives `frames` frames of the same content through the gates and commits whenever
	// the gates ask for it, the way the frame loop does.
	void Run(LazyTargetGates& gates, const RenderContentSignals& signals, const std::uint32_t frames)
	{
		for (std::uint32_t i = 0; i < frames; ++i)
		{
			if (gates.Publish(signals))
			{
				(void) gates.Commit();
			}
		}
	}
} // namespace

TEST_CASE("Lazy targets start released")
{
	const LazyTargetGates gates;
	CHECK_FALSE(gates.DirectionalShadowTargets());
	CHECK_FALSE(gates.LocalShadowTargets());
}

TEST_CASE("A scene with no shadow casters allocates no shadow targets")
{
	LazyTargetGates gates;
	Run(gates, Scene2D(), 300);

	CHECK_FALSE(gates.DirectionalShadowTargets());
	CHECK_FALSE(gates.LocalShadowTargets());
}

TEST_CASE("A scene with shadow casters allocates shadow targets")
{
	LazyTargetGates gates;
	Run(gates, Scene3D(), 1);

	CHECK(gates.DirectionalShadowTargets());
	CHECK(gates.LocalShadowTargets());
}

TEST_CASE("Shadow content arriving allocates on the very next commit")
{
	LazyTargetGates gates;
	Run(gates, Scene2D(), 200);
	REQUIRE_FALSE(gates.DirectionalShadowTargets());
	REQUIRE_FALSE(gates.LocalShadowTargets());

	// One frame of content, one commit. Growing must never wait: a frame that renders
	// with the shadow maps missing is a visible artefact, and the release hysteresis
	// must not be allowed to apply in this direction.
	CHECK(gates.Publish(Scene3D()));
	CHECK(gates.Commit());
	CHECK(gates.DirectionalShadowTargets());
	CHECK(gates.LocalShadowTargets());
}

TEST_CASE("The 2D to 3D to 2D round trip allocates and then releases")
{
	LazyTargetGates gates;

	Run(gates, Scene2D(), 5);
	CHECK_FALSE(gates.LocalShadowTargets());

	Run(gates, Scene3D(), 5);
	CHECK(gates.DirectionalShadowTargets());
	CHECK(gates.LocalShadowTargets());

	Run(gates, Scene2D(), LazyTargetGates::kReleaseFrames + 2);
	CHECK_FALSE(gates.DirectionalShadowTargets());
	CHECK_FALSE(gates.LocalShadowTargets());
}

TEST_CASE("Targets are held through a brief content gap instead of thrashing")
{
	// Deliberately NOT expressed in terms of kReleaseFrames: a test that derives its gap
	// from the constant it is pinning passes for any constant, including one so short the
	// policy is per-frame alloc/free. These numbers are the real-world gaps the policy
	// exists to absorb.
	//
	// A scene load, a Play/Stop round trip or a script that despawns the level for a beat
	// empties the world for a fraction of a second. Releasing there costs a GPU wait-idle
	// and a full render graph rebuild each way, twice, for nothing.
	constexpr std::uint32_t kSceneLoadGapFrames = 30;  // half a second at 60 fps
	constexpr std::uint32_t kPlayStopGapFrames = 60;   // a whole second at 60 fps
	static_assert(LazyTargetGates::kReleaseFrames > kPlayStopGapFrames, "The release policy must outlast a Play/Stop round trip, or it thrashes a half-gigabyte rebuild every time the world is briefly empty.");

	LazyTargetGates gates;
	Run(gates, Scene3D(), 2);
	REQUIRE(gates.LocalShadowTargets());

	Run(gates, Scene2D(), kSceneLoadGapFrames);
	CHECK(gates.DirectionalShadowTargets());
	CHECK(gates.LocalShadowTargets());

	Run(gates, Scene3D(), 1);
	REQUIRE(gates.LocalShadowTargets());

	// The idle count restarts once the content comes back, so a session that flickers
	// every second never reaches the release threshold at all.
	Run(gates, Scene2D(), kPlayStopGapFrames);
	CHECK(gates.LocalShadowTargets());
	Run(gates, Scene3D(), 1);
	Run(gates, Scene2D(), kPlayStopGapFrames);
	CHECK(gates.LocalShadowTargets());
	CHECK(gates.DirectionalShadowTargets());
}

TEST_CASE("Shadow-casting lights alone do not allocate the atlas")
{
	// The INKBOUND case: a 2D level whose point lights are authored with shadow = true.
	// The lights are real, the shadow-casting geometry is not, and the 128 MB atlas plus
	// its 320 MB of depth and blur scratch would be allocated for nothing.
	LazyTargetGates gates;
	Run(gates,
	        RenderContentSignals{
	                .shadowCasterDraws = false,
	                .directionalLight = false,
	                .localShadowLights = true,
	        },
	        10);

	CHECK_FALSE(gates.LocalShadowTargets());
}

TEST_CASE("Shadow-casting geometry alone does not allocate the atlas")
{
	// The mirror case: meshes that would cast, but no local light authored to cast from.
	LazyTargetGates gates;
	Run(gates,
	        RenderContentSignals{
	                .shadowCasterDraws = true,
	                .directionalLight = false,
	                .localShadowLights = false,
	        },
	        10);

	CHECK_FALSE(gates.LocalShadowTargets());
	CHECK_FALSE(gates.DirectionalShadowTargets());
}

TEST_CASE("Directional and local shadow targets are gated independently")
{
	// A sunlit 3D scene whose local lights do not cast. The cascades are needed; the
	// atlas and its blur scratch - the larger half of the bill - are not.
	LazyTargetGates gates;
	Run(gates,
	        RenderContentSignals{
	                .shadowCasterDraws = true,
	                .directionalLight = true,
	                .localShadowLights = false,
	        },
	        10);

	CHECK(gates.DirectionalShadowTargets());
	CHECK_FALSE(gates.LocalShadowTargets());
}

TEST_CASE("A sunless scene with casters allocates no cascades")
{
	LazyTargetGates gates;
	Run(gates,
	        RenderContentSignals{
	                .shadowCasterDraws = true,
	                .directionalLight = false,
	                .localShadowLights = true,
	        },
	        10);

	CHECK_FALSE(gates.DirectionalShadowTargets());
	CHECK(gates.LocalShadowTargets());
}

TEST_CASE("Commit reports whether anything moved")
{
	LazyTargetGates gates;

	// Content that changes nothing must not report a change: the caller turns a true
	// here into a GPU wait-idle and a full render graph rebuild.
	CHECK_FALSE(gates.Publish(Scene2D()));
	CHECK_FALSE(gates.Commit());

	CHECK(gates.Publish(Scene3D()));
	CHECK(gates.Commit());

	CHECK_FALSE(gates.Publish(Scene3D()));
	CHECK_FALSE(gates.Commit());
}

TEST_CASE("A commit with no pending request leaves the gates alone")
{
	LazyTargetGates gates;
	Run(gates, Scene3D(), 2);
	REQUIRE(gates.LocalShadowTargets());

	// Commit rides a shared pending flag with the scene-viewport rebuild, so it can be
	// called on a frame that these gates did not ask for. It must not disturb them.
	CHECK_FALSE(gates.Commit());
	CHECK(gates.DirectionalShadowTargets());
	CHECK(gates.LocalShadowTargets());
}

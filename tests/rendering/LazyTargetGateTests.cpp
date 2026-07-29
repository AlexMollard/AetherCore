// Which render targets a session is allowed to hold, decided from what the frame
// actually drew.
//
// The gates in here arm and disarm ~480 MB of shadow atlas, cascade depth, VSM blur
// scratch, AO and preview memory. Two failure modes matter, and they are not symmetric:
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
		        .sceneDraws = false,
		        .shadowCasterDraws = false,
		        .directionalLight = true,
		        .localShadowLights = true,
		        .texturePreview = false,
		        .skinnedDraws = false,
		};
	}

	// A lit 3D scene with shadow-casting geometry and shadow-casting local lights, all of
	// it static: no skeleton reaches a render queue.
	[[nodiscard]] RenderContentSignals Scene3D()
	{
		return RenderContentSignals{
		        .sceneDraws = true,
		        .shadowCasterDraws = true,
		        .directionalLight = true,
		        .localShadowLights = true,
		        .texturePreview = false,
		        .skinnedDraws = false,
		};
	}

	// The same scene with an animated character in it, i.e. a draw carrying an animation
	// database. This is what arms the 402 MB of skin palette and pose pools.
	[[nodiscard]] RenderContentSignals Scene3DSkinned()
	{
		RenderContentSignals signals = Scene3D();
		signals.skinnedDraws = true;
		return signals;
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
	CHECK_FALSE(gates.GtaoTargets());
	CHECK_FALSE(gates.TexturePreviewTarget());
	CHECK_FALSE(gates.SkinningBuffers());
}

TEST_CASE("A scene with no shadow casters allocates no shadow targets")
{
	LazyTargetGates gates;
	Run(gates, Scene2D(), 300);

	CHECK_FALSE(gates.DirectionalShadowTargets());
	CHECK_FALSE(gates.LocalShadowTargets());
	CHECK_FALSE(gates.GtaoTargets());
}

TEST_CASE("A scene with shadow casters allocates shadow targets")
{
	LazyTargetGates gates;
	Run(gates, Scene3D(), 1);

	CHECK(gates.DirectionalShadowTargets());
	CHECK(gates.LocalShadowTargets());
	CHECK(gates.GtaoTargets());
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
	CHECK(gates.GtaoTargets());
}

TEST_CASE("The 2D to 3D to 2D round trip allocates and then releases")
{
	LazyTargetGates gates;

	Run(gates, Scene2D(), 5);
	CHECK_FALSE(gates.LocalShadowTargets());

	Run(gates, Scene3D(), 5);
	CHECK(gates.DirectionalShadowTargets());
	CHECK(gates.LocalShadowTargets());
	CHECK(gates.GtaoTargets());

	Run(gates, Scene2D(), LazyTargetGates::kReleaseFrames + 2);
	CHECK_FALSE(gates.DirectionalShadowTargets());
	CHECK_FALSE(gates.LocalShadowTargets());
	CHECK_FALSE(gates.GtaoTargets());
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
	CHECK(gates.GtaoTargets());

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
	                .sceneDraws = false,
	                .shadowCasterDraws = false,
	                .directionalLight = false,
	                .localShadowLights = true,
	                .texturePreview = false,
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
	                .sceneDraws = true,
	                .shadowCasterDraws = true,
	                .directionalLight = false,
	                .localShadowLights = false,
	                .texturePreview = false,
	        },
	        10);

	CHECK_FALSE(gates.LocalShadowTargets());
	CHECK_FALSE(gates.DirectionalShadowTargets());
	// The AO targets follow the scene draws, which are there.
	CHECK(gates.GtaoTargets());
}

TEST_CASE("Directional and local shadow targets are gated independently")
{
	// A sunlit 3D scene whose local lights do not cast. The cascades are needed; the
	// atlas and its blur scratch - the larger half of the bill - are not.
	LazyTargetGates gates;
	Run(gates,
	        RenderContentSignals{
	                .sceneDraws = true,
	                .shadowCasterDraws = true,
	                .directionalLight = true,
	                .localShadowLights = false,
	                .texturePreview = false,
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
	                .sceneDraws = true,
	                .shadowCasterDraws = true,
	                .directionalLight = false,
	                .localShadowLights = true,
	                .texturePreview = false,
	        },
	        10);

	CHECK_FALSE(gates.DirectionalShadowTargets());
	CHECK(gates.LocalShadowTargets());
}

TEST_CASE("The texture preview target follows the preview request")
{
	LazyTargetGates gates;
	Run(gates, Scene2D(), 5);
	CHECK_FALSE(gates.TexturePreviewTarget());

	RenderContentSignals previewing = Scene2D();
	previewing.texturePreview = true;
	CHECK(gates.Publish(previewing));
	CHECK(gates.Commit());
	CHECK(gates.TexturePreviewTarget());

	Run(gates, Scene2D(), LazyTargetGates::kReleaseFrames + 2);
	CHECK_FALSE(gates.TexturePreviewTarget());
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

// The skinning pools - skin palette, sampled node poses, node global transforms, all
// triple-buffered - are the single largest lazily-allocated block in the engine at
// 402 MB across the render queues. Unlike the shadow targets they are created on the
// render thread the moment a queue is handed a skinned draw, so this gate exists purely
// to authorise the release. That asymmetry is what the next few cases pin.
//
// The frame counts below are deliberately written as literals rather than derived from
// LazyTargetGates::kReleaseFrames. A test that says "wait kReleaseFrames + 2 frames and
// expect a release" passes for every possible value of kReleaseFrames, including 1 -
// which is the per-frame alloc/free thrash this policy exists to prevent. These numbers
// are wall-clock intent at 60 fps, and they must fail if the constant moves.

TEST_CASE("A scene with no skinned draws never arms the skinning pools")
{
	// A fully populated 3D scene - meshes, sun, shadow-casting local lights - whose
	// geometry is all static. The shadow and AO targets are needed; 402 MB of skinning
	// pools are not, and nothing about a 3D scene on its own may imply them.
	LazyTargetGates gates;
	Run(gates, Scene3D(), 300);

	CHECK(gates.DirectionalShadowTargets());
	CHECK(gates.GtaoTargets());
	CHECK_FALSE(gates.SkinningBuffers());
}

TEST_CASE("A skinned draw arms the skinning gate on the very next commit")
{
	LazyTargetGates gates;
	Run(gates, Scene3D(), 200);
	REQUIRE_FALSE(gates.SkinningBuffers());

	// One frame of skinned content, one commit. The release hysteresis must never apply
	// in this direction: a queue that draws a skinned mesh without its skin palette
	// renders the mesh in bind pose or worse, which is a correctness bug rather than a
	// dropped frame.
	CHECK(gates.Publish(Scene3DSkinned()));
	CHECK(gates.Commit());
	CHECK(gates.SkinningBuffers());
}

TEST_CASE("The skinning pools are released once skinned draws stay away")
{
	LazyTargetGates gates;
	Run(gates, Scene3DSkinned(), 5);
	REQUIRE(gates.SkinningBuffers());

	// Two seconds at 60 fps with no skeleton submitted: the session has moved on, and
	// 402 MB should not still be held for it. A literal, so shortening or lengthening
	// kReleaseFrames has to be a deliberate act that shows up here.
	Run(gates, Scene3D(), 122);
	CHECK_FALSE(gates.SkinningBuffers());

	// The rest of the frame's content is untouched by the skinning gate moving.
	CHECK(gates.DirectionalShadowTargets());
	CHECK(gates.GtaoTargets());
}

TEST_CASE("The skinning pools survive a scene load and a Play/Stop round trip")
{
	// Releasing these costs a GPU wait-idle, a render graph rebuild and a re-upload of
	// every pose pool. Paying that twice because the world was empty for half a second
	// while a scene loaded is worse than holding the memory.
	constexpr std::uint32_t kSceneLoadGapFrames = 30; // half a second at 60 fps
	constexpr std::uint32_t kPlayStopGapFrames = 60;  // a whole second at 60 fps

	LazyTargetGates gates;
	Run(gates, Scene3DSkinned(), 2);
	REQUIRE(gates.SkinningBuffers());

	Run(gates, Scene3D(), kSceneLoadGapFrames);
	CHECK(gates.SkinningBuffers());

	// The idle count restarts the moment a skeleton comes back, so a session that
	// flickers every second never reaches the release threshold at all.
	Run(gates, Scene3DSkinned(), 1);
	Run(gates, Scene3D(), kPlayStopGapFrames);
	CHECK(gates.SkinningBuffers());
	Run(gates, Scene3DSkinned(), 1);
	Run(gates, Scene3D(), kPlayStopGapFrames);
	CHECK(gates.SkinningBuffers());
}

TEST_CASE("The skinning pools are re-armed instantly after a release")
{
	// The release/realloc round trip: this is the cycle a user drives by opening a
	// character scene, switching to a static one for a while, and switching back. The
	// second acquisition must be as immediate as the first.
	LazyTargetGates gates;
	Run(gates, Scene3DSkinned(), 5);
	REQUIRE(gates.SkinningBuffers());

	Run(gates, Scene3D(), 122);
	REQUIRE_FALSE(gates.SkinningBuffers());

	CHECK(gates.Publish(Scene3DSkinned()));
	CHECK(gates.Commit());
	CHECK(gates.SkinningBuffers());

	// And the cycle repeats rather than latching either way.
	Run(gates, Scene3D(), 122);
	CHECK_FALSE(gates.SkinningBuffers());
}

TEST_CASE("The skinning gate is independent of the shadow and AO gates")
{
	// A skinned mesh with no sun and no shadow-casting light: an animated character in a
	// scene lit only by ambient. The pools are needed, the half-gigabyte of shadow
	// targets is not.
	LazyTargetGates gates;
	Run(gates,
	        RenderContentSignals{
	                .sceneDraws = true,
	                .shadowCasterDraws = false,
	                .directionalLight = false,
	                .localShadowLights = false,
	                .texturePreview = false,
	                .skinnedDraws = true,
	        },
	        10);

	CHECK(gates.SkinningBuffers());
	CHECK(gates.GtaoTargets());
	CHECK_FALSE(gates.DirectionalShadowTargets());
	CHECK_FALSE(gates.LocalShadowTargets());
}

TEST_CASE("A skinning gate move is reported as a change so the caller rebuilds")
{
	// Commit's return value is what turns into a GPU wait-idle and a graph rebuild. A
	// skinning release that failed to report would leave the 402 MB allocated forever.
	LazyTargetGates gates;
	Run(gates, Scene3D(), 3);

	CHECK(gates.Publish(Scene3DSkinned()));
	CHECK(gates.Commit());

	// Steady state must not keep asking for rebuilds.
	CHECK_FALSE(gates.Publish(Scene3DSkinned()));
	CHECK_FALSE(gates.Commit());

	// 119 idle frames is one short of the threshold: still nothing to do.
	for (std::uint32_t i = 0; i < 119; ++i)
	{
		CHECK_FALSE(gates.Publish(Scene3D()));
	}
	CHECK(gates.SkinningBuffers());

	// The 120th asks, and the commit reports the release.
	CHECK(gates.Publish(Scene3D()));
	CHECK(gates.Commit());
	CHECK_FALSE(gates.SkinningBuffers());
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
	CHECK(gates.GtaoTargets());
}

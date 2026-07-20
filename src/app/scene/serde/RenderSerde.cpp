// Custom scene serde for the simple renderer components. Sprite Renderer and Sprite
// Animator are reflected but carry an atlas sprite-id resolved against the atlas asset
// (and a paired frame canonicalization), so they capture/apply as whole components
// here rather than through the generic reflected-field path. Mesh Renderer is a small
// flag component. None need load deps; they just cannot go through the plain field path.

#include "scene/SceneComponentSerde.hpp"

#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::app::scene
{
	namespace
	{
		void CaptureSprite(SceneCaptureContext& c)
		{
			if (const auto* s = c.world.TryGet<SpriteRendererComponent>(c.entity))
			{
				c.rec.sprite = *s;
			}
		}

		void ApplySprite(SceneApplyContext& c)
		{
			if (c.rec.sprite)
			{
				c.world.EmplaceOrReplace<SpriteRendererComponent>(c.entity, *c.rec.sprite);
			}
		}

		AE_SCENE_SERDE(Sprite, "Sprite Renderer", 5, CaptureSprite, ApplySprite)

		void CaptureSpriteAnimator(SceneCaptureContext& c)
		{
			if (const auto* a = c.world.TryGet<SpriteAnimatorComponent>(c.entity))
			{
				c.rec.spriteAnimator = *a;
			}
		}

		void ApplySpriteAnimator(SceneApplyContext& c)
		{
			if (c.rec.spriteAnimator)
			{
				c.world.EmplaceOrReplace<SpriteAnimatorComponent>(c.entity, *c.rec.spriteAnimator);
			}
		}

		AE_SCENE_SERDE(SpriteAnimator, "Sprite Animator", 6, CaptureSpriteAnimator, ApplySpriteAnimator)

		void CaptureMeshRenderer(SceneCaptureContext& c)
		{
			if (const auto* mr = c.world.TryGet<MeshRendererComponent>(c.entity))
			{
				c.rec.meshRenderer = true;
				c.rec.meshRendererVisible = mr->visible;
				c.rec.meshRendererCastShadows = mr->castShadows;
			}
		}

		void ApplyMeshRenderer(SceneApplyContext& c)
		{
			if (c.rec.meshRenderer)
			{
				c.world.EmplaceOrReplace<MeshRendererComponent>(c.entity, MeshRendererComponent{.visible = c.rec.meshRendererVisible, .castShadows = c.rec.meshRendererCastShadows});
			}
		}

		AE_SCENE_SERDE(MeshRenderer, "Mesh Renderer", 8, CaptureMeshRenderer, ApplyMeshRenderer)
	} // namespace
} // namespace aether::app::scene

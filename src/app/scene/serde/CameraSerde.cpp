// Custom scene serde for Camera: mostly reflected, but the main-camera state is a
// separate MainCameraComponent tag (World access, not a component field), so it stays
// bespoke and lives here rather than in the capture/apply loops.

#include "scene/SceneComponentSerde.hpp"

#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::app::scene
{
	namespace
	{
		void CaptureCamera(SceneCaptureContext& c)
		{
			if (const auto* cam = c.world.TryGet<CameraComponent>(c.entity))
			{
				c.rec.camera = *cam;
				c.rec.mainCamera = c.world.Has<MainCameraComponent>(c.entity);
			}
		}

		void ApplyCamera(SceneApplyContext& c)
		{
			if (!c.rec.camera)
			{
				return;
			}
			c.world.Emplace<CameraComponent>(c.entity, *c.rec.camera);
			if (c.rec.mainCamera)
			{
				ecs::SetMainCameraEntity(c.world, c.entity);
			}
		}

		AE_SCENE_SERDE(Camera, "Camera", 70, CaptureCamera, ApplyCamera)
	} // namespace
} // namespace aether::app::scene

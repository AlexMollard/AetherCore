#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "camera/CameraManager.hpp"
#include "scene/System.hpp"

namespace aether
{
	// Mirrors entity cameras (CameraComponent + Transform) into the CameraManager
	// pool every frame: each camera entity gets a backing Manual camera whose pose
	// and projection track the entity, so the renderer, gizmos and the "look
	// through camera" preview can all read a live Camera. Backing cameras are
	// destroyed when their entity loses the component or is deleted. Runs with the
	// registered systems while Playing; the app calls it explicitly (dt = 0) while
	// Editing so camera edits and gizmo drags preview live in the frozen scene -
	// the same pattern as LightSystem.
	class CameraSystem final : public System
	{
	public:
		explicit CameraSystem(CameraManager& cameras)
		      : m_cameras(cameras)
		{
		}

		[[nodiscard]] const char* GetName() const override
		{
			return "CameraSystem";
		}

		void Update(World& world, float dt) override;

		// Backing camera of the entity currently tagged MainCameraComponent, or an
		// invalid handle. Refreshed each Update; the editor viewport reads it to
		// drive the game view from the chosen scene camera.
		[[nodiscard]] CameraHandle GetMainCameraBacking() const
		{
			return m_mainBacking;
		}

		// Whether Update() promotes the scene's MainCameraComponent entity to the
		// active render camera. True in the shipped runtime and while Playing, so the
		// scene camera drives the view. The editor sets it FALSE while Editing so the
		// free-look editor camera owns the viewport - otherwise the scene camera would
		// reclaim the view every frame and fight the editor camera. Backing cameras
		// are still mirrored either way (gizmos + look-through preview stay live).
		void SetApplyMainCamera(bool apply)
		{
			m_applyMainCamera = apply;
		}

	private:
		CameraManager& m_cameras;
		bool m_applyMainCamera = true;
		// entity id -> backing camera handle, so backings can be torn down when the
		// owning entity or component goes away (the component itself is gone by then).
		std::unordered_map<std::uint32_t, CameraHandle> m_backing;
		std::unordered_set<std::uint32_t> m_seenScratch;
		CameraHandle m_mainBacking{};
	};
} // namespace aether

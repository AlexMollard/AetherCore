#pragma once

#include <cstdint>
#include <unordered_map>

#include <glm/glm.hpp>

#include "Camera.hpp"

namespace meow
{
	class Input;

	struct CameraHandle
	{
		uint32_t id = 0;
		[[nodiscard]] bool IsValid() const { return id != 0; }
		[[nodiscard]] bool operator==(const CameraHandle&) const = default;
	};

	// Manages a pool of Camera objects.  Maintains one designated "main" camera
	// whose view/projection is used by MeowCore each frame to build FrameConstants.
	//
	// Camera updates (input processing) run automatically via MeowCore::Tick().
	// Cameras in CameraMode::Manual are never updated automatically.
	class CameraManager
	{
	public:
		// ── Camera lifecycle ──────────────────────────────────────────────────
		[[nodiscard]] CameraHandle Create(const CameraDesc& desc = {});
		void Destroy(CameraHandle handle);

		// ── Camera access ─────────────────────────────────────────────────────
		[[nodiscard]] Camera&       Get(CameraHandle handle);
		[[nodiscard]] const Camera& Get(CameraHandle handle) const;

		// Returns nullptr if the handle is invalid or not found.
		[[nodiscard]] Camera*       TryGet(CameraHandle handle);
		[[nodiscard]] const Camera* TryGet(CameraHandle handle) const;

		// ── Main camera ───────────────────────────────────────────────────────
		void SetMainCamera(CameraHandle handle);
		[[nodiscard]] CameraHandle GetMainCamera()  const { return m_mainCamera; }
		[[nodiscard]] bool         HasMainCamera()  const { return m_mainCamera.IsValid(); }

		[[nodiscard]] Camera*       TryGetMainCamera();
		[[nodiscard]] const Camera* TryGetMainCamera() const;

		// ── Convenience matrix getters ────────────────────────────────────────
		// All return identity when no main camera is set.
		[[nodiscard]] glm::mat4 GetMainViewProjection(float aspect) const;
		[[nodiscard]] glm::mat4 GetMainView()                        const;
		[[nodiscard]] glm::mat4 GetMainProjection(float aspect)      const;

		// ── Per-frame update ──────────────────────────────────────────────────
		// Advances all non-Manual cameras by dt seconds.
		// Called by MeowCore::Tick(); not typically called directly from app code.
		void Update(const Input& input, float dt);

	private:
		uint32_t                             m_nextId = 1;
		std::unordered_map<uint32_t, Camera> m_cameras;
		CameraHandle                         m_mainCamera{};
	};
}

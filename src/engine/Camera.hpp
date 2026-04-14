#pragma once

#include <glm/glm.hpp>

namespace aether
{
	class Input;

	enum class CameraMode
	{
		Free,   // WASD + right-mouse-drag to look (Unreal/Unity editor style)
		Orbit,  // Left-mouse-drag to orbit a target; scroll wheel to zoom
		Manual, // No automatic input processing — caller sets pose each frame
	};

	// Initial configuration for a Camera.
	struct CameraDesc
	{
		CameraMode mode = CameraMode::Orbit;
		float fovDegrees = 60.0f;
		float nearPlane = 0.1f;
		float farPlane = 1000.0f;

		// ── Free mode starting pose ───────────────────────────────────────────
		glm::vec3 position = { 0.0f, 3.0f, 7.0f };
		float     yaw = 0.0f;   // degrees, rotation around world Y axis
		float     pitch = -20.0f; // degrees, up/down tilt (±89 clamped)

		// ── Orbit mode starting pose ──────────────────────────────────────────
		glm::vec3 orbitTarget = { 0.0f, 0.0f, 0.0f };
		float     orbitDistance = 7.0f;
		float     orbitYaw = 0.0f;   // degrees around target's Y axis
		float     orbitPitch = 20.0f;  // degrees above/below horizon

		// ── Control speeds ────────────────────────────────────────────────────
		float moveSpeed = 5.0f;  // world units / second (Free)
		float lookSpeed = 0.15f; // degrees per pixel delta (Free RMB drag)
		float orbitSpeed = 0.3f;  // degrees per pixel delta (Orbit LMB drag)
		float zoomSpeed = 0.5f;  // world units per scroll tick (Orbit)
	};

	// Controllable camera supporting Free (FPS) and Orbit modes.
	// Call Update() once per frame — done automatically by CameraManager.
	class Camera
	{
	public:
		explicit Camera(const CameraDesc& desc = {});

		// ── View / projection matrices ────────────────────────────────────────
		[[nodiscard]] glm::mat4 GetViewMatrix() const;
		[[nodiscard]] glm::mat4 GetProjectionMatrix(float aspect) const;
		[[nodiscard]] glm::mat4 GetViewProjectionMatrix(float aspect) const;

		// ── Free mode pose ────────────────────────────────────────────────────
		[[nodiscard]] glm::vec3 GetPosition() const { return m_position; }
		[[nodiscard]] float     GetYaw()      const { return m_yaw; }
		[[nodiscard]] float     GetPitch()    const { return m_pitch; }
		[[nodiscard]] glm::vec3 GetForward()  const;
		[[nodiscard]] glm::vec3 GetRight()    const;
		[[nodiscard]] glm::vec3 GetUp()       const;

		void SetPosition(glm::vec3 pos) { m_position = pos; }
		void SetYawPitch(float yaw, float pitch);

		// ── Orbit mode pose ───────────────────────────────────────────────────
		[[nodiscard]] glm::vec3 GetOrbitTarget()   const { return m_orbitTarget; }
		[[nodiscard]] float     GetOrbitDistance() const { return m_orbitDistance; }
		[[nodiscard]] float     GetOrbitYaw()      const { return m_orbitYaw; }
		[[nodiscard]] float     GetOrbitPitch()    const { return m_orbitPitch; }

		void SetOrbitTarget(glm::vec3 target) { m_orbitTarget = target; }
		void SetOrbitDistance(float dist);
		void SetOrbitYawPitch(float yaw, float pitch);

		// ── Projection params ─────────────────────────────────────────────────
		[[nodiscard]] float GetFovDegrees() const { return m_fovDeg; }
		[[nodiscard]] float GetNearPlane()  const { return m_near; }
		[[nodiscard]] float GetFarPlane()   const { return m_far; }

		void SetFovDegrees(float fov) { m_fovDeg = fov; }
		void SetNearPlane(float n) { m_near = n; }
		void SetFarPlane(float f) { m_far = f; }

		// ── Mode ──────────────────────────────────────────────────────────────
		[[nodiscard]] CameraMode GetMode() const { return m_mode; }
		void SetMode(CameraMode mode) { m_mode = mode; }

		// ── Speeds ────────────────────────────────────────────────────────────
		[[nodiscard]] float GetMoveSpeed()  const { return m_moveSpeed; }
		[[nodiscard]] float GetLookSpeed()  const { return m_lookSpeed; }
		[[nodiscard]] float GetOrbitSpeed() const { return m_orbitSpeed; }
		[[nodiscard]] float GetZoomSpeed()  const { return m_zoomSpeed; }

		void SetMoveSpeed(float s) { m_moveSpeed = s; }
		void SetLookSpeed(float s) { m_lookSpeed = s; }
		void SetOrbitSpeed(float s) { m_orbitSpeed = s; }
		void SetZoomSpeed(float s) { m_zoomSpeed = s; }

		// Called once per frame by CameraManager::Update. No-ops for Manual cameras.
		void Update(const Input& input, float dt);

	private:
		CameraMode m_mode;

		// Free camera pose
		glm::vec3 m_position;
		float     m_yaw;    // degrees, Y-axis rotation
		float     m_pitch;  // degrees, up/down

		// Orbit camera pose
		glm::vec3 m_orbitTarget;
		float     m_orbitDistance;
		float     m_orbitYaw;
		float     m_orbitPitch;

		// Projection
		float m_fovDeg;
		float m_near;
		float m_far;

		// Control speeds
		float m_moveSpeed;
		float m_lookSpeed;
		float m_orbitSpeed;
		float m_zoomSpeed;
	};
}

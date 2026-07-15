#pragma once

#include <glm/glm.hpp>

#include "camera/CameraProjection.hpp"

namespace aether
{
	class Input;

	enum class CameraMode
	{
		Free,
		Orbit,
		Manual,
	};

	struct CameraDesc
	{
		CameraMode mode = CameraMode::Orbit;
		CameraProjection projection = CameraProjection::Perspective;
		float fovDegrees = 60.0f;
		float orthographicHeight = 10.0f;
		float nearPlane = 0.1f;
		float farPlane = 1000.0f;

		glm::vec3 position = {0.0f, 3.0f, 7.0f};
		float yaw = 0.0f;
		float pitch = -20.0f;

		glm::vec3 orbitTarget = {0.0f, 0.0f, 0.0f};
		float orbitDistance = 7.0f;
		float orbitYaw = 0.0f;
		float orbitPitch = 20.0f;

		float moveSpeed = 5.0f;
		float lookSpeed = 0.15f;
		float orbitSpeed = 0.3f;
		float zoomSpeed = 0.5f;
	};

	class Camera
	{
	public:
		explicit Camera(const CameraDesc& desc = {});

		[[nodiscard]] glm::mat4 GetViewMatrix() const;
		[[nodiscard]] glm::mat4 GetProjectionMatrix(float aspect) const;
		[[nodiscard]] glm::mat4 GetViewProjectionMatrix(float aspect) const;

		[[nodiscard]] glm::vec3 GetPosition() const;

		[[nodiscard]] glm::vec3 GetForward() const;
		[[nodiscard]] glm::vec3 GetRight() const;

		void SetPosition(glm::vec3 pos)
		{
			m_position = pos;
		}

		void SetYawPitch(float yaw, float pitch);

		void FocusOn(glm::vec3 target, float distance);

		[[nodiscard]] float GetFocusDistance() const
		{
			return m_focusDistance;
		}

		[[nodiscard]] glm::vec3 GetOrbitTarget() const
		{
			return m_orbitTarget;
		}

		[[nodiscard]] float GetOrbitDistance() const
		{
			return m_orbitDistance;
		}

		[[nodiscard]] float GetOrbitYaw() const
		{
			return m_orbitYaw;
		}

		void SetOrbitTarget(glm::vec3 target)
		{
			m_orbitTarget = target;
		}

		void SetOrbitDistance(float dist);
		void SetOrbitYawPitch(float yaw, float pitch);

		void SetPerspective(float fovDegrees, float nearPlane, float farPlane)
		{
			m_projection = CameraProjection::Perspective;
			m_fovDeg = fovDegrees;
			m_near = nearPlane;
			m_far = farPlane;
		}

		void SetOrthographic(float height, float nearPlane, float farPlane)
		{
			m_projection = CameraProjection::Orthographic;
			m_orthographicHeight = glm::max(0.001f, height);
			m_near = nearPlane;
			m_far = farPlane;
		}

		[[nodiscard]] CameraProjection GetProjection() const
		{
			return m_projection;
		}

		[[nodiscard]] float GetFovDegrees() const
		{
			return m_fovDeg;
		}

		[[nodiscard]] float GetOrthographicHeight() const
		{
			return m_orthographicHeight;
		}

		[[nodiscard]] float GetNearPlane() const
		{
			return m_near;
		}

		[[nodiscard]] float GetFarPlane() const
		{
			return m_far;
		}

		void SetMode(CameraMode mode)
		{
			m_mode = mode;
		}

		void Update(const Input& input, float dt);

	private:
		CameraMode m_mode;

		glm::vec3 m_position;
		float m_yaw;
		float m_pitch;

		float m_focusDistance = 10.0f;

		glm::vec3 m_orbitTarget;
		float m_orbitDistance;
		float m_orbitYaw;
		float m_orbitPitch;

		CameraProjection m_projection;
		float m_fovDeg;
		float m_orthographicHeight;
		float m_near;
		float m_far;

		float m_moveSpeed;
		float m_lookSpeed;
		float m_orbitSpeed;
		float m_zoomSpeed;
	};
} // namespace aether

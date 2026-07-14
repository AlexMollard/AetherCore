#include "camera/Camera.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

#include "platform/Input.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	static constexpr float kPitchLimit = 89.0f;

	Camera::Camera(const CameraDesc& desc)
	      : m_mode(desc.mode),
	        m_position(desc.position),
	        m_yaw(desc.yaw),
	        m_pitch(desc.pitch),
	        m_focusDistance(std::max(0.2f, desc.orbitDistance)),
	        m_orbitTarget(desc.orbitTarget),
	        m_orbitDistance(desc.orbitDistance),
	        m_orbitYaw(desc.orbitYaw),
	        m_orbitPitch(desc.orbitPitch),
	        m_fovDeg(desc.fovDegrees),
	        m_near(desc.nearPlane),
	        m_far(desc.farPlane),
	        m_moveSpeed(desc.moveSpeed),
	        m_lookSpeed(desc.lookSpeed),
	        m_orbitSpeed(desc.orbitSpeed),
	        m_zoomSpeed(desc.zoomSpeed)
	{
	}

	glm::vec3 Camera::GetPosition() const
	{
		if (m_mode == CameraMode::Orbit)
		{
			const float oy = glm::radians(m_orbitYaw);
			const float op = glm::radians(m_orbitPitch);
			const glm::vec3 offset = {
			        m_orbitDistance * std::cos(op) * std::sin(oy),
			        m_orbitDistance * std::sin(op),
			        m_orbitDistance * std::cos(op) * std::cos(oy),
			};
			return m_orbitTarget + offset;
		}

		return m_position;
	}

	glm::vec3 Camera::GetForward() const
	{
		const float yr = glm::radians(m_yaw);
		const float pr = glm::radians(m_pitch);
		return glm::normalize(glm::vec3{
		        -std::sin(yr) * std::cos(pr),
		        std::sin(pr),
		        -std::cos(yr) * std::cos(pr),
		});
	}

	glm::vec3 Camera::GetRight() const
	{
		return glm::normalize(glm::cross(GetForward(), glm::vec3{0.0f, 1.0f, 0.0f}));
	}

	void Camera::SetYawPitch(float yaw, float pitch)
	{
		m_yaw = yaw;
		m_pitch = std::clamp(pitch, -kPitchLimit, kPitchLimit);
	}

	void Camera::SetOrbitDistance(float dist)
	{
		m_orbitDistance = std::max(0.1f, dist);
	}

	void Camera::SetOrbitYawPitch(float yaw, float pitch)
	{
		m_orbitYaw = yaw;
		m_orbitPitch = std::clamp(pitch, -kPitchLimit, kPitchLimit);
	}

	void Camera::FocusOn(glm::vec3 target, float distance)
	{
		// pivot, and remember the distance so orbit + dolly work around it.
		m_focusDistance = std::max(0.2f, distance);
		m_position = target - GetForward() * m_focusDistance;
	}

	glm::mat4 Camera::GetViewMatrix() const
	{
		if (m_mode == CameraMode::Orbit)
		{
			const float oy = glm::radians(m_orbitYaw);
			const float op = glm::radians(m_orbitPitch);
			const glm::vec3 offset = {
			        m_orbitDistance * std::cos(op) * std::sin(oy),
			        m_orbitDistance * std::sin(op),
			        m_orbitDistance * std::cos(op) * std::cos(oy),
			};
			const glm::vec3 eye = m_orbitTarget + offset;
			return glm::lookAt(eye, m_orbitTarget, {0.0f, 1.0f, 0.0f});
		}

		return glm::lookAt(m_position, m_position + GetForward(), {0.0f, 1.0f, 0.0f});
	}

	glm::mat4 Camera::GetProjectionMatrix(float aspect) const
	{
		glm::mat4 proj = glm::perspective(glm::radians(m_fovDeg), aspect, m_near, m_far);
		proj[1][1] *= -1.0f;
		return proj;
	}

	glm::mat4 Camera::GetViewProjectionMatrix(float aspect) const
	{
		return GetProjectionMatrix(aspect) * GetViewMatrix();
	}

	void Camera::Update(const Input& input, float dt)
	{
		AE_PROFILE_ZONE();
		if (m_mode == CameraMode::Manual)
		{
			return;
		}
		if (input.IsMouseCaptured())
		{
			return;
		}

		if (m_mode == CameraMode::Free)
		{
			const glm::vec2 delta = input.GetMouseDelta();
			const float scroll = input.GetScrollDelta().y;
			const bool alt = input.IsKeyDown(Key::LeftAlt) || input.IsKeyDown(Key::RightAlt);
			const bool rmb = input.IsMouseButtonDown(MouseButton::Right);
			const bool mmb = input.IsMouseButtonDown(MouseButton::Middle);
			const bool lmb = input.IsMouseButtonDown(MouseButton::Left);

			if (rmb)
			{
				m_yaw -= delta.x * m_lookSpeed;
				m_pitch -= delta.y * m_lookSpeed;
				m_pitch = std::clamp(m_pitch, -kPitchLimit, kPitchLimit);

				const glm::vec3 fwd = GetForward();
				const glm::vec3 right = GetRight();
				if (input.IsKeyDown(Key::W))
				{
					m_position += fwd * m_moveSpeed * dt;
				}
				if (input.IsKeyDown(Key::S))
				{
					m_position -= fwd * m_moveSpeed * dt;
				}
				if (input.IsKeyDown(Key::D))
				{
					m_position += right * m_moveSpeed * dt;
				}
				if (input.IsKeyDown(Key::A))
				{
					m_position -= right * m_moveSpeed * dt;
				}
				if (input.IsKeyDown(Key::E))
				{
					m_position.y += m_moveSpeed * dt;
				}
				if (input.IsKeyDown(Key::Q))
				{
					m_position.y -= m_moveSpeed * dt;
				}

				if (scroll != 0.0f)
				{
					m_moveSpeed = std::clamp(m_moveSpeed + scroll * 0.5f, 0.5f, 200.0f);
				}
			}
			else if (alt && lmb)
			{
				const glm::vec3 pivot = m_position + GetForward() * m_focusDistance;
				m_yaw -= delta.x * m_orbitSpeed;
				m_pitch -= delta.y * m_orbitSpeed;
				m_pitch = std::clamp(m_pitch, -kPitchLimit, kPitchLimit);
				m_position = pivot - GetForward() * m_focusDistance;
			}
			else if (mmb)
			{
				const glm::vec3 right = GetRight();
				const glm::vec3 up = glm::normalize(glm::cross(right, GetForward()));
				const float panScale = m_focusDistance * 0.0015f + 0.001f;
				m_position += (right * -delta.x + up * delta.y) * panScale;
			}

			if (!rmb && scroll != 0.0f)
			{
				const glm::vec3 pivot = m_position + GetForward() * m_focusDistance;
				m_focusDistance = std::clamp(m_focusDistance * std::pow(0.85f, scroll), 0.2f, 5000.0f);
				m_position = pivot - GetForward() * m_focusDistance;
			}
		}
		else if (m_mode == CameraMode::Orbit)
		{
			if (input.IsMouseButtonDown(MouseButton::Left))
			{
				const glm::vec2 delta = input.GetMouseDelta();
				m_orbitYaw += delta.x * m_orbitSpeed;
				m_orbitPitch -= delta.y * m_orbitSpeed;
				m_orbitPitch = std::clamp(m_orbitPitch, -kPitchLimit, kPitchLimit);
			}

			const float scroll = input.GetScrollDelta().y;
			if (scroll != 0.0f)
			{
				m_orbitDistance = std::max(0.1f, m_orbitDistance - scroll * m_zoomSpeed);
			}
		}
	}
} // namespace aether

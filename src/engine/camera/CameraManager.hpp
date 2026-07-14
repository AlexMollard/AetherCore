#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <unordered_map>

#include "camera/Camera.hpp"

namespace aether
{
	class Input;

	struct CameraHandle
	{
		uint32_t id = 0;

		[[nodiscard]] bool IsValid() const
		{
			return id != 0;
		}

		[[nodiscard]] bool operator==(const CameraHandle&) const = default;
	};

	// Cameras in CameraMode::Manual are never updated automatically.
	class CameraManager
	{
	public:
		[[nodiscard]] CameraHandle Create(const CameraDesc& desc = {});
		void Destroy(CameraHandle handle);

		[[nodiscard]] Camera& Get(CameraHandle handle);
		[[nodiscard]] const Camera& Get(CameraHandle handle) const;

		[[nodiscard]] Camera* TryGet(CameraHandle handle);
		[[nodiscard]] const Camera* TryGet(CameraHandle handle) const;

		void SetMainCamera(CameraHandle handle);

		[[nodiscard]] CameraHandle GetMainCamera() const
		{
			return m_mainCamera;
		}

		[[nodiscard]] bool HasMainCamera() const
		{
			return m_mainCamera.IsValid();
		}

		[[nodiscard]] Camera* TryGetMainCamera();
		[[nodiscard]] const Camera* TryGetMainCamera() const;

		[[nodiscard]] glm::mat4 GetMainViewProjection(float aspect) const;
		[[nodiscard]] glm::mat4 GetMainView() const;
		[[nodiscard]] glm::mat4 GetMainProjection(float aspect) const;

		void Update(const Input& input, float dt);

	private:
		uint32_t m_nextId = 1;
		std::unordered_map<uint32_t, Camera> m_cameras;
		CameraHandle m_mainCamera{};
	};
} // namespace aether

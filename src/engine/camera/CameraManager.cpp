#include "camera/CameraManager.hpp"

#include "utils/Assert.hpp"
#include "utils/Profiler.hpp"

namespace aether
{

	CameraHandle CameraManager::Create(const CameraDesc& desc)
	{
		AE_PROFILE_ZONE();
		const uint32_t id = m_nextId++;
		m_cameras.emplace(id, Camera{ desc });
		return CameraHandle{ id };
	}

	void CameraManager::Destroy(CameraHandle handle)
	{
		AE_PROFILE_ZONE();
		if (!handle.IsValid())
		{
			return;
		}

		m_cameras.erase(handle.id);

		if (m_mainCamera == handle)
		{
			m_mainCamera = {};
		}
	}

	Camera& CameraManager::Get(CameraHandle handle)
	{
		auto it = m_cameras.find(handle.id);
		AE_ASSERT_ALWAYS(it != m_cameras.end(), "CameraManager::Get: invalid camera handle.");
		return it->second;
	}

	const Camera& CameraManager::Get(CameraHandle handle) const
	{
		auto it = m_cameras.find(handle.id);
		AE_ASSERT_ALWAYS(it != m_cameras.end(), "CameraManager::Get: invalid camera handle.");
		return it->second;
	}

	Camera* CameraManager::TryGet(CameraHandle handle)
	{
		auto it = m_cameras.find(handle.id);
		return (it != m_cameras.end()) ? &it->second : nullptr;
	}

	const Camera* CameraManager::TryGet(CameraHandle handle) const
	{
		auto it = m_cameras.find(handle.id);
		return (it != m_cameras.end()) ? &it->second : nullptr;
	}

	void CameraManager::SetMainCamera(CameraHandle handle)
	{
		AE_PROFILE_ZONE();
		m_mainCamera = handle;
	}

	Camera* CameraManager::TryGetMainCamera()
	{
		return TryGet(m_mainCamera);
	}

	const Camera* CameraManager::TryGetMainCamera() const
	{
		return TryGet(m_mainCamera);
	}

	glm::mat4 CameraManager::GetMainViewProjection(float aspect) const
	{
		if (const Camera* cam = TryGetMainCamera())
		{
			return cam->GetViewProjectionMatrix(aspect);
		}
		return glm::mat4{ 1.0f };
	}

	glm::mat4 CameraManager::GetMainView() const
	{
		if (const Camera* cam = TryGetMainCamera())
		{
			return cam->GetViewMatrix();
		}
		return glm::mat4{ 1.0f };
	}

	glm::mat4 CameraManager::GetMainProjection(float aspect) const
	{
		if (const Camera* cam = TryGetMainCamera())
		{
			return cam->GetProjectionMatrix(aspect);
		}
		return glm::mat4{ 1.0f };
	}

	void CameraManager::Update(const Input& input, float dt)
	{
		AE_PROFILE_ZONE();
		for (auto& [id, cam]: m_cameras)
		{
			cam.Update(input, dt);
		}
	}
} // namespace aether

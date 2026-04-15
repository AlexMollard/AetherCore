#include "CameraManager.hpp"

#include <stdexcept>

namespace aether
{
	// ── Camera lifecycle ──────────────────────────────────────────────────────

	CameraHandle CameraManager::Create(const CameraDesc& desc)
	{
		const uint32_t id = m_nextId++;
		m_cameras.emplace(id, Camera{ desc });
		return CameraHandle{ id };
	}

	void CameraManager::Destroy(CameraHandle handle)
	{
		if (!handle.IsValid())
			return;

		m_cameras.erase(handle.id);

		if (m_mainCamera == handle)
			m_mainCamera = {};
	}

	// ── Camera access ─────────────────────────────────────────────────────────

	Camera& CameraManager::Get(CameraHandle handle)
	{
		auto it = m_cameras.find(handle.id);
		if (it == m_cameras.end())
			throw std::out_of_range("CameraManager::Get: invalid camera handle.");
		return it->second;
	}

	const Camera& CameraManager::Get(CameraHandle handle) const
	{
		auto it = m_cameras.find(handle.id);
		if (it == m_cameras.end())
			throw std::out_of_range("CameraManager::Get: invalid camera handle.");
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

	// ── Main camera ───────────────────────────────────────────────────────────

	void CameraManager::SetMainCamera(CameraHandle handle)
	{
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

	// ── Matrix helpers ────────────────────────────────────────────────────────

	glm::mat4 CameraManager::GetMainViewProjection(float aspect) const
	{
		if (const Camera* cam = TryGetMainCamera())
			return cam->GetViewProjectionMatrix(aspect);
		return glm::mat4{ 1.0f };
	}

	glm::mat4 CameraManager::GetMainView() const
	{
		if (const Camera* cam = TryGetMainCamera())
			return cam->GetViewMatrix();
		return glm::mat4{ 1.0f };
	}

	glm::mat4 CameraManager::GetMainProjection(float aspect) const
	{
		if (const Camera* cam = TryGetMainCamera())
			return cam->GetProjectionMatrix(aspect);
		return glm::mat4{ 1.0f };
	}

	// ── Per-frame update ──────────────────────────────────────────────────────

	void CameraManager::Update(const Input& input, float dt)
	{
		for (auto& [id, cam]: m_cameras)
			cam.Update(input, dt);
	}
} // namespace aether

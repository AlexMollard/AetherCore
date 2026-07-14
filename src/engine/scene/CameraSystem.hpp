#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "camera/CameraManager.hpp"
#include "scene/System.hpp"

namespace aether
{
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

		[[nodiscard]] CameraHandle GetMainCameraBacking() const
		{
			return m_mainBacking;
		}

		void SetApplyMainCamera(bool apply)
		{
			m_applyMainCamera = apply;
		}

	private:
		CameraManager& m_cameras;
		bool m_applyMainCamera = true;
		std::unordered_map<std::uint32_t, CameraHandle> m_backing;
		std::unordered_set<std::uint32_t> m_seenScratch;
		CameraHandle m_mainBacking{};
	};
} // namespace aether

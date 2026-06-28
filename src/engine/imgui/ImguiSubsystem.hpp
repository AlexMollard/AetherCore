#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include <imgui.h>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class GpuDevice;
	struct FrameTarget;
	class ImguiFrameData;
	class ServiceContainer;

	namespace gpu
	{
		class CommandList;
	}

	// Owns Dear ImGui lifetime for engine/tooling UI.
	class ImguiSubsystem
	{
	public:
		ImguiSubsystem() = default;
		~ImguiSubsystem();

		ImguiSubsystem(const ImguiSubsystem&) = delete;
		ImguiSubsystem& operator=(const ImguiSubsystem&) = delete;
		ImguiSubsystem(ImguiSubsystem&&) = delete;
		ImguiSubsystem& operator=(ImguiSubsystem&&) = delete;

		void Init(ServiceContainer& services);
		void Shutdown(ServiceContainer& services);

		void BeginFrame(ServiceContainer& services, float deltaTimeSeconds);
		void CaptureFrame(ImguiFrameData& outFrame);
		void RenderFrame(const ImguiFrameData& frame, gpu::CommandList& commands, const FrameTarget& target);
		[[nodiscard]] ImTextureID RegisterTexture(gpu::ImageView imageView, gpu::ImageLayout layout);
		void UnregisterTexture(ImTextureID textureId);

		[[nodiscard]] bool IsInitialized() const noexcept
		{
			return m_initialized;
		}

		[[nodiscard]] bool WantsInputCapture() const noexcept
		{
			return m_wantsInputCapture;
		}

		[[nodiscard]] float GetLastRenderCpuTimeMs() const noexcept
		{
			return m_lastRenderCpuTimeMs.load(std::memory_order_relaxed);
		}

	private:
		void InitBackends(ServiceContainer& services);
		void ShutdownBackends();

		bool m_initialized = false;
		bool m_backendsInitialized = false;
		bool m_wantsInputCapture = false;
		std::uint64_t m_frameIndex = 0;
		std::atomic<float> m_lastRenderCpuTimeMs = 0.0f;
		std::mutex m_mutex;
		std::optional<std::unique_lock<std::mutex>> m_gameThreadFrameLock;
		std::vector<std::byte> m_fontData;
	};
} // namespace aether

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

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

		[[nodiscard]] bool IsInitialized() const noexcept
		{
			return m_initialized;
		}

	private:
		void InitBackends(ServiceContainer& services);
		void ShutdownBackends();

		bool m_initialized = false;
		bool m_backendsInitialized = false;
		std::uint64_t m_frameIndex = 0;
		std::mutex m_mutex;
		std::optional<std::unique_lock<std::mutex>> m_gameThreadFrameLock;
	};
} // namespace aether

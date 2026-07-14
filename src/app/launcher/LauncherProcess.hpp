#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace aether::app::launcher
{
	enum class EditorStartupState
	{
		Pending,
		Ready,
		Exited,
	};

	class EditorLaunch final
	{
	public:
		EditorLaunch() = default;
		~EditorLaunch();

		EditorLaunch(const EditorLaunch&) = delete;
		EditorLaunch& operator=(const EditorLaunch&) = delete;
		EditorLaunch(EditorLaunch&& other) noexcept;
		EditorLaunch& operator=(EditorLaunch&& other) noexcept;

		[[nodiscard]] EditorStartupState Poll() const;

	private:
		EditorLaunch(std::uintptr_t processHandle, std::uintptr_t readyEventHandle);
		void Reset();

		std::uintptr_t m_processHandle = 0;
		std::uintptr_t m_readyEventHandle = 0;

		friend std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort);
	};

	std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort = 0);
} // namespace aether::app::launcher

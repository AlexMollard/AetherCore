#pragma once

#include <climits>
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

		friend std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort, int centerX, int centerY);
	};

		// centerX/centerY are the desktop point the editor should open centred on - the
	// launcher's own centre, so the editor appears where the launcher was rather than
	// wherever the OS decides to put a new window (on a multi-monitor desktop, often a
	// different screen). INT_MIN leaves the placement alone.
	std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort = 0, int centerX = INT_MIN, int centerY = INT_MIN);
} // namespace aether::app::launcher

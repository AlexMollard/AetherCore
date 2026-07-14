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

	// Spawn the Editor process for `projectRoot` as a detached child:
	//   "<launcher-exe-dir>/<AETHER_EDITOR_EXE_NAME> --project <projectRoot>"
	// A named event is passed to the editor and signalled only after its application
	// layers are attached. The launcher polls EditorLaunch before it closes, so a
	// process that merely started but failed while booting leaves the hub available.
	//
	// controlPort > 0 forwards AETHER_CONTROL_PORT=<controlPort> to the spawned editor
	// so its ControlServer auto-starts on that port and the AetherCore MCP / aether-ctl
	// can drive it (see tools/mcp/). controlPort <= 0 leaves the child's environment
	// untouched (the editor starts no control endpoint unless the env already sets one).
	std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort = 0);
} // namespace aether::app::launcher

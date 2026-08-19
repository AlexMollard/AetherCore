#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace aether::app
{
	struct LayerContext;
	struct EditorProjectContext;
} // namespace aether::app

namespace aether::editor
{
	// A recovery copy of one scene, found on disk and newer than the scene it shadows.
	struct RecoveredScene
	{
		std::string sceneName;
		std::filesystem::path recoveryFile;
		std::filesystem::file_time_type savedAt;
		// Seconds the recovery file is ahead of the saved scene. Large means a lot of
		// unsaved work was in flight; the scene file itself is always left untouched.
		long long secondsAheadOfScene = 0;
	};

	// Periodic crash insurance for the editor.
	//
	// The engine writes a minidump when it dies, so a crash was always anticipated - the
	// unsaved scene work it costs was not. This writes a copy of the live scene beside the
	// project on a timer whenever there are unsaved edits.
	//
	// Two properties matter more than the cadence:
	//
	//  - It NEVER writes the scene file. A recovery copy lands under .aether/recovery/, so
	//    a half-finished or wrong autosave cannot damage the thing it exists to protect,
	//    and restoring is always the user's explicit choice.
	//  - The ECS read happens on the game thread (it must), but serialising and writing are
	//    handed to the IO executor, so a large scene does not stall a frame. Saving 10k
	//    entities measured ~900 ms synchronously - a hitch that size every few minutes is
	//    its own reason to turn the feature off.
	class AutosaveService
	{
	public:
		// Call once per editor frame. Cheap and early-outs unless a save is actually due:
		// no project, mid-Play, no unsaved edits, or the interval has not elapsed.
		void Tick(app::LayerContext& context);

		// Recovery copies that are newer than the scene they shadow, newest first. Empty
		// when there is nothing to offer, which is the normal case after a clean exit.
		[[nodiscard]] static std::vector<RecoveredScene> FindRecoverable(const app::EditorProjectContext& project);

		// Promote a recovery copy over the real scene file. Deliberately explicit: nothing
		// restores automatically, because silently preferring an autosave to the file the
		// user last saved would be the same class of surprise this service exists to avoid.
		[[nodiscard]] static bool Restore(const app::EditorProjectContext& project, const std::string& sceneName, std::string& error);

		// Drop the recovery copy for a scene - used after a real save makes it redundant.
		static void Discard(const app::EditorProjectContext& project, const std::string& sceneName);

		// Whether a recovery copy describes the same scene as the file it shadows, once the
		// differences that carry no work are normalised away (entity order, generated node
		// ids and guids, int-vs-float spelling of whole numbers). False when either file
		// cannot be read or parsed, so a copy is never dropped on a maybe.
		[[nodiscard]] static bool HoldsNothingNew(const std::filesystem::path& recoveryFile, const std::filesystem::path& sceneFile);

		[[nodiscard]] static std::filesystem::path RecoveryDirectory(const app::EditorProjectContext& project);

	private:
		std::chrono::steady_clock::time_point m_lastSave{};
		std::uint64_t m_lastSavedEditSeq = 0;
		bool m_started = false;
	};
} // namespace aether::editor

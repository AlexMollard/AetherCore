#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "scene/SceneSerializer.hpp"

namespace aether::app::scene
{
	// Off-thread scene serialization + disk writes. Capturing the scene from the
	// ECS must happen on the main thread (that produces a SceneDescription value),
	// but the heavy part - building the TOML tree, stringifying, encoding the
	// binary, and two disk writes (~15-20 ms even for a small scene) - is pure work
	// over that value and does not belong on the frame. The worker owns a single
	// thread and coalesces pending saves by scene name (latest wins), so holding
	// Ctrl+S never backs up a queue. Pending writes are flushed on destruction, so
	// a save is never lost to shutdown.
	class BackgroundSceneWriter
	{
	public:
		BackgroundSceneWriter();
		~BackgroundSceneWriter();

		BackgroundSceneWriter(const BackgroundSceneWriter&) = delete;
		BackgroundSceneWriter& operator=(const BackgroundSceneWriter&) = delete;

		// Enqueue an already-captured scene for asynchronous write. Returns
		// immediately; the frame pays nothing beyond the ECS capture the caller did.
		void RequestSave(std::string sceneName, SceneDescription desc);

		// Block until every pending write has completed. Use before a launcher
		// hand-off or any point that must observe the file on disk.
		void Flush();

		struct Completion
		{
			std::string sceneName;
			bool ok = false;
		};

		// Writes that finished since the last call, in completion order. The result of
		// the write used to be dropped on the floor: a save that failed still reported
		// success, cleared the unsaved-changes guard and deleted the recovery copy, so a
		// failed save destroyed the one thing that could have recovered the work. Drain
		// this every frame and act on what actually reached disk.
		[[nodiscard]] std::vector<Completion> TakeCompletions();

	private:
		void Run();

		std::thread m_thread;
		std::mutex m_mutex;
		std::condition_variable m_wake;    // worker: new job or stop requested
		std::condition_variable m_drained; // Flush(): queue empty and nothing in flight
		std::unordered_map<std::string, SceneDescription> m_pending;
		std::vector<Completion> m_completions;
		bool m_busy = false;
		bool m_stop = false;
	};
} // namespace aether::app::scene

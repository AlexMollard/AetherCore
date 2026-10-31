#include <doctest/doctest.h>

#include <atomic>

#include "io/IOThread.hpp"
#include "scene/BackgroundSceneWriter.hpp"

TEST_CASE("I/O workers can immediately execute and drain across repeated construction")
{
	std::atomic<int> completed = 0;
	for (int iteration = 0; iteration < 128; ++iteration)
	{
		{
			aether::io::IoExecutor worker;
			worker.Submit(aether::io::IOPriority::Normal, [&] { ++completed; });
			worker.Flush();
			CHECK(completed.load() == 2 * iteration + 1);
			// Destruction also drains work without an explicit Flush.
			worker.Submit(aether::io::IOPriority::Normal, [&] { ++completed; });
		}
		CHECK(completed.load() == 2 * (iteration + 1));
	}
	CHECK(completed.load() == 256);
}

TEST_CASE("Scene writer can flush and stop immediately after construction")
{
	for (int iteration = 0; iteration < 128; ++iteration)
	{
		aether::app::scene::BackgroundSceneWriter writer;
		writer.Flush();
		CHECK(writer.TakeCompletions().empty());
	}
}

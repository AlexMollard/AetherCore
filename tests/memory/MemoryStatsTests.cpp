#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "memory/MemoryStats.hpp"
#include "memory/MemoryTag.hpp"

using namespace aether::memory;

TEST_CASE("Allocations accumulate and frees subtract") {
    MemoryStats stats;
    stats.RecordAllocation(MemTag::Mesh, 100);
    stats.RecordAllocation(MemTag::Mesh, 40);

    CHECK(stats.Get(MemTag::Mesh).currentBytes == 140);
    CHECK(stats.Get(MemTag::Mesh).totalAllocations == 2);
    CHECK(stats.Get(MemTag::Mesh).LiveCount() == 2);

    stats.RecordFree(MemTag::Mesh, 100);
    CHECK(stats.Get(MemTag::Mesh).currentBytes == 40);
    CHECK(stats.Get(MemTag::Mesh).LiveCount() == 1);
}

// The peak is the high-water mark, not the current value, so freeing must not lower it.
TEST_CASE("The peak survives a free") {
    MemoryStats stats;
    stats.RecordAllocation(MemTag::Particles, 300);
    CHECK(stats.Get(MemTag::Particles).peakBytes == 300);

    stats.RecordFree(MemTag::Particles, 300);
    CHECK(stats.Get(MemTag::Particles).currentBytes == 0);
    CHECK(stats.Get(MemTag::Particles).peakBytes == 300);

    stats.RecordAllocation(MemTag::Particles, 50);
    CHECK(stats.Get(MemTag::Particles).peakBytes == 300);
}

// Resetting to zero would record a peak below memory that is live at that instant - a figure
// that never occurred.
TEST_CASE("Resetting peaks drops them to what is currently held, not to zero") {
    MemoryStats stats;
    stats.RecordAllocation(MemTag::Texture, 1000);
    stats.RecordFree(MemTag::Texture, 900);
    stats.RecordAllocation(MemTag::Texture, 50);
    CHECK(stats.Get(MemTag::Texture).peakBytes == 1000);

    stats.ResetPeaks();

    CHECK(stats.Get(MemTag::Texture).peakBytes == 150);
    CHECK(stats.Get(MemTag::Texture).currentBytes == 150);
    CHECK(stats.Get(MemTag::Texture).totalAllocations == 2);
}

TEST_CASE("Tags are independent and totals sum across them") {
    MemoryStats stats;
    stats.RecordAllocation(MemTag::Audio, 64);
    stats.RecordAllocation(MemTag::Net, 32);

    CHECK(stats.Get(MemTag::Audio).currentBytes == 64);
    CHECK(stats.Get(MemTag::Net).currentBytes == 32);
    CHECK(stats.Get(MemTag::Mesh).currentBytes == 0);
    CHECK(stats.Total().currentBytes == 96);
    CHECK(stats.Total().totalAllocations == 2);
}

// Called from the allocation path, where faulting on a bad tag would be far worse than
// attributing it vaguely.
TEST_CASE("An out-of-range tag is folded into Unknown rather than corrupting memory") {
    MemoryStats stats;
    stats.RecordAllocation(static_cast<MemTag>(kMemTagCount + 7), 128);

    CHECK(stats.Get(MemTag::Unknown).currentBytes == 128);
    CHECK(stats.Total().currentBytes == 128);
}

// A reader can observe a free before the matching allocation, and an unsigned underflow
// would report billions of live blocks instead of approximately none.
TEST_CASE("Live count saturates instead of underflowing") {
    MemoryStats stats;
    stats.RecordFree(MemTag::Temp, 0);
    CHECK(stats.Get(MemTag::Temp).LiveCount() == 0);
}

// The counters are lock-free and shared; this is the shape that catches a lost update.
TEST_CASE("Concurrent recording loses no allocations") {
    MemoryStats stats;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 20'000;

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&stats]
        {
            for (int i = 0; i < kPerThread; ++i)
            {
                stats.RecordAllocation(MemTag::Temp, 16);
                stats.RecordFree(MemTag::Temp, 16);
            }
        });
    }
    for (std::thread& worker: workers)
    {
        worker.join();
    }

    CHECK(stats.Get(MemTag::Temp).totalAllocations == kThreads * kPerThread);
    CHECK(stats.Get(MemTag::Temp).totalFrees == kThreads * kPerThread);
    CHECK(stats.Get(MemTag::Temp).currentBytes == 0);
    CHECK(stats.Get(MemTag::Temp).peakBytes >= 16);
}

TEST_CASE("The global instance is usable and shared") {
    const std::uint64_t before = GlobalStats().Get(MemTag::Io).currentBytes;
    GlobalStats().RecordAllocation(MemTag::Io, 256);
    CHECK(GlobalStats().Get(MemTag::Io).currentBytes == before + 256);
    GlobalStats().RecordFree(MemTag::Io, 256);
    CHECK(GlobalStats().Get(MemTag::Io).currentBytes == before);
}

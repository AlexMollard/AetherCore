#include <doctest/doctest.h>

#include <cstddef>
#include <new>
#include <vector>

#include "memory/MemoryBackend.hpp"

using namespace aether;

// Linking mimalloc and USING it are different claims. A replacement operator new defined in
// a static library is only linked in if something pulls its object file out of the archive,
// so this can fail while the build succeeds and every allocation quietly goes to the CRT.
TEST_CASE("The mimalloc operator new replacement is actually in effect") {
    CHECK(memory::IsMimallocActive());
}

// Over-aligned types were served by the CRT before this replacement existed, because the
// previous override omitted the C++17 aligned overloads entirely. Allocating from mimalloc
// and releasing to the CRT is exactly the mismatch that produces a heap corruption much
// later and somewhere else.
TEST_CASE("Over-aligned allocations round-trip through the replacement") {
    struct alignas(64) OverAligned
    {
        char payload[64];
    };

    auto* single = new OverAligned{};
    CHECK(reinterpret_cast<std::uintptr_t>(single) % 64 == 0);
    delete single;

    auto* many = new OverAligned[4]{};
    CHECK(reinterpret_cast<std::uintptr_t>(many) % 64 == 0);
    delete[] many;
}

TEST_CASE("The nothrow forms return memory rather than throwing") {
    void* ptr = ::operator new(128, std::nothrow);
    REQUIRE(ptr != nullptr);
    ::operator delete(ptr, std::nothrow);
}

// The standard library allocates through the same replaced operators, so a mismatch here
// would show up as a crash in ordinary container use rather than anywhere near this file.
TEST_CASE("Standard containers allocate and free through the replacement") {
    std::vector<std::size_t> values;
    for (std::size_t i = 0; i < 10'000; ++i)
    {
        values.push_back(i);
    }
    CHECK(values.size() == 10'000);
    CHECK(values.back() == 9'999);
}

#include <doctest/doctest.h>

#include <vector>

#include "utils/LogRingBuffer.hpp"

using namespace aether;

TEST_CASE("LogRingBuffer preserves order and assigns increasing seq") {
    LogRingBuffer buf;
    buf.Push(LogLevel::Info, "Cat", "first");
    buf.Push(LogLevel::Warn, "Cat", "second");

    std::vector<LogRingBuffer::Record> out;
    buf.Snapshot(out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].message == "first");
    CHECK(out[1].message == "second");
    CHECK(out[1].seq == out[0].seq + 1);
    CHECK(out[1].level == LogLevel::Warn);
    CHECK(out[0].category == "Cat");
}

TEST_CASE("LogRingBuffer evicts the oldest records past capacity") {
    LogRingBuffer buf;
    const std::size_t cap = buf.Capacity();
    for (std::size_t i = 0; i < cap + 50; ++i) {
        buf.Push(LogLevel::Info, "C", "m");
    }

    std::vector<LogRingBuffer::Record> out;
    buf.Snapshot(out);
    CHECK(out.size() == cap);
    CHECK(out.front().seq == 50); // first 50 evicted; seq never reused
    CHECK(out.back().seq == cap + 49);
}

TEST_CASE("LogRingBuffer Clear empties the buffer") {
    LogRingBuffer buf;
    buf.Push(LogLevel::Info, "C", "m");
    buf.Clear();

    std::vector<LogRingBuffer::Record> out;
    buf.Snapshot(out);
    CHECK(out.empty());
}

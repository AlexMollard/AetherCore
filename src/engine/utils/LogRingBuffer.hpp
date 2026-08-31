#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "utils/Logger.hpp"

namespace aether
{
	// Fixed-capacity, thread-safe in-memory log tail feeding the editor Console
	class LogRingBuffer
	{
	public:
		struct Record
		{
			LogLevel level = LogLevel::Info;
			std::string category;
			std::string message;
			std::string file;
			int line = 0;
			std::string time;
			std::uint64_t seq = 0; // monotonic insertion sequence (never reused)
		};

		static LogRingBuffer& Get();

		void Push(LogLevel level, std::string_view category, std::string_view message, std::string_view file = {}, int line = 0, std::time_t timestamp = 0);
		void Snapshot(std::vector<Record>& out) const;

		// How many records of each level the buffer currently holds. Kept as running totals
		// so a caller that only needs "are there errors?" - the status bar, every frame -
		// does not copy the whole buffer under the lock to find out.
		struct LevelCounts
		{
			std::size_t error = 0;
			std::size_t warn = 0;
			std::size_t info = 0;
			std::size_t verbose = 0;
		};
		[[nodiscard]] LevelCounts Counts() const;
		void Clear();

		[[nodiscard]] std::size_t Capacity() const noexcept
		{
			return kCapacity;
		}

	private:
		static constexpr std::size_t kCapacity = 4096;

		mutable std::mutex m_mutex;
		std::deque<Record> m_records;
		std::uint64_t m_nextSeq = 0;
		LevelCounts m_counts;

		// Caller holds m_mutex.
		void Tally(LogLevel level, int delta);
	};

	struct CollapsedRecord
	{
		LogRingBuffer::Record record;
		std::size_t count = 1;
	};

	[[nodiscard]] std::vector<CollapsedRecord> CollapseConsecutive(const std::vector<LogRingBuffer::Record>& records);
} // namespace aether

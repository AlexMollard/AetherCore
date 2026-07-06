#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "utils/Logger.hpp" // LogLevel

namespace aether
{
	// Fixed-capacity, thread-safe in-memory log tail feeding the editor Console
	// panel. Written from any thread via Logger::Log (a direct hook), read on the
	// UI thread via Snapshot. Oldest records evict once capacity is exceeded.
	class LogRingBuffer
	{
	public:
		struct Record
		{
			LogLevel level = LogLevel::Info;
			std::string category;
			std::string message;
			std::string file;      // call-site source file (full path; display basename)
			int line = 0;          // call-site line, 0 if unknown
			std::string time;      // "HH:MM:SS" formatted at push, empty if unknown
			std::uint64_t seq = 0; // monotonic insertion sequence (never reused)
		};

		// Process-wide instance the Logger hook feeds and the Console panel reads.
		static LogRingBuffer& Get();

		// file/line/timestamp are optional (default: unknown) so tests and callers
		// without source info stay simple; the Logger hook supplies all three.
		void Push(LogLevel level, std::string_view category, std::string_view message, std::string_view file = {}, int line = 0, std::time_t timestamp = 0);
		void Snapshot(std::vector<Record>& out) const;
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
	};

	// One display row after folding a run of identical log records.
	struct CollapsedRecord
	{
		LogRingBuffer::Record record; // representative: the most recent in the run
		std::size_t count = 1;        // number of consecutive identical records
	};

	// Fold consecutive records sharing level+category+message into one row each,
	// carrying a repeat count (Unity-style "Collapse"). Order is preserved.
	[[nodiscard]] std::vector<CollapsedRecord> CollapseConsecutive(const std::vector<LogRingBuffer::Record>& records);
} // namespace aether

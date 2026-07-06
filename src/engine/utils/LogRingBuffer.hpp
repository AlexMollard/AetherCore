#pragma once

#include <cstddef>
#include <cstdint>
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
			std::uint64_t seq = 0; // monotonic insertion sequence (never reused)
		};

		// Process-wide instance the Logger hook feeds and the Console panel reads.
		static LogRingBuffer& Get();

		void Push(LogLevel level, std::string_view category, std::string_view message);
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
} // namespace aether

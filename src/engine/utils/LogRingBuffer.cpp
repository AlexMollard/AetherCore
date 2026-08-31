#include "utils/LogRingBuffer.hpp"

#include <cstdio>

namespace aether
{
	namespace
	{
		// Thread-safe (each call fills its own tm) "HH:MM:SS" from a time_t.
		std::string FormatClock(std::time_t t)
		{
			if (t == 0)
			{
				return {};
			}
			std::tm tm{};
#if defined(_WIN32)
			localtime_s(&tm, &t);
#else
			localtime_r(&t, &tm);
#endif
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
			return buf;
		}
	} // namespace

	LogRingBuffer& LogRingBuffer::Get()
	{
		static LogRingBuffer instance;
		return instance;
	}

	void LogRingBuffer::Push(LogLevel level, std::string_view category, std::string_view message, std::string_view file, int line, std::time_t timestamp)
	{
		const std::scoped_lock lock(m_mutex);
		m_records.push_back(Record{
		        .level = level,
		        .category = std::string(category),
		        .message = std::string(message),
		        .file = std::string(file),
		        .line = line,
		        .time = FormatClock(timestamp),
		        .seq = m_nextSeq++,
		});
		Tally(level, 1);
		while (m_records.size() > kCapacity)
		{
			// Evicted records leave the buffer, so they leave the totals too - otherwise the
			// status bar would keep reporting errors the console can no longer show.
			Tally(m_records.front().level, -1);
			m_records.pop_front();
		}
	}

	void LogRingBuffer::Tally(const LogLevel level, const int delta)
	{
		const auto apply = [delta](std::size_t& n)
		{
			if (delta < 0 && n > 0)
			{
				--n;
			}
			else if (delta > 0)
			{
				++n;
			}
		};
		switch (level)
		{
			case LogLevel::Error:
				apply(m_counts.error);
				break;
			case LogLevel::Warn:
				apply(m_counts.warn);
				break;
			case LogLevel::Info:
				apply(m_counts.info);
				break;
			case LogLevel::Verbose:
			default:
				apply(m_counts.verbose);
				break;
		}
	}

	LogRingBuffer::LevelCounts LogRingBuffer::Counts() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_counts;
	}

	void LogRingBuffer::Snapshot(std::vector<Record>& out) const
	{
		const std::scoped_lock lock(m_mutex);
		out.assign(m_records.begin(), m_records.end());
	}

	void LogRingBuffer::Clear()
	{
		const std::scoped_lock lock(m_mutex);
		m_records.clear();
		m_counts = {};
	}

	std::vector<CollapsedRecord> CollapseConsecutive(const std::vector<LogRingBuffer::Record>& records)
	{
		std::vector<CollapsedRecord> out;
		out.reserve(records.size());
		for (const LogRingBuffer::Record& r: records)
		{
			if (!out.empty())
			{
				CollapsedRecord& prev = out.back();
				if (prev.record.level == r.level && prev.record.category == r.category && prev.record.message == r.message)
				{
					prev.record = r;
					++prev.count;
					continue;
				}
			}
			out.push_back(CollapsedRecord{r, 1});
		}
		return out;
	}
} // namespace aether

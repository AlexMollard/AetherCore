#include "utils/LogRingBuffer.hpp"

namespace aether
{
	LogRingBuffer& LogRingBuffer::Get()
	{
		static LogRingBuffer instance;
		return instance;
	}

	void LogRingBuffer::Push(LogLevel level, std::string_view category, std::string_view message)
	{
		std::scoped_lock lock(m_mutex);
		m_records.push_back(Record{
		        .level = level,
		        .category = std::string(category),
		        .message = std::string(message),
		        .seq = m_nextSeq++,
		});
		while (m_records.size() > kCapacity)
		{
			m_records.pop_front();
		}
	}

	void LogRingBuffer::Snapshot(std::vector<Record>& out) const
	{
		std::scoped_lock lock(m_mutex);
		out.assign(m_records.begin(), m_records.end());
	}

	void LogRingBuffer::Clear()
	{
		std::scoped_lock lock(m_mutex);
		m_records.clear();
	}
} // namespace aether

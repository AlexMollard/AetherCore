#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace aether
{
	// Safe binary reader for deserializing packed disk formats.
	// Prevents out-of-bounds reads and handles unaligned data via memcpy.
	class BinaryReader
	{
	public:
		BinaryReader(const std::vector<std::byte>& data)
		      : m_data(data.data()), m_end(data.data() + data.size()), m_pos(data.data())
		{
		}

		BinaryReader(const std::byte* data, std::size_t size)
		      : m_data(data), m_end(data + size), m_pos(data)
		{
		}

		// Read a POD type via memcpy. Returns default-constructed T if insufficient data.
		template<typename T>
		T Read()
		{
			if (!CanRead(sizeof(T)))
			{
				return T{};
			}
			T val;
			std::memcpy(&val, m_pos, sizeof(T));
			m_pos += sizeof(T);
			return val;
		}

		// Read a fixed-size array into a caller-provided buffer.
		template<std::size_t N>
		void ReadArray(float* out)
		{
			constexpr std::size_t bytes = N * sizeof(float);
			if (!CanRead(bytes))
			{
				std::memset(out, 0, bytes);
				return;
			}
			std::memcpy(out, m_pos, bytes);
			m_pos += bytes;
		}

		// Read a length-prefixed string (uint16_t length + raw bytes).
		std::string ReadString()
		{
			const uint16_t len = Read<uint16_t>();
			if (len == 0)
			{
				return {};
			}
			if (!CanRead(len))
			{
				return {};
			}
			std::string s(reinterpret_cast<const char*>(m_pos), len);
			m_pos += len;
			return s;
		}

		// Read raw bytes into a buffer.
		void ReadRaw(void* out, std::size_t bytes)
		{
			if (!CanRead(bytes))
			{
				std::memset(out, 0, bytes);
				return;
			}
			std::memcpy(out, m_pos, bytes);
			m_pos += bytes;
		}

		void Skip(std::size_t n)
		{
			if (static_cast<std::size_t>(m_end - m_pos) < n)
			{
				m_pos = m_end;
				return;
			}
			m_pos += n;
		}

		bool CanRead(std::size_t n) const
		{
			return static_cast<std::size_t>(m_end - m_pos) >= n;
		}

		std::size_t Remaining() const
		{
			return static_cast<std::size_t>(m_end - m_pos);
		}

		// Current position as raw pointer (for bulk memcpy of known-safe regions).
		const std::byte* Data() const
		{
			return m_pos;
		}

		// Advance position manually (use after bulk memcpy).
		void Advance(std::size_t n)
		{
			m_pos += n;
		}

	private:
		const std::byte* m_data;
		const std::byte* m_end;
		const std::byte* m_pos;
	};
} // namespace aether

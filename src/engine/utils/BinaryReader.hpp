#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace aether
{
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

		// Read<T>() returns a default-constructed T{} on truncation - fine for a
		// plain count/length (a zero fallback is a sane "nothing here"), but WRONG
		// for any disk-format header struct: every *HeaderDisk type in
		// BinaryFormats.hpp default-initialises its OWN magic/version fields to the
		// value its own CheckMagic() expects (so writer code can do `MeshHeaderDisk
		// hdr{}; hdr.vertexCount = ...;` without re-stating them), which means a
		// truncated or empty file's "failed" T{} passes its own magic check and is
		// silently accepted as a valid, empty asset instead of being rejected. Use
		// this for any header read that will be validated by magic/version;
		// `out` is left untouched on failure so a caller can never mistake it for
		// a successful read by inspecting fields that happen to look right.
		template<typename T>
		[[nodiscard]] bool TryRead(T& out)
		{
			if (!CanRead(sizeof(T)))
			{
				return false;
			}
			std::memcpy(&out, m_pos, sizeof(T));
			m_pos += sizeof(T);
			return true;
		}

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

		std::string ReadString()
		{
			const auto len = Read<uint16_t>();
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
			if (std::cmp_less(m_end - m_pos, n))
			{
				m_pos = m_end;
				return;
			}
			m_pos += n;
		}

		[[nodiscard]] bool CanRead(std::size_t n) const
		{
			return std::cmp_greater_equal(m_end - m_pos, n);
		}

		[[nodiscard]] std::size_t Remaining() const
		{
			return static_cast<std::size_t>(m_end - m_pos);
		}

		[[nodiscard]] const std::byte* Data() const
		{
			return m_pos;
		}

		void Advance(std::size_t n)
		{
			if (std::cmp_less(m_end - m_pos, n))
			{
				m_pos = m_end;
				return;
			}
			m_pos += n;
		}

	private:
		const std::byte* m_data;
		const std::byte* m_end;
		const std::byte* m_pos;
	};
} // namespace aether

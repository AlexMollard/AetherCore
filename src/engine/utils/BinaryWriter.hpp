#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace aether
{
	// Append-only little-endian byte writer, the counterpart to BinaryReader.
	// Strings are length-prefixed with a uint32 (BinaryReader::ReadString uses a
	// uint16, so binary formats that may hold long strings read the length back
	// explicitly rather than via ReadString).
	class BinaryWriter
	{
	public:
		template<typename T>
		void Write(const T& value)
		{
			static_assert(std::is_trivially_copyable_v<T>, "BinaryWriter::Write requires a trivially copyable type");
			const auto* bytes = reinterpret_cast<const std::byte*>(&value);
			m_data.insert(m_data.end(), bytes, bytes + sizeof(T));
		}

		void WriteString(std::string_view text)
		{
			Write<std::uint32_t>(static_cast<std::uint32_t>(text.size()));
			const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
			m_data.insert(m_data.end(), bytes, bytes + text.size());
		}

		void WriteBytes(const void* data, std::size_t size)
		{
			const auto* bytes = reinterpret_cast<const std::byte*>(data);
			m_data.insert(m_data.end(), bytes, bytes + size);
		}

		[[nodiscard]] const std::vector<std::byte>& Bytes() const
		{
			return m_data;
		}

		[[nodiscard]] std::vector<std::byte> Take()
		{
			return std::move(m_data);
		}

	private:
		std::vector<std::byte> m_data;
	};
} // namespace aether

#include "net/NetSerialize.hpp"

#include <cstring>

namespace aether::net
{
	namespace
	{
		template<typename T>
		void Append(std::vector<std::byte>& out, T v)
		{
			const auto* p = reinterpret_cast<const std::byte*>(&v);
			out.insert(out.end(), p, p + sizeof(T));
		}

		// A single packet is never legitimately this large; anything claiming to be is
		// either corrupt or hostile.
		constexpr std::uint32_t kMaxStringBytes = 64u * 1024u;
	} // namespace

	void ByteWriter::U8(std::uint8_t v)
	{
		m_data.push_back(static_cast<std::byte>(v));
	}

	void ByteWriter::U16(std::uint16_t v)
	{
		Append(m_data, v);
	}

	void ByteWriter::U32(std::uint32_t v)
	{
		Append(m_data, v);
	}

	void ByteWriter::I32(std::int32_t v)
	{
		Append(m_data, v);
	}

	void ByteWriter::F32(float v)
	{
		Append(m_data, v);
	}

	void ByteWriter::Str(std::string_view v)
	{
		U32(static_cast<std::uint32_t>(v.size()));
		const auto* p = reinterpret_cast<const std::byte*>(v.data());
		m_data.insert(m_data.end(), p, p + v.size());
	}

	void ByteWriter::Bytes(std::span<const std::byte> v)
	{
		m_data.insert(m_data.end(), v.begin(), v.end());
	}

	bool ByteReader::Want(std::size_t n)
	{
		if (!m_ok || m_cursor + n > m_data.size())
		{
			m_ok = false;
			return false;
		}
		return true;
	}

	std::uint8_t ByteReader::U8()
	{
		if (!Want(1))
		{
			return 0;
		}
		return static_cast<std::uint8_t>(m_data[m_cursor++]);
	}

	std::uint16_t ByteReader::U16()
	{
		if (!Want(sizeof(std::uint16_t)))
		{
			return 0;
		}
		std::uint16_t v = 0;
		std::memcpy(&v, m_data.data() + m_cursor, sizeof(v));
		m_cursor += sizeof(v);
		return v;
	}

	std::uint32_t ByteReader::U32()
	{
		if (!Want(sizeof(std::uint32_t)))
		{
			return 0;
		}
		std::uint32_t v = 0;
		std::memcpy(&v, m_data.data() + m_cursor, sizeof(v));
		m_cursor += sizeof(v);
		return v;
	}

	std::int32_t ByteReader::I32()
	{
		if (!Want(sizeof(std::int32_t)))
		{
			return 0;
		}
		std::int32_t v = 0;
		std::memcpy(&v, m_data.data() + m_cursor, sizeof(v));
		m_cursor += sizeof(v);
		return v;
	}

	float ByteReader::F32()
	{
		if (!Want(sizeof(float)))
		{
			return 0.f;
		}
		float v = 0.f;
		std::memcpy(&v, m_data.data() + m_cursor, sizeof(v));
		m_cursor += sizeof(v);
		return v;
	}

	std::string ByteReader::Str()
	{
		const std::uint32_t len = U32();
		if (!m_ok || len > kMaxStringBytes || !Want(len))
		{
			m_ok = false;
			return {};
		}
		std::string out(reinterpret_cast<const char*>(m_data.data() + m_cursor), len);
		m_cursor += len;
		return out;
	}

	std::vector<std::byte> ByteReader::Bytes(std::size_t n)
	{
		if (!Want(n))
		{
			return {};
		}
		std::vector<std::byte> out(m_data.begin() + static_cast<std::ptrdiff_t>(m_cursor),
		        m_data.begin() + static_cast<std::ptrdiff_t>(m_cursor + n));
		m_cursor += n;
		return out;
	}

	bool IsReplicableFieldType(reflect::FieldType type)
	{
		using reflect::FieldType;
		switch (type)
		{
		case FieldType::Float:
		case FieldType::Int:
		case FieldType::UInt:
		case FieldType::Bool:
		case FieldType::Vec2:
		case FieldType::Vec3:
		case FieldType::Vec4:
		case FieldType::Color3:
		case FieldType::Color4:
		case FieldType::Enum:
		case FieldType::String:
			return true;
		case FieldType::EntityRef:
		case FieldType::List:
			return false;
		}
		return false;
	}

	void WriteFieldValue(ByteWriter& w, const reflect::FieldValue& value)
	{
		using reflect::FieldType;
		switch (value.type)
		{
		case FieldType::Float:
			w.F32(static_cast<float>(value.num));
			break;
		case FieldType::Int:
			w.I32(static_cast<std::int32_t>(value.num));
			break;
		case FieldType::UInt:
			w.U32(static_cast<std::uint32_t>(value.num));
			break;
		case FieldType::Bool:
			w.U8(value.boolean ? 1u : 0u);
			break;
		case FieldType::Vec2:
			w.F32(value.vec.x);
			w.F32(value.vec.y);
			break;
		case FieldType::Vec3:
		case FieldType::Color3:
			w.F32(value.vec.x);
			w.F32(value.vec.y);
			w.F32(value.vec.z);
			break;
		case FieldType::Vec4:
		case FieldType::Color4:
			w.F32(value.vec.x);
			w.F32(value.vec.y);
			w.F32(value.vec.z);
			w.F32(value.vec.w);
			break;
		case FieldType::Enum:
			w.I32(value.enumValue);
			break;
		case FieldType::String:
			w.Str(value.str);
			break;
		case FieldType::EntityRef:
		case FieldType::List:
			break; // not replicable; the schema never includes these
		}
	}

	reflect::FieldValue ReadFieldValue(ByteReader& r, reflect::FieldType type)
	{
		using reflect::FieldType;
		reflect::FieldValue v;
		v.type = type;
		switch (type)
		{
		case FieldType::Float:
			v.num = r.F32();
			break;
		case FieldType::Int:
			v.num = r.I32();
			break;
		case FieldType::UInt:
			v.num = r.U32();
			break;
		case FieldType::Bool:
			v.boolean = r.U8() != 0;
			break;
		case FieldType::Vec2:
			v.vec.x = r.F32();
			v.vec.y = r.F32();
			break;
		case FieldType::Vec3:
		case FieldType::Color3:
			v.vec.x = r.F32();
			v.vec.y = r.F32();
			v.vec.z = r.F32();
			break;
		case FieldType::Vec4:
		case FieldType::Color4:
			v.vec.x = r.F32();
			v.vec.y = r.F32();
			v.vec.z = r.F32();
			v.vec.w = r.F32();
			break;
		case FieldType::Enum:
			v.enumValue = r.I32();
			break;
		case FieldType::String:
			v.str = r.Str();
			break;
		case FieldType::EntityRef:
		case FieldType::List:
			break;
		}
		return v;
	}
} // namespace aether::net

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "scene/reflection/Reflection.hpp"

namespace aether::net
{
	// Little-endian byte cursor. Deliberately not a general serialization library:
	// it exists so replication packets have one obvious, bounds-checked encoding.
	class ByteWriter
	{
	public:
		void U8(std::uint8_t v);
		void U16(std::uint16_t v);
		void U32(std::uint32_t v);
		void I32(std::int32_t v);
		void F32(float v);
		void Str(std::string_view v); // u32 length + bytes
		void Bytes(std::span<const std::byte> v);

		[[nodiscard]] std::size_t Size() const
		{
			return m_data.size();
		}

		[[nodiscard]] std::vector<std::byte> Take()
		{
			return std::move(m_data);
		}

		[[nodiscard]] std::span<const std::byte> View() const
		{
			return m_data;
		}

	private:
		std::vector<std::byte> m_data;
	};

	// Bounds-checked reader. A remote peer can send anything, so every read past the
	// end sets a sticky failure and returns a zero value rather than reading garbage
	// or throwing. Callers check Ok() once after parsing rather than at every field.
	class ByteReader
	{
	public:
		explicit ByteReader(std::span<const std::byte> data)
		      : m_data(data)
		{
		}

		std::uint8_t U8();
		std::uint16_t U16();
		std::uint32_t U32();
		std::int32_t I32();
		float F32();
		std::string Str();
		std::vector<std::byte> Bytes(std::size_t n);
		void Fail(); // set the sticky failure without reading anything

		[[nodiscard]] bool Ok() const
		{
			return m_ok;
		}

		[[nodiscard]] std::size_t Remaining() const
		{
			return m_ok ? m_data.size() - m_cursor : 0;
		}

	private:
		bool Want(std::size_t n);

		std::span<const std::byte> m_data;
		std::size_t m_cursor = 0;
		bool m_ok = true;
	};

	// FieldValue codec. The type tag is NOT written - the schema (now verified by
	// the snapshot header's hash) already agrees on it at both ends, so writing it
	// per field would be pure overhead on every snapshot.
	void WriteFieldValue(ByteWriter& w, const reflect::FieldValue& value);

	// True when `value` can ride this codec: finite floats (and within float
	// range - the wire is 32-bit, so a double past FLT_MAX would arrive as inf),
	// and strings within the reader's length cap. The writer must skip values that
	// fail this (a value every receiver drops must never reach the wire), and the
	// reader rejects them on the way in, so no peer can install NaN/Inf into a
	// component.
	[[nodiscard]] bool IsSendableFieldValue(const reflect::FieldValue& value);

	// Decodes one field value of `type`. Returns false when the payload is
	// unusable, in exactly two flavours the caller distinguishes with Ok(): a
	// failed reader (truncated - the rest of the packet is unparseable), or a
	// decoded value that IsSendableFieldValue rejects, whose bytes ARE consumed so
	// the caller can skip just this field and keep parsing the packet.
	[[nodiscard]] bool ReadFieldValue(ByteReader& r, reflect::FieldType type, reflect::FieldValue& out);

	// Strict form: a decoded-but-rejected value (e.g. a non-finite float) fails the
	// reader, so callers that cannot skip a field individually still refuse the
	// packet instead of applying poison.
	[[nodiscard]] reflect::FieldValue ReadFieldValue(ByteReader& r, reflect::FieldType type);

	// True for types replication can carry. EntityRef and List are excluded: an entity
	// id is meaningless across machines (it must go through a netId), and a list is a
	// variable-size structure whose change detection would need its own design.
	[[nodiscard]] bool IsReplicableFieldType(reflect::FieldType type);
} // namespace aether::net

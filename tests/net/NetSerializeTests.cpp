#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "net/NetSerialize.hpp"

using namespace aether;
using namespace aether::reflect;

TEST_CASE("ByteWriter and ByteReader round-trip primitives")
{
	net::ByteWriter w;
	w.U8(7);
	w.U16(1234);
	w.U32(0xDEADBEEF);
	w.I32(-42);
	w.F32(1.5f);
	w.Str("hello");

	const std::vector<std::byte> bytes = w.Take();
	net::ByteReader r{bytes};

	CHECK(r.U8() == 7);
	CHECK(r.U16() == 1234);
	CHECK(r.U32() == 0xDEADBEEF);
	CHECK(r.I32() == -42);
	CHECK(r.F32() == doctest::Approx(1.5f));
	CHECK(r.Str() == "hello");
	CHECK(r.Ok());
	CHECK(r.Remaining() == 0);
}

TEST_CASE("ByteReader fails safely past the end instead of reading garbage")
{
	net::ByteWriter w;
	w.U8(1);
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	CHECK(r.U8() == 1);
	CHECK(r.Ok());

	CHECK(r.U32() == 0u); // past the end
	CHECK_FALSE(r.Ok());
	CHECK(r.Str().empty()); // still safe once failed
	CHECK_FALSE(r.Ok());
}

TEST_CASE("ByteReader rejects a string length that overruns the buffer")
{
	// A hostile peer claims a 1 GB string in a 5-byte packet.
	net::ByteWriter w;
	w.U32(1024u * 1024u * 1024u);
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	CHECK(r.Str().empty());
	CHECK_FALSE(r.Ok());
}

TEST_CASE("FieldValue round-trips for every replicable type")
{
	auto roundTrip = [](FieldValue in, FieldType type)
	{
		net::ByteWriter w;
		net::WriteFieldValue(w, in);
		const std::vector<std::byte> bytes = w.Take();
		net::ByteReader r{bytes};
		FieldValue out = net::ReadFieldValue(r, type);
		CHECK(r.Ok());
		return out;
	};

	FieldValue f;
	f.type = FieldType::Float;
	f.num = 2.5;
	CHECK(roundTrip(f, FieldType::Float).num == doctest::Approx(2.5));

	FieldValue i;
	i.type = FieldType::Int;
	i.num = -17;
	CHECK(roundTrip(i, FieldType::Int).num == doctest::Approx(-17));

	FieldValue b;
	b.type = FieldType::Bool;
	b.boolean = true;
	CHECK(roundTrip(b, FieldType::Bool).boolean);

	FieldValue v3;
	v3.type = FieldType::Vec3;
	v3.vec = {1.f, 2.f, 3.f, 0.f};
	const FieldValue outV3 = roundTrip(v3, FieldType::Vec3);
	CHECK(outV3.vec.x == doctest::Approx(1.f));
	CHECK(outV3.vec.z == doctest::Approx(3.f));

	FieldValue c4;
	c4.type = FieldType::Color4;
	c4.vec = {0.1f, 0.2f, 0.3f, 0.4f};
	CHECK(roundTrip(c4, FieldType::Color4).vec.w == doctest::Approx(0.4f));

	FieldValue e;
	e.type = FieldType::Enum;
	e.enumValue = 3;
	CHECK(roundTrip(e, FieldType::Enum).enumValue == 3);

	FieldValue s;
	s.type = FieldType::String;
	s.str = "player one";
	CHECK(roundTrip(s, FieldType::String).str == "player one");
}

// A peer can send the NaN bit pattern for any float field it likes. Nothing
// downstream - change detection, interpolation, physics - behaves once NaN is in
// a transform, so the codec must refuse it instead of returning it. Before the
// guard this round trip handed back (NaN, +inf) with the reader still Ok().
TEST_CASE("A non-finite float payload is refused by the strict reader form")
{
	net::ByteWriter w;
	w.F32(std::numeric_limits<float>::quiet_NaN());
	w.F32(std::numeric_limits<float>::infinity());
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	const FieldValue v = net::ReadFieldValue(r, FieldType::Vec2);
	CHECK_FALSE(r.Ok());
	CHECK(std::isnan(v.vec.x)); // decoded, then refused - not silently zeroed
}

TEST_CASE("A rejected field value consumes its bytes so the rest of the packet survives")
{
	// A Vec3 of NaN followed by a marker the reader must still reach: when the
	// byte length is known, rejecting the value must not blackhole every field
	// after it in the packet.
	net::ByteWriter w;
	w.F32(std::numeric_limits<float>::quiet_NaN());
	w.F32(std::numeric_limits<float>::quiet_NaN());
	w.F32(std::numeric_limits<float>::quiet_NaN());
	w.U32(0xC0FFEE00u);
	const std::vector<std::byte> bytes = w.Take();

	net::ByteReader r{bytes};
	FieldValue v;
	CHECK_FALSE(net::ReadFieldValue(r, FieldType::Vec3, v));
	CHECK(r.Ok()); // rejected as a value, not failed as a read
	CHECK(r.U32() == 0xC0FFEE00u); // the packet's remainder still parses
}

TEST_CASE("IsSendableFieldValue marks exactly what can survive the wire")
{
	// The writer side of the reader's guards: a value that fails this must never
	// be serialized, because every receiver drops it - and a sticky reader
	// failure drops the whole packet carrying it.
	FieldValue v;
	v.type = FieldType::Vec3;
	v.vec = {1.f, 2.f, 3.f, 0.f};
	CHECK(net::IsSendableFieldValue(v));

	v.vec.x = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(net::IsSendableFieldValue(v));
	v.vec.x = 1.f;
	v.vec.w = std::numeric_limits<float>::infinity();
	CHECK_FALSE(net::IsSendableFieldValue(v));

	// num is a double: a finite double past FLT_MAX narrows to inf on the 32-bit
	// wire (and the narrowing cast is undefined behaviour), so the check must be
	// range-aware, not merely isfinite.
	FieldValue big;
	big.type = FieldType::Float;
	big.num = 1e300;
	CHECK_FALSE(net::IsSendableFieldValue(big));

	// Strings ride a u32 length prefix the reader caps at 64 KiB; the boundary
	// itself must stay sendable or the cap would eat a legal value.
	FieldValue s;
	s.type = FieldType::String;
	s.str = std::string(64u * 1024u, 'x');
	CHECK(net::IsSendableFieldValue(s));
	s.str.push_back('x');
	CHECK_FALSE(net::IsSendableFieldValue(s));
}

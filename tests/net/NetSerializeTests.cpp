#include <doctest/doctest.h>

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

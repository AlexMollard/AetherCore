#include <doctest/doctest.h>

#include <cstring>
#include <stdexcept>

#include "scripting/CSharpGameplayHost.hpp"

namespace
{
	using namespace aether::app::scripting;
	using namespace aether::scripting;

	struct FakeManaged
	{
		static inline std::string text;
		static inline std::uint64_t next = 100;
		static inline int attached = 0;
		static inline int updated = 0;
		static inline int detached = 0;
		static inline int destroyed = 0;
		static inline std::uint32_t owner = 0;
		static inline int isOwner = 0;
		static inline bool throws = false;

		static int Write(const char* value, char* out, int capacity)
		{
			const int size = static_cast<int>(std::strlen(value));
			if (capacity > size)
			{
				std::memcpy(out, value, size + 1);
				return size;
			}
			return 0;
		}

		static ManagedScriptApi Api()
		{
			ManagedScriptApi api{};
			api.GetScriptTypeCount = []
			{
				return 2;
			};
			api.GetScriptTypeName = [](int i, char* out, int cap)
			{
				return Write(i == 0 ? "Mover" : "Other", out, cap);
			};
			api.GetPropertyCount = [](const char*)
			{
				return 8;
			};
			api.GetPropertyInfo = [](const char*, int i, char* out, int cap, int* type)
			{
				*type = i + 1;
				return Write(("Field" + std::to_string(i)).c_str(), out, cap);
			};
			api.CreateInstance = [](const char*, std::uint32_t) -> std::uint64_t
			{
				return ++next;
			};
			api.DestroyInstance = [](std::uint64_t)
			{
				++destroyed;
			};
			api.InvokeAttach = [](std::uint64_t)
			{
				++attached;
				if (throws)
				{
					throw std::runtime_error("fake managed failure");
				}
			};
			api.InvokeUpdate = [](std::uint64_t, float)
			{
				++updated;
			};
			api.InvokeDetach = [](std::uint64_t)
			{
				++detached;
			};
			api.InvokeOwnershipChanged = [](std::uint64_t, std::uint32_t value, int local)
			{
				owner = value;
				isOwner = local;
			};
			api.GetProperty = [](std::uint64_t, int index, PropertyValue* value)
			{
				value->type = static_cast<PropertyType>(index + 1);
				value->f4[0] = 3.25f;
				value->f4[1] = 2.0f;
				value->i64 = 0xf1234567;
				value->str = text.data();
				value->reserved = static_cast<int>(text.size());
				return 1;
			};
			api.GetDefaultProperty = [](const char*, int index, PropertyValue* value)
			{
				return Api().GetProperty(0, index, value);
			};
			api.SetProperty = [](std::uint64_t, int, const PropertyValue* value)
			{
				text.assign(value->str, value->reserved);
				return 1;
			};
			return api;
		}
	};
} // namespace

TEST_CASE("CSharp gameplay adapter routes lifecycle through v1 and rejects stale handles")
{
	FakeManaged::attached = FakeManaged::updated = FakeManaged::detached = FakeManaged::destroyed = 0;
	FakeManaged::throws = false;
	auto api = FakeManaged::Api();
	CSharpGameplayHost adapter;
	REQUIRE(adapter.Reset(&api, 1));
	auto& host = adapter.Gameplay();
	CHECK(host.Language() == "csharp");
	CHECK(host.GetScriptTypeNames().size() == 2);
	CHECK(host.FindPropertyIndex("Mover", "Field4") == 4);
	CHECK_FALSE(host.CreateInstance("Missing", 1));
	auto first = host.CreateInstance("Mover", 7);
	auto second = host.CreateInstance("Other", 8);
	REQUIRE(first);
	REQUIRE(second);
	CHECK(*first != *second);
	CHECK(host.Attach(*first));
	CHECK(host.Update(*first, 0.25f));
	CHECK(host.DispatchOwnershipChanged(*first, 19, true));
	CHECK(FakeManaged::owner == 19);
	CHECK(FakeManaged::isOwner == 1);
	CHECK(host.Detach(*first));
	CHECK(host.Destroy(*first));
	CHECK_FALSE(host.Update(*first, 0.25f));
	CHECK(FakeManaged::attached == 1);
	CHECK(FakeManaged::updated == 1);
	CHECK(FakeManaged::detached == 1);
	CHECK(FakeManaged::destroyed == 1);
	REQUIRE(adapter.Reset(&api, 2));
	CHECK_FALSE(host.Attach(*second));
	CHECK(host.ScriptReloadGeneration() == 2);
	CHECK_FALSE(adapter.Reset(nullptr, 3));
	CHECK_FALSE(host.IsAvailable());
	CHECK(host.GetScriptTypeNames().empty());
	CHECK(host.ScriptReloadGeneration() == 3);
}

TEST_CASE("CSharp gameplay adapter preserves every property kind and owned UTF8 bytes")
{
	auto api = FakeManaged::Api();
	CSharpGameplayHost adapter;
	REQUIRE(adapter.Reset(&api, 1));
	auto& host = adapter.Gameplay();
	auto instance = host.CreateInstance("Mover", 2);
	REQUIRE(instance);
	FakeManaged::text = std::string("left\0right", 10) + std::string(700, 'x');
	const auto expected = FakeManaged::text;
	for (int index = 0; index < 8; ++index)
	{
		aether::ScriptPropertyValue value;
		REQUIRE(host.GetPropertyValue(*instance, index, value));
		CHECK(static_cast<int>(value.type) == index + 1);
		CHECK(value.str == expected);
		if (index == 6 || index == 7)
		{
			CHECK(value.i64 == 0xf1234567);
		}
		aether::ScriptPropertyValue defaultValue;
		REQUIRE(host.GetDefaultPropertyValue("Mover", index, defaultValue));
		CHECK(defaultValue.str == expected);
		host.SetPropertyValue(*instance, index, value);
		CHECK(FakeManaged::text == expected);
	}
	aether::ScriptPropertyValue retained;
	REQUIRE(host.GetPropertyValue(*instance, 4, retained));
	FakeManaged::text = "new scratch data";
	CHECK(retained.str == expected);
	CHECK_FALSE(host.GetDefaultPropertyValue("Missing", 0, retained));
}

TEST_CASE("CSharp gameplay adapter contains exceptions and advertises only supported events")
{
	auto api = FakeManaged::Api();
	api.InvokeOwnershipChanged = nullptr;
	CSharpGameplayHost adapter;
	REQUIRE(adapter.Reset(&api, 1));
	auto& host = adapter.Gameplay();
	CHECK(host.Capabilities() == 0);
	auto instance = host.CreateInstance("Mover", 2);
	REQUIRE(instance);
	CHECK_FALSE(host.DispatchOwnershipChanged(*instance, 0, true));
	FakeManaged::throws = true;
	CHECK_FALSE(host.Attach(*instance));
	FakeManaged::throws = false;
	CHECK(host.Destroy(*instance));
}

TEST_CASE("CSharp gameplay adapter load is metadata-only and property failures keep output intact")
{
	auto api = FakeManaged::Api();
	static int defaultReads;
	defaultReads = 0;
	api.GetDefaultProperty = [](const char*, int, PropertyValue*)
	{
		++defaultReads;
		return 0;
	};
	CSharpGameplayHost adapter;
	REQUIRE(adapter.Reset(&api, 17));
	CHECK(defaultReads == 0);
	aether::ScriptPropertyValue output;
	output.str = "untouched";
	CHECK_FALSE(adapter.Gameplay().GetDefaultPropertyValue("Mover", 0, output));
	CHECK(defaultReads == 1);
	CHECK(output.str == "untouched");
	std::vector<std::string> diagnostics;
	adapter.Gameplay().SetErrorHandler([&](const std::string& message) { diagnostics.push_back(message); });
	CHECK_FALSE(adapter.Gameplay().CreateInstance("Absent", 1));
	REQUIRE(diagnostics.size() == 1);
	CHECK(diagnostics.front().find("Absent") != std::string::npos);
	api.GetProperty = nullptr;
	CHECK_FALSE(adapter.Reset(&api, 18));
	CHECK_FALSE(adapter.Gameplay().IsAvailable());
	CHECK(adapter.Gameplay().GetScriptTypeNames().empty());
}

TEST_CASE("CSharp gameplay adapter observes errors reported by legacy void callbacks")
{
	auto api = FakeManaged::Api();
	static CSharpGameplayHost* current;
	CSharpGameplayHost adapter;
	current = &adapter;
	api.InvokeAttach = [](std::uint64_t)
	{
		current->ReportManagedError();
	};
	REQUIRE(adapter.Reset(&api, 1));
	const auto instance = adapter.Gameplay().CreateInstance("Mover", 1);
	REQUIRE(instance);
	CHECK_FALSE(adapter.Gameplay().Attach(*instance));
	CHECK(adapter.Gameplay().Destroy(*instance));
}

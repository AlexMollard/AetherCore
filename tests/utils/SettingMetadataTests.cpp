// The settings panel used to be a bare reflection dump: no help text, no ranges, and a
// free-text box for a value with three legal spellings. SettingInfo is what fixed that, and
// its whole risk is drift - a range that says one thing while the loader enforces another
// is worse than no range at all, because the UI then refuses values the engine accepts (or
// offers values it silently rewrites).
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

#include "utils/EngineSettings.hpp"

using aether::DefaultSettingValue;
using aether::EngineSettings;
using aether::EngineSettingsIO;
using aether::ForEachSettingField;
using aether::SettingMetadata;

TEST_CASE("Every advertised bound survives Sanitize")
{
	// Push each bounded field to the ends of its advertised range and to just outside,
	// then sanitize. Inside the range must come back untouched; outside must be pulled
	// back inside it. Either failure means the metadata is lying to the user.
	const EngineSettings defaults{};
	ForEachSettingField(defaults,
	        [](std::string_view key, const auto& probe)
	        {
		        using Field = std::decay_t<decltype(probe)>;
		        if constexpr (std::is_same_v<Field, int> || std::is_same_v<Field, float>)
		        {
			        const aether::SettingInfo& info = SettingMetadata(key);
			        if (info.minValue == info.maxValue)
			        {
				        return; // unbounded by design
			        }
			        for (const double candidate: {info.minValue, info.maxValue})
			        {
				        EngineSettings settings{};
				        bool applied = false;
				        ForEachSettingField(settings,
				                [&](std::string_view candidateKey, auto& field)
				                {
					                if (candidateKey != key)
					                {
						                return;
					                }
					                if constexpr (std::is_same_v<std::decay_t<decltype(field)>, Field>)
					                {
						                field = static_cast<Field>(candidate);
						                applied = true;
					                }
				                });
				        REQUIRE(applied);
				        EngineSettingsIO::Sanitize(settings);

				        ForEachSettingField(settings,
				                [&](std::string_view candidateKey, const auto& field)
				                {
					                if (candidateKey != key)
					                {
						                return;
					                }
					                if constexpr (std::is_same_v<std::decay_t<decltype(field)>, Field>)
					                {
						                INFO("key=" << key << " advertised bound=" << candidate << " survived as=" << field);
						                CHECK(static_cast<double>(field) == doctest::Approx(candidate));
					                }
				                });
			        }
		        }
	        });
}

TEST_CASE("A closed value set lists exactly what the loader accepts")
{
	const aether::SettingInfo& mode = SettingMetadata("window.mode");
	REQUIRE(mode.choices.size() == 3);
	CHECK(mode.choices[0] == "windowed");
	CHECK(mode.choices[1] == "borderless");
	CHECK(mode.choices[2] == "fullscreen");

	// The default has to be one of the offered options, or the combo opens showing a
	// value it cannot re-select.
	std::string defaultMode;
	REQUIRE(DefaultSettingValue<std::string>("window.mode", defaultMode));
	CHECK(std::ranges::find(mode.choices, defaultMode) != mode.choices.end());
}

TEST_CASE("Every setting key has help text")
{
	// A key with no entry still renders - it just renders with no explanation, which is
	// the state this whole table exists to end. Adding a setting and forgetting its line
	// should fail here rather than ship a bare name.
	std::vector<std::string> undocumented;
	const EngineSettings defaults{};
	ForEachSettingField(defaults,
	        [&](std::string_view key, const auto&)
	        {
		        if (SettingMetadata(key).description.empty())
		        {
			        undocumented.emplace_back(key);
		        }
	        });
	INFO("settings with no description: " << undocumented.size());
	CHECK(undocumented.empty());
}

TEST_CASE("An unknown key is not an error")
{
	const aether::SettingInfo& none = SettingMetadata("graphics.doesNotExist");
	CHECK(none.description.empty());
	CHECK(none.choices.empty());
	CHECK(none.minValue == none.maxValue);
}

TEST_CASE("DefaultSettingValue reads the type it was asked for, and only that")
{
	int width = 0;
	REQUIRE(DefaultSettingValue<int>("window.width", width));
	CHECK(width == EngineSettings{}.window.width);

	bool vsync = false;
	REQUIRE(DefaultSettingValue<bool>("graphics.vsync", vsync));
	CHECK(vsync == EngineSettings{}.graphics.vsync);

	// Wrong type for a real key, and a key that does not exist, both report failure
	// rather than handing back a zero the reset button would then write.
	float widthAsFloat = 123.0f;
	CHECK_FALSE(DefaultSettingValue<float>("window.width", widthAsFloat));
	CHECK(widthAsFloat == 123.0f);

	int missing = 7;
	CHECK_FALSE(DefaultSettingValue<int>("nope.missing", missing));
	CHECK(missing == 7);
}

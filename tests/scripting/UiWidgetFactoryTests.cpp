// Regression coverage for the missing Ui.CreateSlider/CreateToggle/CreateProgressBar
// factory gap: UISlider/UIToggle/UIProgressBar (and their value/range accessors) were
// already fully implemented at the engine level - CreateSliderEntity/CreateToggleEntity/
// CreateProgressBarEntity existed in ui/UiEntities.cpp - but there was no way to actually
// spawn one from a script: no native export, no managed wrapper. These tests call the
// exported C-ABI functions directly (extern "C", so no CoreCLR host is needed), exactly
// like PhysicsExportsTests.cpp/CharacterExportsTests.cpp already do.
#include <doctest/doctest.h>

#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "scripting/interop/InteropCommon.hpp"
#include "ui/UiComponents.hpp"

extern "C" std::uint32_t aether_ui_create_canvas();
extern "C" std::uint32_t aether_ui_create_image(std::uint32_t canvasId);
extern "C" void aether_ui_set_clip(std::uint32_t id, std::int32_t enabled, float padding);
extern "C" std::uint32_t aether_ui_create_slider(std::uint32_t canvasId);
extern "C" void aether_ui_set_slider_range(std::uint32_t id, float minValue, float maxValue, float step);
extern "C" float aether_ui_get_slider_value(std::uint32_t id);
extern "C" void aether_ui_set_slider_value(std::uint32_t id, float value);
extern "C" std::uint32_t aether_ui_create_toggle(std::uint32_t canvasId);
extern "C" std::int32_t aether_ui_get_toggle(std::uint32_t id);
extern "C" void aether_ui_set_toggle(std::uint32_t id, std::int32_t on);
extern "C" std::uint32_t aether_ui_create_progress_bar(std::uint32_t canvasId);
extern "C" void aether_ui_set_progress(std::uint32_t id, float value);
extern "C" float aether_ui_get_progress(std::uint32_t id);

namespace
{
	struct UiFactoryFixture
	{
		aether::World world;
		aether::app::scripting::SceneContext ctx;
		aether::app::scripting::ActiveContextScope scope;

		UiFactoryFixture()
		      : scope(ctx)
		{
			ctx.world = &world;
		}
	};
} // namespace

TEST_CASE("Ui.CreateSlider spawns an entity carrying UISlider, usable through the existing value/range accessors")
{
	UiFactoryFixture fx;
	const std::uint32_t canvas = aether_ui_create_canvas();
	const std::uint32_t slider = aether_ui_create_slider(canvas);

	REQUIRE(slider != 0);
	REQUIRE(fx.world.TryGet<aether::ui::UISlider>(aether::Entity{slider}) != nullptr);
	// Composes UISelectable for focus, per UISlider's own doc comment.
	CHECK(fx.world.TryGet<aether::ui::UISelectable>(aether::Entity{slider}) != nullptr);

	aether_ui_set_slider_range(slider, 0.0f, 100.0f, 1.0f);
	aether_ui_set_slider_value(slider, 40.0f);
	CHECK(aether_ui_get_slider_value(slider) == doctest::Approx(40.0f));

	// Range change clamps the CURRENT value too, not just future writes.
	aether_ui_set_slider_range(slider, 0.0f, 10.0f, 1.0f);
	CHECK(aether_ui_get_slider_value(slider) <= 10.0f);
}

TEST_CASE("Ui.CreateToggle spawns an entity carrying UIToggle, usable through the existing get/set accessors")
{
	UiFactoryFixture fx;
	const std::uint32_t canvas = aether_ui_create_canvas();
	const std::uint32_t toggle = aether_ui_create_toggle(canvas);

	REQUIRE(toggle != 0);
	REQUIRE(fx.world.TryGet<aether::ui::UIToggle>(aether::Entity{toggle}) != nullptr);

	CHECK(aether_ui_get_toggle(toggle) == 0);
	aether_ui_set_toggle(toggle, 1);
	CHECK(aether_ui_get_toggle(toggle) != 0);
}

TEST_CASE("Ui.CreateProgressBar spawns an entity carrying UIProgressBar, with no UISelectable - it is read-only")
{
	UiFactoryFixture fx;
	const std::uint32_t canvas = aether_ui_create_canvas();
	const std::uint32_t bar = aether_ui_create_progress_bar(canvas);

	REQUIRE(bar != 0);
	REQUIRE(fx.world.TryGet<aether::ui::UIProgressBar>(aether::Entity{bar}) != nullptr);
	CHECK(fx.world.TryGet<aether::ui::UISelectable>(aether::Entity{bar}) == nullptr);

	aether_ui_set_progress(bar, 0.75f);
	CHECK(aether_ui_get_progress(bar) == doctest::Approx(0.75f));
}

TEST_CASE("Ui.SetClip attaches and detaches UIMask on an element that already has a UIRect")
{
	// UIMask was fully implemented and consumed by UiDrawBuilder already - clipping a
	// scrollable list's overflow - but had no script entry point at all, the same
	// "feature with no entry point" shape as Slider/Toggle/ProgressBar's missing
	// create functions.
	UiFactoryFixture fx;
	const std::uint32_t canvas = aether_ui_create_canvas();
	const std::uint32_t image = aether_ui_create_image(canvas);
	REQUIRE(fx.world.TryGet<aether::ui::UIRect>(aether::Entity{image}) != nullptr);
	REQUIRE(fx.world.TryGet<aether::ui::UIMask>(aether::Entity{image}) == nullptr);

	aether_ui_set_clip(image, 1, 4.0f);
	const auto* mask = fx.world.TryGet<aether::ui::UIMask>(aether::Entity{image});
	REQUIRE(mask != nullptr);
	CHECK(mask->padding == doctest::Approx(4.0f));

	aether_ui_set_clip(image, 0, 0.0f);
	CHECK(fx.world.TryGet<aether::ui::UIMask>(aether::Entity{image}) == nullptr);
}

TEST_CASE("Ui.SetClip is a no-op on an entity with no UIRect - never clips against nothing")
{
	UiFactoryFixture fx;
	const aether::Entity bare = fx.world.Create();
	REQUIRE(fx.world.TryGet<aether::ui::UIRect>(bare) == nullptr);

	aether_ui_set_clip(bare.id, 1, 0.0f);
	CHECK(fx.world.TryGet<aether::ui::UIMask>(bare) == nullptr);
}

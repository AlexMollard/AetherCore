#include <doctest/doctest.h>

#include "utils/EngineSettings.hpp"
#include "utils/ServiceContainer.hpp"
#include "utils/SettingsService.hpp"

using namespace aether;

// These tests exercise the service's value/dirty plumbing and its null-safety
// contract. Live dispatch to concrete subsystems (Renderer, AetherCore, Window)
// is covered by the build + integration, not here, since those are not fakeable
// through the type-keyed service locator.

TEST_CASE("SettingsService exposes the values it was constructed with") {
    EngineSettings values;
    values.graphics.fxaa = true;
    values.window.width = 2560;
    EngineSettings base; // defaults

    ServiceContainer services;
    SettingsService service(values, base, services);

    CHECK(service.Get().graphics.fxaa == true);
    CHECK(service.Get().window.width == 2560);
    CHECK(service.Base().window.width == 1280);
}

TEST_CASE("ApplyField marks the settings dirty") {
    EngineSettings values;
    EngineSettings base;
    ServiceContainer services;
    SettingsService service(values, base, services);

    CHECK(service.IsDirty() == false);

    service.Values().graphics.fxaa = true;
    service.ApplyField("graphics.fxaa"); // no Renderer registered -> apply is a no-op
    CHECK(service.IsDirty() == true);
}

TEST_CASE("ApplyAll is a safe no-op when no subsystems are registered") {
    EngineSettings values;
    values.graphics.fxaa = true;
    values.app.targetFps = 120.0f;
    values.window.width = 3440;
    EngineSettings base;

    ServiceContainer services; // empty: every TryGet returns nullptr

    SettingsService service(values, base, services);
    service.ApplyAll(); // must not crash / dereference a missing subsystem

    // Values are unchanged; ApplyAll only pushes outward.
    CHECK(service.Get().graphics.fxaa == true);
    CHECK(service.Get().app.targetFps == doctest::Approx(120.0f));
}

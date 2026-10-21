# This branch intentionally precedes desktop Dependencies/DotNet/SlangShaders.
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "AETHERCORE_WEB_RUNTIME requires the pinned Emscripten toolchain")
endif()
option(AETHERCORE_WEB_TEST_BRIDGE "Include development-only browser automation" OFF)
option(AETHERCORE_WEB_MANAGED "Build the experimental managed-owned browser runtime archive" OFF)
enable_language(C)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --version OUTPUT_VARIABLE _em_version RESULT_VARIABLE _em_exit)
file(READ "${CMAKE_SOURCE_DIR}/tools/experiments/rust-webgpu/toolchain.json" _pins)
string(JSON _emscripten_pin GET "${_pins}" emscripten)
string(FIND "${_em_version}" "${_emscripten_pin}" _pin_offset)
if(NOT _em_exit EQUAL 0 OR _pin_offset EQUAL -1)
    message(FATAL_ERROR "Browser runtime requires Emscripten ${_emscripten_pin}; got ${_em_version}")
endif()
set(AETHERCORE_BUILD_EDITOR OFF CACHE BOOL "Browser excludes editor" FORCE)
set(AETHERCORE_BUILD_LAUNCHER OFF CACHE BOOL "Browser excludes launcher" FORCE)
set(AETHERCORE_BUILD_TESTS OFF CACHE BOOL "Native tests are host executables" FORCE)
set(AETHER_WEB_ASSETPACKER "" CACHE FILEPATH "Verified native AssetPacker executable")
set(AETHER_WEB_SLANGC "" CACHE FILEPATH "Verified native Slang 2026.8 executable")
foreach(_kind IN ITEMS ASSETPACKER SLANGC)
    if(_kind STREQUAL "ASSETPACKER")
        set(_tool_kind assetpacker)
    else()
        set(_tool_kind slang)
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/audit.py"
        tool "${AETHER_WEB_${_kind}}" --kind "${_tool_kind}" RESULT_VARIABLE _tool_exit OUTPUT_VARIABLE _tool_output ERROR_VARIABLE _tool_error)
    if(NOT _tool_exit EQUAL 0)
        message(FATAL_ERROR "Native ${_kind} preflight failed: ${_tool_output}${_tool_error}")
    endif()
endforeach()

include(get_cpm)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
include(PortableDependencies)
if(NOT AETHERCORE_WEB_MANAGED)
    find_program(AETHER_CARGO cargo REQUIRED NO_CMAKE_FIND_ROOT_PATH)
endif()
find_program(AETHER_WEB_NODE node REQUIRED NO_CMAKE_FIND_ROOT_PATH)
string(JSON _rust_pin GET "${_pins}" rustWasm)
set(AETHER_WEB_PROJECT "" CACHE PATH "Selected Rust project root (empty uses qualification fixture)")
set(AETHER_WEB_STAGED_PROJECT "" CACHE PATH "Preflight-validated selected project assets")
set(AETHER_WEB_GAMEPLAY_MANIFEST "" CACHE FILEPATH "Selected Cargo manifest")
set(AETHER_WEB_GAMEPLAY_NAME "" CACHE STRING "Selected Rust library name")
set(AETHER_WEB_MANAGED_PROJECT "" CACHE FILEPATH "Selected C# gameplay project")
if(AETHERCORE_WEB_MANAGED AND (AETHERCORE_WEB_TEST_BRIDGE OR NOT EXISTS "${AETHER_WEB_MANAGED_PROJECT}"))
    message(FATAL_ERROR "Managed Web exports require a selected C# project and automation OFF")
endif()
if(AETHER_WEB_PROJECT AND NOT AETHERCORE_WEB_MANAGED)
    if(AETHERCORE_WEB_TEST_BRIDGE OR NOT AETHER_WEB_GAMEPLAY_MANIFEST OR NOT AETHER_WEB_STAGED_PROJECT OR NOT AETHER_WEB_GAMEPLAY_NAME MATCHES "^[A-Za-z0-9_]+$")
        message(FATAL_ERROR "Project Web exports require validated inputs and automation OFF")
    endif()
    set(_rust_library "${CMAKE_BINARY_DIR}/rust/wasm32-unknown-emscripten/release/lib${AETHER_WEB_GAMEPLAY_NAME}.a")
    file(GLOB_RECURSE _project_rust CONFIGURE_DEPENDS "${AETHER_WEB_PROJECT}/*.rs" "${AETHER_WEB_PROJECT}/scripts/Cargo.*")
    file(GLOB_RECURSE _rust_sdk CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/rust/*.rs" "${CMAKE_SOURCE_DIR}/rust/*/Cargo.toml")
    add_custom_command(OUTPUT "${_rust_library}"
        COMMAND "${CMAKE_COMMAND}" -E env CARGO_PROFILE_RELEASE_PANIC=abort "${AETHER_CARGO}" "+${_rust_pin}" rustc -Zbuild-std=std,panic_abort
            --manifest-path "${AETHER_WEB_GAMEPLAY_MANIFEST}" --release --target wasm32-unknown-emscripten
            --target-dir "${CMAKE_BINARY_DIR}/rust" --lib --crate-type staticlib
        DEPENDS ${_project_rust} ${_rust_sdk} "${AETHER_WEB_GAMEPLAY_MANIFEST}"
        VERBATIM)
elseif(NOT AETHERCORE_WEB_MANAGED)
set(_rust_library "${CMAKE_BINARY_DIR}/rust/wasm32-unknown-emscripten/release/libaether_browser_gameplay.a")
file(GLOB_RECURSE _rust_sdk CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/rust/*.rs" "${CMAKE_SOURCE_DIR}/rust/*/Cargo.toml")
if(AETHERCORE_WEB_TEST_BRIDGE)
    set(_rust_test_features --features automation)
endif()
add_custom_command(OUTPUT "${_rust_library}"
    COMMAND "${AETHER_CARGO}" "+${_rust_pin}" build -Zbuild-std=std,panic_abort
        --manifest-path "${CMAKE_SOURCE_DIR}/tools/web-runtime/gameplay/Cargo.toml" --locked ${_rust_test_features}
        --release --target wasm32-unknown-emscripten --target-dir "${CMAKE_BINARY_DIR}/rust"
    DEPENDS ${_rust_sdk} "${CMAKE_SOURCE_DIR}/tools/web-runtime/gameplay/Cargo.toml"
        "${CMAKE_SOURCE_DIR}/tools/web-runtime/gameplay/Cargo.lock" "${CMAKE_SOURCE_DIR}/tools/web-runtime/gameplay/src/lib.rs"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/scripts/src/reference.rs"
        "${CMAKE_SOURCE_DIR}/projects/EmberRun/scripts/src/game.rs"
    VERBATIM)
endif()
if(NOT AETHERCORE_WEB_MANAGED)
    add_custom_target(WebGameplay DEPENDS "${_rust_library}")
endif()
add_subdirectory(src/engine)
add_subdirectory(src/app)
if(NOT AETHERCORE_WEB_MANAGED)
    add_dependencies(GameRuntime WebGameplay)
    target_link_libraries(GameRuntime PRIVATE "${_rust_library}")
endif()
if(AETHERCORE_WEB_TEST_BRIDGE)
    target_link_options(GameRuntime PRIVATE --emit-symbol-map)
endif()
target_compile_definitions(Engine PUBLIC AE_WEB_TEST_BRIDGE=$<BOOL:${AETHERCORE_WEB_TEST_BRIDGE}>)
set(AETHER_WEB_BRIDGE_SCRIPT "")
if(AETHERCORE_WEB_TEST_BRIDGE)
    configure_file("${CMAKE_SOURCE_DIR}/resources/web/test-bridge.js" "${CMAKE_BINARY_DIR}/web/test-bridge.js" COPYONLY)
    set(AETHER_WEB_BRIDGE_SCRIPT "<script src=\"test-bridge.js\"></script>")
else()
    file(REMOVE "${CMAKE_BINARY_DIR}/web/test-bridge.js" "${CMAKE_BINARY_DIR}/web/AetherGame.js.symbols")
endif()
configure_file("${CMAKE_SOURCE_DIR}/resources/web/index.html" "${CMAKE_BINARY_DIR}/web/index.html" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/resources/web/platform.js" "${CMAKE_BINARY_DIR}/web/platform.js" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/resources/branding/aethercore-app-icon.png" "${CMAKE_BINARY_DIR}/web/aethercore-icon.png" COPYONLY)

file(WRITE "${CMAKE_BINARY_DIR}/web/project.js" "globalThis.aetherProject=null;\n")
set(_assets "${CMAKE_BINARY_DIR}/web-assets")
file(MAKE_DIRECTORY "${_assets}" "${CMAKE_BINARY_DIR}/test-fixtures")
if(NOT AETHER_WEB_PROJECT)
add_custom_command(OUTPUT "${_assets}/reference.pak"
    COMMAND "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets" "${_assets}/reference.pak"
    DEPENDS "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/prepare_reference.mjs" ${_game_sources}
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/ProjectSettings.toml"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/audio/reference.wav"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/textures/reference.png"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/prefabs/ReferencePickup.prefab.toml"
    VERBATIM)
endif()
foreach(_shader IN ITEMS sprite mesh)
    add_custom_command(OUTPUT "${_assets}/${_shader}.wgsl"
        COMMAND "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/resources/web/${_shader}.slang"
            -target wgsl -entry vertexMain -entry fragmentMain -o "${_assets}/${_shader}.wgsl"
        DEPENDS "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/resources/web/${_shader}.slang"
        VERBATIM)
endforeach()
add_custom_target(WebAssets DEPENDS "${_assets}/sprite.wgsl" "${_assets}/mesh.wgsl")
if(NOT AETHER_WEB_PROJECT)
    add_custom_target(WebFixtureAssets DEPENDS "${_assets}/reference.pak")
    add_dependencies(WebAssets WebFixtureAssets)
endif()
add_dependencies(GameRuntime WebAssets)

if(AETHER_WEB_PROJECT)
    file(GLOB_RECURSE _game_sources CONFIGURE_DEPENDS "${AETHER_WEB_STAGED_PROJECT}/*")
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/web/game.pak"
        COMMAND "${AETHER_WEB_ASSETPACKER}" --project --target webgpu-2d-v1 "${AETHER_WEB_STAGED_PROJECT}" "${CMAKE_BINARY_DIR}/web/game.pak"
        DEPENDS "${AETHER_WEB_ASSETPACKER}" ${_game_sources}
        VERBATIM)
else()
file(GLOB_RECURSE _game_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/*" "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/scenes/*" "${CMAKE_SOURCE_DIR}/resources/web/reference/*" "${CMAKE_SOURCE_DIR}/projects/EmberRun/assets/ember/*" "${CMAKE_SOURCE_DIR}/projects/EmberRun/scenes/EmberRun.scene.toml")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/web/game.pak"
    COMMAND "${AETHER_WEB_NODE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/prepare_reference.mjs" "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust" "${CMAKE_BINARY_DIR}"
    COMMAND "${AETHER_WEB_ASSETPACKER}" --project --target webgpu-2d-v1 "${CMAKE_BINARY_DIR}/reference-project"
        "${CMAKE_BINARY_DIR}/web/game.pak"
    COMMAND "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets"
        "${CMAKE_BINARY_DIR}/test-fixtures/desktop-reference.pak"
    DEPENDS "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/prepare_reference.mjs" ${_game_sources}
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/ProjectSettings.toml"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/audio/reference.wav"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/textures/reference.png"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/prefabs/ReferencePickup.prefab.toml"
    VERBATIM)
endif()
add_custom_target(WebGameAssets DEPENDS "${CMAKE_BINARY_DIR}/web/game.pak")
add_dependencies(GameRuntime WebGameAssets)

file(GLOB _font_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/fonts/*")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/web/engine.pak"
    COMMAND "${AETHER_WEB_ASSETPACKER}" --target webgpu-2d-v1
        "${CMAKE_SOURCE_DIR}/resources/fonts" "${CMAKE_BINARY_DIR}/web/engine.pak"
    DEPENDS "${AETHER_WEB_ASSETPACKER}" ${_font_sources}
    VERBATIM)
add_custom_target(WebEngineAssets DEPENDS "${CMAKE_BINARY_DIR}/web/engine.pak")
add_dependencies(GameRuntime WebEngineAssets)
if(TARGET WebManagedRuntime)
    add_dependencies(WebManagedRuntime WebAssets WebGameAssets WebEngineAssets)
endif()
if(NOT AETHERCORE_WEB_MANAGED)
foreach(_shader IN ITEMS sprite mesh)
    set_property(TARGET GameRuntime APPEND PROPERTY LINK_DEPENDS "${_assets}/${_shader}.wgsl")
    target_link_options(GameRuntime PRIVATE "--preload-file=${_assets}/${_shader}.wgsl@/assets/${_shader}.wgsl")
endforeach()
if(NOT AETHER_WEB_PROJECT)
    set_property(TARGET GameRuntime APPEND PROPERTY LINK_DEPENDS "${_assets}/reference.pak")
    target_link_options(GameRuntime PRIVATE "--preload-file=${_assets}/reference.pak@/assets/reference.pak")
endif()
add_custom_target(WebRuntimeAudit ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/audit.py" wasm
        "$<TARGET_FILE_DIR:GameRuntime>/AetherGame.wasm" --report "${CMAKE_BINARY_DIR}/web-imports.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/audit.py" sources
        "${CMAKE_BINARY_DIR}/compile_commands.json" --report "${CMAKE_BINARY_DIR}/web-sources.json"
    DEPENDS GameRuntime
    VERBATIM)
else()
add_custom_target(WebRuntimeAudit ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/audit.py" sources
        "${CMAKE_BINARY_DIR}/compile_commands.json" --report "${CMAKE_BINARY_DIR}/web-sources.json"
    DEPENDS GameRuntime VERBATIM)
endif()

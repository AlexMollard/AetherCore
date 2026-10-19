# This branch intentionally precedes desktop Dependencies/DotNet/SlangShaders.
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "AETHERCORE_WEB_RUNTIME requires the pinned Emscripten toolchain")
endif()
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
find_program(AETHER_CARGO cargo REQUIRED NO_CMAKE_FIND_ROOT_PATH)
find_program(AETHER_WEB_NODE node REQUIRED NO_CMAKE_FIND_ROOT_PATH)
string(JSON _rust_pin GET "${_pins}" rustWasm)
set(_rust_library "${CMAKE_BINARY_DIR}/rust/wasm32-unknown-emscripten/release/libaether_browser_gameplay.a")
file(GLOB_RECURSE _rust_sdk CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/rust/*.rs" "${CMAKE_SOURCE_DIR}/rust/*/Cargo.toml")
add_custom_command(OUTPUT "${_rust_library}"
    COMMAND "${AETHER_CARGO}" "+${_rust_pin}" build -Zbuild-std=std,panic_abort
        --manifest-path "${CMAKE_SOURCE_DIR}/tools/web-runtime/gameplay/Cargo.toml" --locked
        --release --target wasm32-unknown-emscripten --target-dir "${CMAKE_BINARY_DIR}/rust"
    DEPENDS ${_rust_sdk} "${CMAKE_SOURCE_DIR}/tools/web-runtime/gameplay/Cargo.toml"
        "${CMAKE_SOURCE_DIR}/tools/web-runtime/gameplay/Cargo.lock" "${CMAKE_SOURCE_DIR}/tools/web-runtime/gameplay/src/lib.rs"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/scripts/src/reference.rs"
    VERBATIM)
add_custom_target(WebGameplay DEPENDS "${_rust_library}")
add_subdirectory(src/engine)
add_subdirectory(src/app)
add_dependencies(GameRuntime WebGameplay)
target_link_libraries(GameRuntime PRIVATE "${_rust_library}")
configure_file("${CMAKE_SOURCE_DIR}/resources/web/index.html" "${CMAKE_BINARY_DIR}/web/index.html" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/resources/web/platform.js" "${CMAKE_BINARY_DIR}/web/platform.js" COPYONLY)

set(_assets "${CMAKE_BINARY_DIR}/web-assets")
file(MAKE_DIRECTORY "${_assets}" "${CMAKE_BINARY_DIR}/test-fixtures")
add_custom_command(OUTPUT "${_assets}/reference.pak"
    COMMAND "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets" "${_assets}/reference.pak"
    DEPENDS "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/prepare_reference.mjs" ${_game_sources}
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/ProjectSettings.toml"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/audio/reference.wav"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/textures/reference.png"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/prefabs/ReferencePickup.prefab.toml"
    VERBATIM)
add_custom_command(OUTPUT "${_assets}/sprite.wgsl"
    COMMAND "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/resources/web/sprite.slang"
        -target wgsl -entry vertexMain -entry fragmentMain -o "${_assets}/sprite.wgsl"
    DEPENDS "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/resources/web/sprite.slang"
    VERBATIM)
add_custom_target(WebAssets DEPENDS "${_assets}/reference.pak" "${_assets}/sprite.wgsl")
add_dependencies(GameRuntime WebAssets)

file(GLOB_RECURSE _game_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/*" "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/scenes/*" "${CMAKE_SOURCE_DIR}/resources/web/reference/*")
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
set_property(TARGET GameRuntime APPEND PROPERTY LINK_DEPENDS
    "${_assets}/reference.pak" "${_assets}/sprite.wgsl")
target_link_options(GameRuntime PRIVATE
    "--preload-file=${_assets}/reference.pak@/assets/reference.pak"
    "--preload-file=${_assets}/sprite.wgsl@/assets/sprite.wgsl")
add_custom_target(WebRuntimeAudit ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/audit.py" wasm
        "$<TARGET_FILE_DIR:GameRuntime>/AetherGame.wasm" --report "${CMAKE_BINARY_DIR}/web-imports.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/audit.py" sources
        "${CMAKE_BINARY_DIR}/compile_commands.json" --report "${CMAKE_BINARY_DIR}/web-sources.json"
    DEPENDS GameRuntime
    VERBATIM)

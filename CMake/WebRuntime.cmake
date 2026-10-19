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
add_subdirectory(src/engine)
add_subdirectory(src/app)

set(_assets "${CMAKE_BINARY_DIR}/web-assets")
file(MAKE_DIRECTORY "${_assets}")
add_custom_command(OUTPUT "${_assets}/reference.pak"
    COMMAND "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets" "${_assets}/reference.pak"
    DEPENDS "${AETHER_WEB_ASSETPACKER}"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/audio/reference.wav"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/textures/reference.png"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/prefabs/ReferencePickup.prefab.toml"
    VERBATIM)
add_custom_command(OUTPUT "${_assets}/sprite.wgsl"
    COMMAND "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/tools/experiments/rust-webgpu/webgpu/sprite.slang"
        -target wgsl -entry vertexMain -entry fragmentMain -o "${_assets}/sprite.wgsl"
    DEPENDS "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/tools/experiments/rust-webgpu/webgpu/sprite.slang"
    VERBATIM)
add_custom_target(WebAssets DEPENDS "${_assets}/reference.pak" "${_assets}/sprite.wgsl")
add_dependencies(GameRuntime WebAssets)
target_link_options(GameRuntime PRIVATE "--preload-file=${_assets}@/assets")
add_custom_target(WebRuntimeAudit ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/audit.py" wasm
        "$<TARGET_FILE_DIR:GameRuntime>/AetherGame.wasm" --report "${CMAKE_BINARY_DIR}/web-imports.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/audit.py" sources
        "${CMAKE_BINARY_DIR}/compile_commands.json" --report "${CMAKE_BINARY_DIR}/web-sources.json"
    DEPENDS GameRuntime
    VERBATIM)

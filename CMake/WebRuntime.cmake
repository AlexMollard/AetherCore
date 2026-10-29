# This branch intentionally precedes desktop Dependencies/DotNet/SlangShaders.
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "AETHERCORE_WEB_RUNTIME requires the pinned Emscripten toolchain")
endif()
option(AETHERCORE_WEB_TEST_BRIDGE "Include development-only browser automation" OFF)
option(AETHERCORE_WEB_QUALIFY_STAGED_PROJECT "Allow the test bridge in an internal staged-project qualification build" OFF)
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
set(AETHER_WEB_MATERIAL_COMPILER "" CACHE FILEPATH "Native material graph compiler (resolved lazily for development fixtures)")
if(NOT AETHER_WEB_PROJECT)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c
        "import sys; sys.path.insert(0,sys.argv[1]); from web_materials import resolve_compiler; print(resolve_compiler(sys.argv[2] or None))"
        "${CMAKE_SOURCE_DIR}/tools/shaders" "${AETHER_WEB_MATERIAL_COMPILER}"
        RESULT_VARIABLE _material_exit OUTPUT_VARIABLE _material_compiler ERROR_VARIABLE _material_error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _material_exit EQUAL 0)
        message(FATAL_ERROR "Native material compiler preflight failed: ${_material_error}")
    endif()
    set(AETHER_WEB_MATERIAL_COMPILER "${_material_compiler}")
endif()
set(AETHER_WEB_STAGED_PROJECT "" CACHE PATH "Preflight-validated selected project assets")
set(AETHER_WEB_PROFILE "webgpu-2d-v1" CACHE STRING "Selected browser export profile")
set(AETHER_WEB_GAMEPLAY_MANIFEST "" CACHE FILEPATH "Selected Cargo manifest")
set(AETHER_WEB_GAMEPLAY_NAME "" CACHE STRING "Selected Rust library name")
set(AETHER_WEB_MANAGED_PROJECT "" CACHE FILEPATH "Selected C# gameplay project")
# Published (staged-project) exports never carry the automation bridge; a reference/qualification
# build of the C# runtime (no staged project) may, so native-vs-browser parity can read its state.
if(AETHERCORE_WEB_MANAGED AND (NOT EXISTS "${AETHER_WEB_MANAGED_PROJECT}" OR (AETHERCORE_WEB_TEST_BRIDGE AND AETHER_WEB_STAGED_PROJECT AND NOT AETHERCORE_WEB_QUALIFY_STAGED_PROJECT)))
    message(FATAL_ERROR "Managed Web exports require a selected C# project; published exports require automation OFF")
endif()
if(AETHER_WEB_PROJECT AND NOT AETHERCORE_WEB_MANAGED)
    if((AETHERCORE_WEB_TEST_BRIDGE AND NOT AETHERCORE_WEB_QUALIFY_STAGED_PROJECT) OR NOT AETHER_WEB_GAMEPLAY_MANIFEST OR NOT AETHER_WEB_STAGED_PROJECT OR NOT AETHER_WEB_GAMEPLAY_NAME MATCHES "^[A-Za-z0-9_]+$")
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
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/scripts/src/model_probe.rs"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/scripts/src/custom_pass.rs"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/fixtures3d/gameplay3d.rs"
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
if(AETHERCORE_WEB_TEST_BRIDGE AND NOT AETHERCORE_WEB_MANAGED)
    target_link_options(GameRuntime PRIVATE --emit-symbol-map)
endif()
target_compile_definitions(Engine PUBLIC AE_WEB_TEST_BRIDGE=$<BOOL:${AETHERCORE_WEB_TEST_BRIDGE}>)
# The automation panic fixture is exported by the reference Rust crate and by the C# bridge; a staged
# project's own Rust crate has none, so its qualification builds report the injected fault in C++.
if(AETHER_WEB_PROJECT AND NOT AETHERCORE_WEB_MANAGED)
    target_compile_definitions(Engine PUBLIC AE_WEB_PANIC_FIXTURE=0)
else()
    target_compile_definitions(Engine PUBLIC AE_WEB_PANIC_FIXTURE=1)
endif()
target_compile_definitions(Engine PUBLIC AE_WEB_PROFILE=\"${AETHER_WEB_PROFILE}\")
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
    DEPENDS "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/prepare_reference.mjs" "${CMAKE_SOURCE_DIR}/tools/web-runtime/cook_fonts.py" ${_game_sources}
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/ProjectSettings.toml"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/audio/reference.wav"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/textures/reference.png"
        "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/prefabs/ReferencePickup.prefab.toml"
    VERBATIM)
endif()
file(GLOB_RECURSE _shared_shader_includes CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/shaders/include/*.slangh")
file(GLOB _material_shader_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/web/*.slang*")
configure_file("${CMAKE_SOURCE_DIR}/resources/web/exposure.wgsl" "${_assets}/exposure.wgsl" COPYONLY)
foreach(_shader IN ITEMS sprite mesh hdr light2d occluder2d billboard ui_shapes_web)
	set(_shader_entries -entry vertexMain -entry fragmentMain)
	if(_shader STREQUAL "mesh")
		list(APPEND _shader_entries -entry shadowMain -entry auxiliaryMain)
	elseif(_shader STREQUAL "hdr")
		set(_shader_entries -entry vertexMain -entry backgroundMain -entry compositeMain)
	endif()
    add_custom_command(OUTPUT "${_assets}/${_shader}.wgsl"
        COMMAND "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/resources/web/${_shader}.slang"
            -target wgsl ${_shader_entries} -o "${_assets}/${_shader}.wgsl.tmp"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/shaders/wgsl_checks.py" "${_assets}/${_shader}.wgsl.tmp"
        COMMAND "${CMAKE_COMMAND}" -E rename "${_assets}/${_shader}.wgsl.tmp" "${_assets}/${_shader}.wgsl"
        DEPENDS "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/resources/web/${_shader}.slang" ${_shared_shader_includes} "${CMAKE_SOURCE_DIR}/src/shaders/text_curves.slangh"
            "${CMAKE_SOURCE_DIR}/tools/shaders/wgsl_checks.py"
        VERBATIM)
endforeach()
add_custom_command(OUTPUT "${_assets}/tiled_light_cull.wgsl"
    COMMAND "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/resources/web/tiled_light_cull.slang"
        -target wgsl -entry computeMain -o "${_assets}/tiled_light_cull.wgsl.tmp"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/shaders/wgsl_checks.py" "${_assets}/tiled_light_cull.wgsl.tmp"
    COMMAND "${CMAKE_COMMAND}" -E rename "${_assets}/tiled_light_cull.wgsl.tmp" "${_assets}/tiled_light_cull.wgsl"
    DEPENDS "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/resources/web/tiled_light_cull.slang" ${_shared_shader_includes}
        "${CMAKE_SOURCE_DIR}/tools/shaders/wgsl_checks.py"
    VERBATIM)
set(_post_effects bloom fxaa tonemap gtao_main gtao_denoise ssr ssr_composite dof volumetric_fog volumetric_composite)
set(_post_outputs)
foreach(_effect IN LISTS _post_effects)
    set(_entries -entry vertexMain -stage vertex -entry fragmentMain -stage fragment)
    if(_effect STREQUAL "bloom")
        set(_entries -entry vertexMain -stage vertex -entry downsampleMain -stage fragment -entry upsampleMain -stage fragment)
    endif()
    add_custom_command(OUTPUT "${_assets}/post-${_effect}.wgsl"
        COMMAND "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/src/shaders/${_effect}.slang"
            -D AE_WEB_POST=1 -target wgsl -matrix-layout-column-major ${_entries} -o "${_assets}/post-${_effect}.wgsl.tmp"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/shaders/wgsl_checks.py" "${_assets}/post-${_effect}.wgsl.tmp"
        COMMAND "${CMAKE_COMMAND}" -E rename "${_assets}/post-${_effect}.wgsl.tmp" "${_assets}/post-${_effect}.wgsl"
        DEPENDS "${AETHER_WEB_SLANGC}" "${CMAKE_SOURCE_DIR}/src/shaders/${_effect}.slang" ${_shared_shader_includes}
            "${CMAKE_SOURCE_DIR}/tools/shaders/wgsl_checks.py"
        VERBATIM)
    list(APPEND _post_outputs "${_assets}/post-${_effect}.wgsl")
endforeach()
add_custom_target(WebAssets DEPENDS "${_assets}/sprite.wgsl" "${_assets}/mesh.wgsl" "${_assets}/tiled_light_cull.wgsl" "${_assets}/hdr.wgsl" "${_assets}/light2d.wgsl" "${_assets}/occluder2d.wgsl" "${_assets}/billboard.wgsl" "${_assets}/ui_shapes_web.wgsl" "${_assets}/exposure.wgsl" ${_post_outputs})
if(NOT AETHER_WEB_PROJECT)
    add_custom_target(WebFixtureAssets DEPENDS "${_assets}/reference.pak")
    add_dependencies(WebAssets WebFixtureAssets)
endif()
add_dependencies(GameRuntime WebAssets)

if(AETHER_WEB_PROJECT)
    file(GLOB_RECURSE _game_sources CONFIGURE_DEPENDS "${AETHER_WEB_STAGED_PROJECT}/*")
    if(AETHER_WEB_PROFILE STREQUAL "webgpu-3d-v1")
        set(_material_compiler_args)
        if(AETHER_WEB_MATERIAL_COMPILER)
            list(APPEND _material_compiler_args --compiler "${AETHER_WEB_MATERIAL_COMPILER}")
        endif()
        add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/web/game.pak"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/cook_fonts.py" "${AETHER_WEB_ASSETPACKER}" "${AETHER_WEB_STAGED_PROJECT}"
            COMMAND "${AETHER_WEB_ASSETPACKER}" bake-all "${AETHER_WEB_STAGED_PROJECT}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/shaders/cook_project_materials.py"
                --project "${AETHER_WEB_STAGED_PROJECT}" ${_material_compiler_args}
                --slangc "${AETHER_WEB_SLANGC}" --report "${CMAKE_BINARY_DIR}/web/material-cook.json"
            COMMAND "${AETHER_WEB_ASSETPACKER}" --project --target "${AETHER_WEB_PROFILE}" "${AETHER_WEB_STAGED_PROJECT}" "${CMAKE_BINARY_DIR}/web/game.pak"
            DEPENDS "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/cook_fonts.py" ${_game_sources}
                "${CMAKE_SOURCE_DIR}/tools/shaders/cook_project_materials.py" "${CMAKE_SOURCE_DIR}/tools/shaders/web_materials.py"
            VERBATIM)
    else()
        add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/web/game.pak"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/cook_fonts.py" "${AETHER_WEB_ASSETPACKER}" "${AETHER_WEB_STAGED_PROJECT}"
            COMMAND "${AETHER_WEB_ASSETPACKER}" --project --target "${AETHER_WEB_PROFILE}" "${AETHER_WEB_STAGED_PROJECT}" "${CMAKE_BINARY_DIR}/web/game.pak"
            DEPENDS "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/cook_fonts.py" ${_game_sources}
            VERBATIM)
    endif()
else()
file(GLOB_RECURSE _game_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets/*" "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/scenes/*" "${CMAKE_SOURCE_DIR}/resources/web/reference/*" "${CMAKE_SOURCE_DIR}/projects/EmberRun/assets/ember/*" "${CMAKE_SOURCE_DIR}/projects/EmberRun/scenes/EmberRun.scene.toml")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/web/game.pak"
    COMMAND "${AETHER_WEB_NODE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/prepare_reference.mjs" "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust" "${CMAKE_BINARY_DIR}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/cook_fonts.py" "${AETHER_WEB_ASSETPACKER}" "${CMAKE_BINARY_DIR}/reference-project"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/shaders/cook_project_materials.py"
        --project "${CMAKE_BINARY_DIR}/reference-project" --compiler "${AETHER_WEB_MATERIAL_COMPILER}" --slangc "${AETHER_WEB_SLANGC}"
        --report "${CMAKE_BINARY_DIR}/web/material-cook.json"
    COMMAND "${AETHER_WEB_ASSETPACKER}" --project --target "${AETHER_WEB_PROFILE}" "${CMAKE_BINARY_DIR}/reference-project"
        "${CMAKE_BINARY_DIR}/web/game.pak"
    COMMAND "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/projects/AgentPipelineRust/assets"
        "${CMAKE_BINARY_DIR}/test-fixtures/desktop-reference.pak"
    DEPENDS "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/prepare_reference.mjs" "${CMAKE_SOURCE_DIR}/tools/web-runtime/cook_fonts.py" ${_game_sources}
        "${AETHER_WEB_MATERIAL_COMPILER}" "${CMAKE_SOURCE_DIR}/tools/shaders/cook_project_materials.py" "${CMAKE_SOURCE_DIR}/tools/shaders/web_materials.py"
        ${_shared_shader_includes} ${_material_shader_sources}
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
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_assets}/engine-fonts/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/resources/fonts" "${_assets}/engine-fonts/fonts"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/cook_fonts.py" "${AETHER_WEB_ASSETPACKER}" "${_assets}/engine-fonts/fonts"
    COMMAND "${AETHER_WEB_ASSETPACKER}" --target "${AETHER_WEB_PROFILE}"
        "${_assets}/engine-fonts" "${CMAKE_BINARY_DIR}/web/engine.pak"
    DEPENDS "${AETHER_WEB_ASSETPACKER}" "${CMAKE_SOURCE_DIR}/tools/web-runtime/cook_fonts.py" ${_font_sources}
    VERBATIM)
add_custom_target(WebEngineAssets DEPENDS "${CMAKE_BINARY_DIR}/web/engine.pak")
add_dependencies(GameRuntime WebEngineAssets)
if(TARGET WebManagedRuntime)
    add_dependencies(WebManagedRuntime WebAssets WebGameAssets WebEngineAssets)
endif()
if(NOT AETHERCORE_WEB_MANAGED)
foreach(_shader IN ITEMS sprite mesh tiled_light_cull hdr light2d occluder2d billboard ui_shapes_web)
    set_property(TARGET GameRuntime APPEND PROPERTY LINK_DEPENDS "${_assets}/${_shader}.wgsl")
    target_link_options(GameRuntime PRIVATE "--preload-file=${_assets}/${_shader}.wgsl@/assets/${_shader}.wgsl")
endforeach()
foreach(_effect IN LISTS _post_effects)
    set_property(TARGET GameRuntime APPEND PROPERTY LINK_DEPENDS "${_assets}/post-${_effect}.wgsl")
    target_link_options(GameRuntime PRIVATE "--preload-file=${_assets}/post-${_effect}.wgsl@/assets/post-${_effect}.wgsl")
endforeach()
set_property(TARGET GameRuntime APPEND PROPERTY LINK_DEPENDS "${_assets}/exposure.wgsl")
target_link_options(GameRuntime PRIVATE "--preload-file=${_assets}/exposure.wgsl@/assets/exposure.wgsl")
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

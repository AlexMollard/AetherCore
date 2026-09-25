# All packer logic lives in the AssetPipeline static lib (single source of truth for pak
# production). AssetPacker is a thin CLI frontend for the build-time bake + CI; the editor
# links AssetPipeline directly and packs in-process.

file(GLOB_RECURSE ASSETPIPELINE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ASSETPIPELINE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# main.cpp and the exe-only stb impl are NOT part of the library.
list(REMOVE_ITEM ASSETPIPELINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StbImageImpl.cpp"
)

add_library(AssetPipeline STATIC
    ${ASSETPIPELINE_SOURCES}
    ${ASSETPIPELINE_HEADERS}
    "${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp"
    "${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp"
)

target_include_directories(AssetPipeline PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(AssetPipeline PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"   # PakFormat.hpp, AeBnFormat.hpp
    "${xxHash_SOURCE_DIR}"
)

# SYSTEM: these deps trip deprecated-CRT / unused-internal warnings on Windows.
target_include_directories(AssetPipeline SYSTEM PRIVATE
    "${bc7enc_rdo_SOURCE_DIR}"      # bc7enc.h, rgbcx.h
    "${stb_SOURCE_DIR}"             # stb_image.h
    "${cgltf_SOURCE_DIR}"           # cgltf.h
    "${glm_SOURCE_DIR}"             # glm/glm.hpp
)

target_link_libraries(AssetPipeline PRIVATE zstd::libzstd_static freetype)

if(TARGET tomlplusplus::tomlplusplus)
    target_link_libraries(AssetPipeline PRIVATE tomlplusplus::tomlplusplus)
elseif(TARGET tomlplusplus_tomlplusplus)
    target_link_libraries(AssetPipeline PRIVATE tomlplusplus_tomlplusplus)
else()
    target_include_directories(AssetPipeline SYSTEM PRIVATE "${tomlplusplus_SOURCE_DIR}/include")
endif()

# Thin CLI frontend: compiles the exe-only stb_image impl and links the library.
add_executable(AssetPacker
    main.cpp
    StbImageImpl.cpp
)
target_link_libraries(AssetPacker PRIVATE AssetPipeline nlohmann_json::nlohmann_json)
target_include_directories(AssetPacker SYSTEM PRIVATE "${stb_SOURCE_DIR}" "${glm_SOURCE_DIR}")

# main.cpp includes PakWriter.hpp -> PakFormat.hpp, which is PRIVATE on AssetPipeline, so
# the exe needs its own path to src/include. It also includes KenneyImport.hpp, whose
# PipelineUtils.hpp pulls in <glm/glm.hpp> (PRIVATE on AssetPipeline too, hence the
# SYSTEM include above rather than relying on the library link to propagate it).
target_include_directories(AssetPacker PRIVATE "${CMAKE_SOURCE_DIR}/src/include")

set_target_properties(AssetPacker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

aethercore_enable_dead_strip_report(AssetPacker)

# ── Automatic per-project asset baking ───────────────────────────────────────
# Keeps every project's assets/models/*.glb|*.gltf baked automatically, so "a fresh
# clone has zero baked meshes" (every gitignored .mesh/.material/etc - see .gitignore's
# baked-artifact comment) is fixed by `cmake --build` itself rather than a manual step
# someone has to remember. One custom command per discovered project (a directory under
# projects/ with a ProjectSettings.toml), each producing a stamp file so Ninja/Make only
# reruns `AssetPacker bake-all` for a project whose model SOURCE files actually changed -
# not on every build, and not because the project's OWN baked outputs (.mesh/.material/
# etc, written alongside the source by BakeOutputs.hpp) changed. That second exclusion is
# why the DEPENDS glob below is a source-extension allowlist rather than "every file
# under assets/models/": including the generated outputs would make each bake retrigger
# the next build forever, since running the command is what changes their mtimes.
file(GLOB _aethercore_project_settings CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/projects/*/ProjectSettings.toml")

set(_aethercore_bake_stamps "")
foreach(_aethercore_settings ${_aethercore_project_settings})
    get_filename_component(_aethercore_project_dir "${_aethercore_settings}" DIRECTORY)
    get_filename_component(_aethercore_project_name "${_aethercore_project_dir}" NAME)
    set(_aethercore_models_dir "${_aethercore_project_dir}/assets/models")
    if(EXISTS "${_aethercore_models_dir}")
        file(GLOB_RECURSE _aethercore_model_sources CONFIGURE_DEPENDS
            "${_aethercore_models_dir}/*.glb"
            "${_aethercore_models_dir}/*.gltf"
            "${_aethercore_models_dir}/*.bin"
            "${_aethercore_models_dir}/*.png"
            "${_aethercore_models_dir}/*.jpg"
            "${_aethercore_models_dir}/*.jpeg"
            "${_aethercore_models_dir}/*.tga"
            "${_aethercore_models_dir}/*.hdr"
            "${_aethercore_models_dir}/*.ktx2"
            # Material overrides (materials/<name>.material.override.toml) are hand-authored
            # SOURCE files: a change to one must retrigger the bake like a glTF change.
            "${_aethercore_models_dir}/*.override.toml"
        )
        set(_aethercore_stamp "${CMAKE_BINARY_DIR}/bake-stamps/${_aethercore_project_name}.stamp")
        add_custom_command(
            OUTPUT "${_aethercore_stamp}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bake-stamps"
            COMMAND "$<TARGET_FILE:AssetPacker>" bake-all "${_aethercore_project_dir}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_aethercore_stamp}"
            DEPENDS AssetPacker ${_aethercore_model_sources}
            COMMENT "Baking ${_aethercore_project_name}'s models (AssetPacker bake-all)"
            VERBATIM
        )
        list(APPEND _aethercore_bake_stamps "${_aethercore_stamp}")
    endif()
endforeach()

if(_aethercore_bake_stamps)
    add_custom_target(BakeProjectAssets ALL DEPENDS ${_aethercore_bake_stamps})
endif()

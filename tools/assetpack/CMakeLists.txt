# All packer logic lives in the AssetPipeline static library (single source of
# truth for pak production). The AssetPacker executable is a thin CLI frontend
# kept for the CMake build-time bake and CI/headless use. The editor (App) links
# AssetPipeline directly and packs in-process.

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

# Consumers (exe, editor, tests) include AssetPipeline.hpp from this dir.
target_include_directories(AssetPipeline PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(AssetPipeline PRIVATE
    "${CMAKE_SOURCE_DIR}/include"   # PakFormat.hpp, AeBnFormat.hpp
    "${xxHash_SOURCE_DIR}"
)

# External libraries use deprecated CRT functions / have unused internals on
# Windows; mark as SYSTEM to suppress warnings.
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

# Thin CLI frontend. Compiles the exe-only stb_image implementation and links the
# library. stb include dir is needed for StbImageImpl.cpp.
add_executable(AssetPacker
    main.cpp
    StbImageImpl.cpp
)
target_link_libraries(AssetPacker PRIVATE AssetPipeline)
target_include_directories(AssetPacker SYSTEM PRIVATE "${stb_SOURCE_DIR}")

# main.cpp includes PakWriter.hpp directly, which needs PakFormat.hpp. This is
# PRIVATE on AssetPipeline (a library implementation detail), so main.cpp -
# compiled straight into this exe target, not into the library - needs its own
# path to it.
target_include_directories(AssetPacker PRIVATE "${CMAKE_SOURCE_DIR}/include")

set_target_properties(AssetPacker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

aethercore_enable_dead_strip_report(AssetPacker)

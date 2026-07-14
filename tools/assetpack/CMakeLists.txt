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
target_link_libraries(AssetPacker PRIVATE AssetPipeline)
target_include_directories(AssetPacker SYSTEM PRIVATE "${stb_SOURCE_DIR}")

# main.cpp includes PakWriter.hpp -> PakFormat.hpp, which is PRIVATE on AssetPipeline, so
# the exe needs its own path to src/include.
target_include_directories(AssetPacker PRIVATE "${CMAKE_SOURCE_DIR}/src/include")

set_target_properties(AssetPacker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

aethercore_enable_dead_strip_report(AssetPacker)

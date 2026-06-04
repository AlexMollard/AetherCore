file(GLOB_RECURSE ASSETPACKER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ASSETPACKER_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_executable(AssetPacker
    ${ASSETPACKER_SOURCES}
    ${ASSETPACKER_HEADERS}
    "${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp"
    "${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp"
)

target_include_directories(AssetPacker PRIVATE
    "${CMAKE_SOURCE_DIR}/include"   # PakFormat.hpp, AeBnFormat.hpp
    "${xxHash_SOURCE_DIR}"
)

# External libraries use deprecated CRT functions / have unused internals on Windows;
# mark as SYSTEM to suppress warnings.
target_include_directories(AssetPacker SYSTEM PRIVATE
    "${bc7enc_rdo_SOURCE_DIR}"      # bc7enc.h, rgbcx.h
    "${stb_SOURCE_DIR}"             # stb_image.h
    "${cgltf_SOURCE_DIR}"           # cgltf.h
)

target_link_libraries(AssetPacker PRIVATE zstd::libzstd_static)

if(TARGET tomlplusplus::tomlplusplus)
    target_link_libraries(AssetPacker PRIVATE tomlplusplus::tomlplusplus)
elseif(TARGET tomlplusplus_tomlplusplus)
    target_link_libraries(AssetPacker PRIVATE tomlplusplus_tomlplusplus)
else()
    target_include_directories(AssetPacker SYSTEM PRIVATE "${tomlplusplus_SOURCE_DIR}/include")
endif()

set_target_properties(AssetPacker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

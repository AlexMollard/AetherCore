file(GLOB_RECURSE ASSETPACKER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ASSETPACKER_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_executable(AssetPacker
    ${ASSETPACKER_SOURCES}
    ${ASSETPACKER_HEADERS}
)

target_compile_features(AssetPacker PRIVATE cxx_std_20)

# Place the binary in a dedicated subdirectory so it does not sit alongside the
# game's executable and is easy to exclude from distribution packages.
set_target_properties(AssetPacker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

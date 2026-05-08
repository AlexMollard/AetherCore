file(GLOB_RECURSE ASSETPACKER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ASSETPACKER_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_executable(AssetPacker
    ${ASSETPACKER_SOURCES}
    ${ASSETPACKER_HEADERS}
)

target_compile_features(AssetPacker PRIVATE cxx_std_20)

# Shared pak format header lives at <repo>/include/ so both AssetPacker and
# the engine's PakBackend can include it with angle brackets: #include <PakFormat.hpp>
target_include_directories(AssetPacker PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${xxHash_SOURCE_DIR}"
)

target_link_libraries(AssetPacker PRIVATE libzstd_static)

# Place the binary in a dedicated subdirectory so it does not sit alongside the
# game's executable and is easy to exclude from distribution packages.
set_target_properties(AssetPacker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

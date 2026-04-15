add_executable(AssetPacker main.cpp)

target_compile_features(AssetPacker PRIVATE cxx_std_20)

# Place the binary in a dedicated subdirectory so it does not sit alongside the
# game's executable and is easy to exclude from distribution packages.
set_target_properties(AssetPacker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

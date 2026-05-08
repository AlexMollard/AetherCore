include(get_cpm)

# After the initial download, do not re-check remotes on every configure.
set(CPM_SOURCE_CACHE_UPDATES_DISCONNECTED ON)

find_package(Vulkan REQUIRED)

# ── Window / input ────────────────────────────────────────────────────────────
CPMAddPackage(
    NAME glfw
    GIT_REPOSITORY https://github.com/glfw/glfw
    GIT_TAG        a74efa0d5628b74adc0426af4c5710e287fa7c2c # 3.4
    GIT_SHALLOW    TRUE
    OPTIONS
        "GLFW_BUILD_DOCS OFF"
        "GLFW_BUILD_TESTS OFF"
        "GLFW_BUILD_EXAMPLES OFF"
        "GLFW_INSTALL OFF"
)

# ── Math ──────────────────────────────────────────────────────────────────────
CPMAddPackage(
    NAME glm
    GIT_REPOSITORY https://github.com/g-truc/glm
    GIT_TAG        8d1fd52e5ab5590e2c81768ace50c72bae28f2ed # 1.0.3
    GIT_SHALLOW    TRUE
)

# ── Vulkan helpers ────────────────────────────────────────────────────────────
CPMAddPackage(
    NAME vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap
    GIT_TAG        29777173ac64752fe9ce3d2ff2821d1dd9c7f79b # v1.4.341
    GIT_SHALLOW    TRUE
)

# volk: header-only mode so all loader globals are built with the same config.
CPMAddPackage(
    NAME volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        vulkan-sdk-1.4.341.0
    GIT_SHALLOW    TRUE
    OPTIONS "VOLK_HEADERS_ONLY ON"
)

# ── GPU memory ────────────────────────────────────────────────────────────────
# VMA is used header-only; DOWNLOAD_ONLY skips its own CMakeLists.
CPMAddPackage(
    NAME VMA
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        1d8f600fd424278486eade7ed3e877c99f0846b1 # v3.3.0
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  YES
)

# ── Image / mesh loading (header-only, no CMakeLists) ─────────────────────────
CPMAddPackage(
    NAME stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  YES
)

CPMAddPackage(
    NAME cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  YES
)

# ── ECS ───────────────────────────────────────────────────────────────────────
CPMAddPackage(
    NAME EnTT
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
)

# ── Config parsing ────────────────────────────────────────────────────────────
CPMAddPackage(
    NAME tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# ── Font rendering ────────────────────────────────────────────────────────────
CPMAddPackage(
    NAME freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG        VER-2-14-3
    GIT_SHALLOW    TRUE
    OPTIONS
        "FT_DISABLE_ZLIB ON"
        "FT_DISABLE_BZIP2 ON"
        "FT_DISABLE_PNG ON"
        "FT_DISABLE_HARFBUZZ ON"
        "FT_DISABLE_BROTLI ON"
)

# ── Profiler ──────────────────────────────────────────────────────────────────
option(AETHERCORE_ENABLE_TRACY "Enable Tracy profiler instrumentation" ON)
if(AETHERCORE_ENABLE_TRACY)
    set(_tracy_opts "TRACY_ENABLE ON" "TRACY_ON_DEMAND ON")
else()
    set(_tracy_opts "TRACY_ENABLE OFF")
endif()

CPMAddPackage(
    NAME Tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.13.1
    GIT_SHALLOW    TRUE
    OPTIONS        ${_tracy_opts}
)

# ── Physics ───────────────────────────────────────────────────────────────────
# Cross-platform determinism is required for future lockstep / rollback networking.
CPMAddPackage(
    NAME JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.5.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build
    OPTIONS
        "CROSS_PLATFORM_DETERMINISTIC ON"
        "OVERRIDE_CXX_FLAGS OFF"
        "ENABLE_ALL_WARNINGS OFF"
        "USE_STATIC_MSVC_RUNTIME_LIBRARY OFF"
        "USE_AVX2 OFF"
        "USE_AVX512 OFF"
)

# ── Hashing ───────────────────────────────────────────────────────────────────
CPMAddPackage(
    NAME xxHash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  YES
)

# ── BCn texture compression (packer only) ─────────────────────────────────────
# bc7enc.cpp is compiled directly into AssetPacker; rgbcx.h is header-only.
CPMAddPackage(
    NAME bc7enc_rdo
    GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  YES
)

# ── Compression ───────────────────────────────────────────────────────────────
CPMAddPackage(
    NAME zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  build/cmake
    OPTIONS
        "ZSTD_BUILD_PROGRAMS OFF"
        "ZSTD_BUILD_TESTS OFF"
        "ZSTD_BUILD_CONTRIB OFF"
        "ZSTD_BUILD_SHARED OFF"
)

# ── Solution folder organisation (Visual Studio only) ─────────────────────────
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

foreach(_dep IN ITEMS
    glfw update_mappings
    glm vk-bootstrap volk
    VulkanMemoryAllocator
    freetype
    cgltf
    EnTT
    tomlplusplus_tomlplusplus
    TracyClient
    Jolt
    libzstd_static
)
    if(TARGET ${_dep})
        set_target_properties(${_dep} PROPERTIES FOLDER "Dependencies")
    endif()
endforeach()

set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")

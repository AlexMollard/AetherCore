add_library(Engine STATIC)

# CONFIGURE_DEPENDS re-runs glob at build time when the directory changes,
# so adding a new .cpp/.hpp no longer requires manually editing this file.
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

target_sources(Engine
    PRIVATE ${ENGINE_SOURCES}
    PUBLIC FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES ${ENGINE_HEADERS}
)


target_link_libraries(Engine
    PUBLIC
        volk::volk_headers
        glfw
        glm::glm
        EnTT::EnTT
        vk-bootstrap
        Jolt
        # dbghelp is Windows-only; generator expression avoids the if(WIN32) block.
        $<$<PLATFORM_ID:Windows>:dbghelp>
        # libdl provides dladdr for symbol resolution in crash handler on Linux
        $<$<PLATFORM_ID:Linux>:dl>
        Tracy::TracyClient
    PRIVATE
        freetype
)

if(AETHERCORE_ENABLE_IMGUI AND TARGET imgui)
    target_link_libraries(Engine PUBLIC imgui)
endif()

target_compile_definitions(Engine PUBLIC
    VK_NO_PROTOTYPES
    $<$<BOOL:${AETHERCORE_ENABLE_IMGUI}>:AETHER_IMGUI>
    $<$<PLATFORM_ID:Windows>:VK_USE_PLATFORM_WIN32_KHR>
    GLM_FORCE_DEPTH_ZERO_TO_ONE   # Vulkan depth range is [0, 1], not [-1, 1]
    GLM_ENABLE_EXPERIMENTAL
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
)

# Shared pak format header lives at <repo>/include/ - included by PakBackend
# and the AssetPacker tool so the on-disk layout stays in sync automatically.
target_include_directories(Engine PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    "${xxHash_SOURCE_DIR}"
)

target_link_libraries(Engine PUBLIC zstd::libzstd_static)

# VMA is header-only from the consumer's perspective.
# Mark as SYSTEM so clang suppresses warnings from its headers.
target_include_directories(Engine SYSTEM PUBLIC "${VMA_SOURCE_DIR}/include")

# Jolt's EXCLUDE_FROM_ALL flag can prevent automatic include propagation through
# target_link_libraries in some CMake generators, so we set it explicitly.
# The headers live directly under joltphysics_SOURCE_DIR, so <Jolt/Jolt.h> resolves.
target_include_directories(Engine PUBLIC "${joltphysics_SOURCE_DIR}")

# stb is header-only: add its source directory as a private include path.
# Texture.cpp defines STB_IMAGE_IMPLEMENTATION once to pull in the implementation.
target_include_directories(Engine PRIVATE
    "${stb_SOURCE_DIR}"
)

if(TARGET tomlplusplus::tomlplusplus)
    target_link_libraries(Engine PUBLIC tomlplusplus::tomlplusplus)
elseif(TARGET tomlplusplus_tomlplusplus)
    target_link_libraries(Engine PUBLIC tomlplusplus_tomlplusplus)
else()
    target_include_directories(Engine PUBLIC "${tomlplusplus_SOURCE_DIR}/include")
endif()

aethercore_target_defaults(Engine)

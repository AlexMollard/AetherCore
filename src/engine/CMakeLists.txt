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
        $<$<BOOL:${NVIDIA_AFTERMATH_FOUND}>:Nvidia::Aftermath>
        imgui
    PRIVATE
        freetype
)

# ── .NET (CoreCLR) hosting ────────────────────────────────────────────────────
# DotNetHost links nethost statically to bootstrap the C# runtime. When the .NET
# SDK is absent AETHER_HAS_DOTNET is 0 and DotNetHost compiles as a stub.
if(AETHER_HAS_DOTNET)
    target_link_libraries(Engine PRIVATE DotNet::NetHost)
endif()

target_compile_definitions(Engine PUBLIC
    AETHER_HAS_DOTNET=$<BOOL:${AETHER_HAS_DOTNET}>
    VK_NO_PROTOTYPES
    $<$<PLATFORM_ID:Windows>:VK_USE_PLATFORM_WIN32_KHR>
    GLM_FORCE_DEPTH_ZERO_TO_ONE   # Vulkan depth range is [0, 1], not [-1, 1]
    GLM_ENABLE_EXPERIMENTAL
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    AETHER_DEFAULT_ASSET_PAK="${CMAKE_BINARY_DIR}/data/assets.pak"
    AETHER_DEFAULT_ASSET_DIR="${CMAKE_SOURCE_DIR}/resources"
    AETHER_DEFAULT_PROJECT_PAK="${CMAKE_BINARY_DIR}/data/project.pak"
    AETHER_DEFAULT_PROJECT_DIR="${AETHERCORE_PROJECT_DIR}"
    # Build-tier config - Defines.hpp reads these for policy (Tracy, asserts, etc.)
    $<$<CONFIG:Debug>:AE_CONFIG_DEBUG>
    $<$<CONFIG:RelWithDebInfo>:AE_CONFIG_DEV>
    $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${AETHERCORE_RETAIL}>>>:AE_CONFIG_SHIP>
    $<$<BOOL:${AETHERCORE_RETAIL}>:AE_CONFIG_RETAIL>
    $<$<CONFIG:MinSizeRel>:AE_CONFIG_RETAIL>
    AETHERCORE_ENABLE_TRACY_GPU=$<BOOL:${AETHERCORE_ENABLE_TRACY_GPU}>
    AETHERCORE_ENABLE_TRACY_PLOTS=$<BOOL:${AETHERCORE_ENABLE_TRACY_PLOTS}>
    AETHERCORE_ENABLE_TRACY_MEMORY=$<BOOL:${AETHERCORE_ENABLE_TRACY_MEMORY}>
    $<$<BOOL:${NVIDIA_AFTERMATH_FOUND}>:AETHER_ENABLE_NVIDIA_AFTERMATH>
)

# Force-include Defines.hpp so it is the very first thing read by every
# translation unit. Defines.hpp sets TRACY_ENABLE, which must precede any
# Tracy header include. AAA engines commonly use this pattern.
#
# Key off the compiler *frontend variant*, not COMPILER_ID. clang-cl reports
# COMPILER_ID=Clang yet drives an MSVC-style command line, so it needs the
# /FI spelling. Feeding clang-cl the GNU "-include" form silently drops the
# flag (unknown argument) and promotes Defines.hpp to a stray second source
# input, which breaks every compile_commands.json consumer (clangd, clang-tidy)
# with "expected exactly one compiler job" and cascading std-library errors
# such as a bogus 'no type named string_view in namespace std'.
#
# Note the quote placement: "/FI${path}" quotes the whole option so CMake sees
# one clean argument. Writing /FI"${path}" instead embeds literal quotes that
# Ninja shell-escapes to /FI\"...\" in compile_commands.json - cl.exe copes,
# but clang-tidy mis-parses it into a bogus 'empty filename' error.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(Engine PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/Defines.hpp")
else()
    target_compile_options(Engine PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/Defines.hpp")
endif()

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

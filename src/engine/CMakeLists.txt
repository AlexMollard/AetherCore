# Engine core deliberately does NOT link Dear ImGui: it depends only on the abstract
# rendering/IUiOverlay.hpp, so GameRuntime (Engine without App's imgui sources) never
# links or initializes imgui. See AetherCore::SetUiOverlay + src/app's source filter.
add_library(Engine STATIC)

# CONFIGURE_DEPENDS re-globs at build time, so a new .cpp/.hpp needs no edit here.
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
        $<$<PLATFORM_ID:Windows>:dbghelp>   # crash-handler symbols
        $<$<PLATFORM_ID:Linux>:dl>          # dladdr for crash-handler symbols
        # Tracy (dev profiler) only in configs whose Defines.hpp sets TRACY_ENABLE; every
        # <tracy/...> include is TRACY_ENABLE-gated, so Ship/Retail never link it.
        $<$<CONFIG:Debug,RelWithDebInfo>:Tracy::TracyClient>
        $<$<BOOL:${NVIDIA_AFTERMATH_FOUND}>:Nvidia::Aftermath>
        $<$<BOOL:${NVIDIA_AFTERMATH_FOUND}>:delayimp>   # thunk for /DELAYLOAD below
    PRIVATE
        box2d
        freetype
)

# Aftermath (dev-only GPU crash diagnostics) is delay-loaded so its DLL is never a
# load-time dependency of every Engine consumer (GameRuntime/EngineTests/App on a machine
# without it). The vendor/editor runtime gate in VulkanContext.cpp / AftermathContext.cpp
# ensures it is only ever CALLED from an editor build on an NVIDIA device. PUBLIC so the
# option propagates to every executable linking this static lib.
target_link_options(Engine PUBLIC
    $<$<BOOL:${NVIDIA_AFTERMATH_FOUND}>:/DELAYLOAD:GFSDK_Aftermath_Lib.x64.dll>
)

# ── .NET (CoreCLR) hosting ──────────────────────────────────────────────────
# DotNetHost links nethost to bootstrap the C# runtime; when the SDK is absent
# AETHER_HAS_DOTNET is 0 and it compiles as a stub.
if(AETHER_HAS_DOTNET)
    target_link_libraries(Engine PRIVATE DotNet::NetHost)

# PUBLIC: the operator new/delete replacement lives in Engine, but the symbols must
# reach every executable that links it or the replacement silently does not apply.
target_link_libraries(Engine PUBLIC mimalloc-static)
endif()

target_compile_definitions(Engine PUBLIC
    AETHER_HAS_DOTNET=$<BOOL:${AETHER_HAS_DOTNET}>
    VK_NO_PROTOTYPES
    $<$<PLATFORM_ID:Windows>:VK_USE_PLATFORM_WIN32_KHR>
    GLM_FORCE_DEPTH_ZERO_TO_ONE   # Vulkan depth range is [0, 1]
    GLM_ENABLE_EXPERIMENTAL
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    AETHER_DEFAULT_ENGINE_PAK="${CMAKE_BINARY_DIR}/data/engine.pak"
    AETHER_DEFAULT_ENGINE_DIR="${CMAKE_BINARY_DIR}/engine_assets"
    AETHER_DEFAULT_PROJECT_PAK="${CMAKE_BINARY_DIR}/data/project.pak"
    AETHER_DEFAULT_PROJECT_DIR="${AETHERCORE_PROJECT_DIR}"
    # Build-tier policy flags read by Defines.hpp (Tracy, asserts, etc.)
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

# Force-include Defines.hpp first in every TU (it sets TRACY_ENABLE, which must precede
# any Tracy header). Key off the compiler *frontend variant*, not COMPILER_ID: clang-cl
# reports COMPILER_ID=Clang but needs the MSVC "/FI" spelling - the GNU "-include" form is
# silently dropped and breaks compile_commands.json consumers (clangd, clang-tidy). Quote
# the whole option ("/FI${path}") so it stays a single argument in compile_commands.json.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(Engine PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/Defines.hpp")
else()
    target_compile_options(Engine PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/Defines.hpp")
endif()

# Shared pak-format header (src/include/), also included by the AssetPacker tool.
target_include_directories(Engine PUBLIC
    "${CMAKE_SOURCE_DIR}/src/include"
    "${xxHash_SOURCE_DIR}"
)

target_link_libraries(Engine PUBLIC zstd::libzstd_static)

# ENet (UDP transport). PUBLIC so GameRuntime and EngineTests inherit the include
# directory that Dependencies.cmake exports on the enet target.
target_link_libraries(Engine PUBLIC enet)

# Port mapping is PRIVATE: the gateway conversation is entirely an implementation
# detail of PortMapping.cpp, and no engine header exposes a libplum type.
target_link_libraries(Engine PRIVATE plum-static)
if(WIN32)
    # enet's own CMakeLists only links winmm/ws2_32 under MINGW; MSVC/clang-cl builds
    # (every consumer here) need them supplied explicitly, same as Editor/Launcher/aether-ctl.
    target_link_libraries(Engine PUBLIC ws2_32 winmm)
endif()

# miniaudio is header-only; the MINIAUDIO_IMPLEMENTATION TU lives in audio/.
target_include_directories(Engine SYSTEM PRIVATE "${miniaudio_SOURCE_DIR}")

# VMA + stb are header-only; SYSTEM on VMA silences its header warnings. Jolt's include is
# explicit because EXCLUDE_FROM_ALL can block propagation through target_link_libraries.
target_include_directories(Engine SYSTEM PUBLIC "${VMA_SOURCE_DIR}/include")
target_include_directories(Engine PUBLIC "${joltphysics_SOURCE_DIR}")
target_include_directories(Engine PRIVATE "${stb_SOURCE_DIR}")

if(TARGET tomlplusplus::tomlplusplus)
    target_link_libraries(Engine PUBLIC tomlplusplus::tomlplusplus)
elseif(TARGET tomlplusplus_tomlplusplus)
    target_link_libraries(Engine PUBLIC tomlplusplus_tomlplusplus)
else()
    target_include_directories(Engine PUBLIC "${tomlplusplus_SOURCE_DIR}/include")
endif()

aethercore_target_defaults(Engine)

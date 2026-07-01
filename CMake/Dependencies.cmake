include(get_cpm)

# After the initial download, do not re-check remotes on every configure.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

find_package(Vulkan REQUIRED)

# ── NVIDIA Aftermath (GPU crash diagnostics) ────────────────────────────────────
# Optional - only enabled on Windows with MSVC for now.
if(MSVC)
    find_package(NvidiaAftermath)
    if(NVIDIA_AFTERMATH_FOUND)
        message(STATUS "NVIDIA Aftermath SDK found: ${NVIDIA_AFTERMATH_INCLUDE_DIR}")
    else()
        message(STATUS "NVIDIA Aftermath SDK not found - GPU crash dumps disabled")
    endif()
endif()

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
    GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4 # 2026-04-15
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
    GIT_TAG        v3.4.0
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
option(AETHERCORE_ENABLE_TRACY_GPU "Enable Tracy Vulkan GPU timeline instrumentation" ON)
option(AETHERCORE_ENABLE_TRACY_PLOTS "Enable Tracy plot/counter streams" ON)
option(AETHERCORE_ENABLE_TRACY_MEMORY "Enable Tracy CPU and named-pool memory reporting" ON)

# Tracy is always compiled with profiling support enabled so all profiler symbols
# exist in the library. The engine controls TRACY_ENABLE per build config via
# Defines.hpp (AE_CONFIG_DEBUG/DEV → Tracy ON, AE_CONFIG_SHIP/RETAIL → Tracy OFF).
# The linker strips unused Tracy symbols in Ship/Retail via /OPT:REF /Gy.
CPMAddPackage(
    NAME Tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.13.1
    GIT_SHALLOW    TRUE
    OPTIONS
        "TRACY_ENABLE ON"
        "TRACY_ON_DEMAND ON"
)

# Strip TRACY_ENABLE from TracyClient's public interface — Defines.hpp manages
# it per config, which is impossible with a PUBLIC define on a static library
# in multi-config generators.
if(TARGET TracyClient)
    get_target_property(_tracy_iface_defs TracyClient INTERFACE_COMPILE_DEFINITIONS)
    if(_tracy_iface_defs)
        list(REMOVE_ITEM _tracy_iface_defs TRACY_ENABLE)
        set_property(TARGET TracyClient PROPERTY INTERFACE_COMPILE_DEFINITIONS ${_tracy_iface_defs})
    endif()
endif()

# ── Debug / tooling UI ────────────────────────────────────────────────────────
CPMAddPackage(
    NAME imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.8-docking
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  YES
)

if(imgui_ADDED)
    add_library(imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
    )
    target_include_directories(imgui
        PUBLIC
            "${imgui_SOURCE_DIR}"
            "${imgui_SOURCE_DIR}/backends"
    )
    target_link_libraries(imgui PRIVATE glfw Vulkan::Vulkan)
    target_compile_definitions(imgui PRIVATE
        IMGUI_IMPL_VULKAN_NO_PROTOTYPES
        IMGUI_IMPL_VULKAN_USE_VOLK
    )
    if(TARGET volk::volk_headers)
        target_link_libraries(imgui PRIVATE volk::volk_headers)
    endif()
endif()

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
    GIT_TAG        dbe416d28a5530b4e8cc45b14bf034dc6b96bbde # 2026-02-27
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

# Alias the fragile internal target name so consumers use a stable interface.
if(TARGET libzstd_static AND NOT TARGET zstd::libzstd_static)
    add_library(zstd::libzstd_static ALIAS libzstd_static)
endif()

# ── Scripting ─────────────────────────────────────────────────────────────────
CPMAddPackage(
    NAME daScript
    GITHUB_REPOSITORY GaijinEntertainment/daScript
    GIT_TAG        v0.6.0
    GIT_SHALLOW    TRUE
    OPTIONS
        "DAS_TUTORIAL_DISABLED ON"
        "DAS_TESTS_DISABLED ON"
        "DAS_AOT_EXAMPLES_DISABLED ON"
        "DAS_TOOLS_DISABLED ON"
        "DAS_GLFW_DISABLED ON"
        "DAS_STBIMAGE_DISABLED ON"
        "DAS_STBTRUETYPE_DISABLED ON"
        "DAS_STDDLG_DISABLED ON"
        "DAS_UNIT_TEST_DISABLED ON"
        "DAS_OPENGL_DISABLED ON"
        "DAS_GLSL_DISABLED ON"
        "DAS_PEG_DISABLED ON"
)

if(TARGET libDaScriptDyn_xxd AND TARGET libDaScript_xxd)
    add_dependencies(libDaScriptDyn_xxd libDaScript_xxd)
endif()

# ── daScript ABI / layout defines (consumer-side ODR fix) ─────────────────────
if(TARGET libDaScript)
    target_compile_definitions(libDaScript INTERFACE
        $<$<CONFIG:Debug>:DAS_SMART_PTR_DEBUG=1>
        $<$<CONFIG:Release>:DAS_FUSION=2 DAS_DEBUGGER=1 DAS_FREE_LIST=1>
        $<$<CONFIG:MinSizeRel>:DAS_FUSION=1 DAS_DEBUGGER=1 DAS_FREE_LIST=1>
        $<$<CONFIG:RelWithDebInfo>:DAS_FUSION=1 DAS_RELWITHDEBINFO=1 DAS_SMART_PTR_DEBUG=1>
    )
endif()

# When Tracy is active (Debug/Dev), the engine defines its own global operator
# new/delete (via MemoryTracker.cpp) to hook allocation profiling. daScript's
# free_list also defines them when DAS_FREE_LIST=1. Prevent the ODR violation
# by telling daScript to skip its global operator new/delete.
# The config guard matches Defines.hpp: Debug and Dev (RelWithDebInfo).
if(TARGET libDaScript)
    target_compile_definitions(libDaScript PRIVATE
        $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:DAS_NO_GLOBAL_NEW_AND_DELETE>
    )
endif()
if(TARGET libDaScriptDyn)
    target_compile_definitions(libDaScriptDyn INTERFACE
        $<$<CONFIG:Debug>:DAS_SMART_PTR_DEBUG=1>
        $<$<CONFIG:Release>:DAS_FUSION=2 DAS_DEBUGGER=1 DAS_FREE_LIST=1>
        $<$<CONFIG:MinSizeRel>:DAS_FUSION=1 DAS_DEBUGGER=1 DAS_FREE_LIST=1>
        $<$<CONFIG:RelWithDebInfo>:DAS_FUSION=1 DAS_RELWITHDEBINFO=1 DAS_SMART_PTR_DEBUG=1>
    )
endif()

# ── Silence warnings from third-party daScript build targets ─────────────────
# daScript and its bundled deps emit hundreds of clang/MSVC warnings that we
# can't fix upstream. Suppress them on every target the daScript package adds.
if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32))
    set(_das_warning_flag /w)
else()
    set(_das_warning_flag -w)
endif()
foreach(_das_target IN ITEMS
    libDaScript libDaScriptDyn
    libDaScript_xxd libDaScriptDyn_xxd
    libUriParser libUriParserDyn
    daslang need_and_resolve)
    if(TARGET ${_das_target})
        get_target_property(_t_type ${_das_target} TYPE)
        if(NOT _t_type STREQUAL "UTILITY" AND NOT _t_type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(${_das_target} PRIVATE ${_das_warning_flag})
        endif()
        # daScript's xxd codegen rules declare .inc outputs but skip writing
        # them on no-op runs ("has not been modified"). MSBuild flags that as
        # MSB8065. Demote it to a message so the build log stays quiet.
        if(MSVC)
            set_property(TARGET ${_das_target} PROPERTY
                VS_GLOBAL_MSBuildWarningsAsMessages "MSB8065")
        endif()
    endif()
endforeach()

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
    imgui
    Jolt
    libzstd_static
    libDaScript daslang
    libDaScriptDyn libDaScript_xxd libDaScriptDyn_xxd
    libUriParser libUriParserDyn
    need_and_resolve
)
    if(TARGET ${_dep})
        set_target_properties(${_dep} PROPERTIES FOLDER "Dependencies")
    endif()
endforeach()

set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")

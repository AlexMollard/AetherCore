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
    GIT_SHALLOW    FALSE
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
    GIT_SHALLOW    FALSE
)

# ── Vulkan helpers ────────────────────────────────────────────────────────────
CPMAddPackage(
    NAME vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap
    GIT_TAG        29777173ac64752fe9ce3d2ff2821d1dd9c7f79b # v1.4.341
    GIT_SHALLOW    FALSE
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
    GIT_SHALLOW    FALSE
    DOWNLOAD_ONLY  YES
)

# ── CPU allocator ─────────────────────────────────────────────────────────────
option(AETHERCORE_MEMORY_SECURE "Build mimalloc in secure mode (guard pages, encoded free lists, double-free detection)" OFF)

# mimalloc is the process heap. Chosen for its first-class heaps, the
# mi_heap_visit_blocks walker and MI_SECURE, which the memory tracking
# subsystem builds on; see docs/superpowers/specs/2026-07-30-memory-allocator-design.md.
#
# MI_OVERRIDE is deliberately OFF. The dynamic override runtime-patches the CRT
# process-wide, which is a poor fit for a process that hosts CoreCLR; we replace
# operator new/delete ourselves in one translation unit instead, so malloc/free
# are untouched and coreclr.dll keeps its own allocator entirely.
# Resolved here rather than with a generator expression: CPM OPTIONS are plain cache
# values read at configure time, and a $<IF:...> string is non-empty, so it evaluates
# TRUTHY and silently turns secure mode ON. The tell is mimalloc reporting its library
# base name as "mimalloc-secure".
if(AETHERCORE_MEMORY_SECURE)
    set(AETHERCORE_MI_SECURE ON)
else()
    set(AETHERCORE_MI_SECURE OFF)
endif()

CPMAddPackage(
    NAME mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG        v2.1.7
    GIT_SHALLOW    TRUE
    OPTIONS
        "MI_BUILD_SHARED OFF"
        "MI_BUILD_OBJECT OFF"
        "MI_BUILD_TESTS OFF"
        "MI_OVERRIDE OFF"
        "MI_SECURE ${AETHERCORE_MI_SECURE}"
)

# NOTE: GIT_SHALLOW must stay FALSE wherever GIT_TAG is a commit SHA. A shallow clone
# fetches only the default branch tip, so a pinned commit that upstream has since moved
# past is simply absent and the checkout fails. It works on any machine with a warm CPM
# cache and fails on a clean clone - which is why it went unnoticed until CI ran. Tag and
# version pins are fine shallow, because a tag is fetchable directly.

# ── Image / mesh loading (header-only, no CMakeLists) ─────────────────────────
CPMAddPackage(
    NAME stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4 # 2026-04-15
    GIT_SHALLOW    FALSE
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

# toml++ disables std::to_chars for floats whenever the compiler is clang, guarding
# against the libstdc++/libc++ versions that shipped <charconv> without the floating
# point overloads. clang-cl uses the MSVC standard library, which has had them since
# VS2017, so on Windows that guard costs us the shortest round-trip format for no
# reason: a clang-built editor writes 0.41999999999999998 where an MSVC-built one
# writes 0.42, and the same scene file churns depending on who compiled the tool.
# Turn it back on only where the overloads are genuinely present - the clang/libc++
# case the guard was written for is left alone.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    if(TARGET tomlplusplus_tomlplusplus)
        target_compile_definitions(tomlplusplus_tomlplusplus INTERFACE TOML_FLOAT_CHARCONV=1)
    elseif(TARGET tomlplusplus)
        target_compile_definitions(tomlplusplus INTERFACE TOML_FLOAT_CHARCONV=1)
    endif()
endif()

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

# Dev-only profiler. TracyClient is built with profiling on, but Engine links it only in
# Debug/RelWithDebInfo (the TRACY_ENABLE configs); Ship/Retail reference no Tracy symbol.
# EXCLUDE_FROM_ALL keeps it out of ALL so single-config Release skips it entirely.
CPMAddPackage(
    NAME Tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.13.1
    GIT_SHALLOW    TRUE
    OPTIONS
        "TRACY_ENABLE ON"
        "TRACY_ON_DEMAND ON"
)
if(TARGET TracyClient)
    set_target_properties(TracyClient PROPERTIES EXCLUDE_FROM_ALL ON)
endif()

# Strip TRACY_ENABLE from TracyClient's public interface - Defines.hpp manages
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
        "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
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
    # UI automation: route imgui's item-info hooks to our registry (UiAutomation.cpp
    # implements the ImGuiTestEngineHook_* symbols). PUBLIC so imgui.cpp emits the
    # calls and consumers can include imgui_internal.h with matching layout.
    target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_TEST_ENGINE)
    if(TARGET volk::volk_headers)
        target_link_libraries(imgui PRIVATE volk::volk_headers)
    endif()
endif()

# ImGuizmo (viewport transform gizmo) compiles into the imgui target so it
# shares the same ImGui context/atlas. Pinned to a master commit - the 1.83
# release tag (2021) predates the imgui 1.9x API.
CPMAddPackage(
    NAME imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        87fb88b13a4f6bb04cd6ecd4e9d1625929935ff1
    DOWNLOAD_ONLY  YES
)

if(imguizmo_ADDED AND TARGET imgui)
    # Master keeps the widget sources under src/; only the gizmo itself is
    # compiled (not the sequencer/graph/curve extras).
    target_sources(imgui PRIVATE "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
    target_include_directories(imgui PUBLIC "${imguizmo_SOURCE_DIR}/src")
endif()

# ImPlot (profiler charts), compiled into the imgui target for the same reason as the
# gizmo above. Pinned to a master commit rather than the v0.16 release: v0.16 predates
# imgui 1.92, which changed ImDrawList::AddRect and moved textures to ImTextureRef, and
# does not compile against it.
CPMAddPackage(
    NAME implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        7eeb9168d2e5e6b14e266d8782ecf7e649dfc3a4
    DOWNLOAD_ONLY  YES
)

if(implot_ADDED AND TARGET imgui)
    target_sources(imgui PRIVATE
        "${implot_SOURCE_DIR}/implot.cpp"
        "${implot_SOURCE_DIR}/implot_items.cpp"
    )
    target_include_directories(imgui PUBLIC "${implot_SOURCE_DIR}")
endif()

# ── Physics ───────────────────────────────────────────────────────────────────
# Cross-platform determinism is required for future lockstep / rollback networking.
# Jolt sets /MP on itself and compiles through a precompiled header. Both make a compiler
# cache refuse the work - sccache reports "multiple input files" for /MP and cannot cache a
# /Yc or /Fp command at all - and Jolt is over a hundred translation units, so that is a
# sixth of a clean build permanently uncacheable.
#
# Neither earns anything under a generator that invokes the compiler once per file and
# schedules the parallelism itself. The Visual Studio generator is left alone, because there
# one cl invocation really is handed many files.
#
# Jolt also asks for /Zi, which our preset then overrides with /Z7 - once per file, as a
# D9025 warning. Separate PDBs are uncacheable anyway (the compiler writes to a file the
# cache does not model), which is why embedded debug info is the setting in the first place,
# so the flag is dropped rather than argued with.
#
# The PCH is a target property. These are not: Jolt appends them to CMAKE_CXX_FLAGS in its
# own directory scope, long after any hook we can attach runs. So the hook defers a call to
# the END of Jolt's directory, where the flags are final and still editable.
function(aethercore_strip_uncacheable_flags)
    string(REGEX REPLACE "(^| )/(MP|Zi)( |$)" " " _flags "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "(^| )/(MP|Zi)( |$)" " " _flags "${_flags}")
    set(CMAKE_CXX_FLAGS "${_flags}" PARENT_SCOPE)
endfunction()

if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_PROJECT_JoltPhysics_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/JoltNoMultiProcess.cmake")
endif()

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


unset(CMAKE_PROJECT_JoltPhysics_INCLUDE)

if(TARGET Jolt AND NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(Jolt PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
endif()

# ── 2D physics ────────────────────────────────────────────────────────────────
# Box2D v3 (C API). Cross-platform deterministic since 3.1 - required for the
# Physics2D replay tests and future lockstep networking, matching the Jolt policy.
CPMAddPackage(
    NAME box2d
    GIT_REPOSITORY https://github.com/erincatto/box2d.git
    GIT_TAG        v3.1.1
    GIT_SHALLOW    TRUE
    OPTIONS
        "BOX2D_SAMPLES OFF"
        "BOX2D_BENCHMARKS OFF"
        "BOX2D_DOCS OFF"
        "BOX2D_UNIT_TESTS OFF"
        "BOX2D_AVX2 OFF"          # match Jolt: no AVX2 so one binary runs everywhere
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
    GIT_SHALLOW    FALSE
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

# ── Editor control endpoint (JSON + ENet transport) ───────────────────────────
# Used by the editor-only ControlServer (src/app/editor) and the aether-ctl CLI
# (tools/control-client) for the localhost MCP control channel. Linked only where
# used, so GameRuntime carries neither.
CPMAddPackage(
    NAME nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    OPTIONS "JSON_BuildTests OFF"
)

CPMAddPackage(
    NAME enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
)

# lsalzman/enet's CMake scopes its include dir to its own build only; export it on
# the target so consumers (App, aether-ctl) resolve <enet/enet.h>.
if(TARGET enet)
    target_include_directories(enet PUBLIC "${enet_SOURCE_DIR}/include")
    set_target_properties(enet PROPERTIES FOLDER "Dependencies")
endif()

# ── Solution folder organisation (Visual Studio only) ─────────────────────────
# USE_FOLDERS / PREDEFINED_TARGETS_FOLDER are set once in the root CMakeLists.
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
    box2d
    libzstd_static
)
    if(TARGET ${_dep})
        set_target_properties(${_dep} PROPERTIES FOLDER "Dependencies")
    endif()
endforeach()

# ── Unit test framework ───────────────────────────────────────────────────────
CPMAddPackage(
    NAME doctest
    GITHUB_REPOSITORY doctest/doctest
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE
    OPTIONS "CMAKE_POLICY_VERSION_MINIMUM 3.5"
)

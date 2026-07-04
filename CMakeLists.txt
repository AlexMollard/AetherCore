cmake_minimum_required(VERSION 4.0)
project(AetherCore VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_STANDARD 26)
else()
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

# Build-speed knobs (especially useful with Visual Studio generator).
option(AETHERCORE_FAST_MSVC_DEBUG_INFO "Use /Z7 + /DEBUG:FASTLINK in Debug for faster iterative builds" ON)

option(AETHERCORE_DEAD_STRIP_REPORT
    "Emit link map files (and per-section gc log) for App and AssetPacker" OFF)

# Generate compile_commands.json for clangd / tooling (ignored by VS generator).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(Dependencies)
include(TargetDefaults)
include(SlangShaders)
include(DeadStripReport)

option(AETHERCORE_ENABLE_CLANG_TIDY_TARGET "Add clang-tidy custom target (uses the clangd preset's compile_commands.json)" OFF)
if(AETHERCORE_ENABLE_CLANG_TIDY_TARGET)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(AETHERCORE_CLANG_TIDY_RUNNER
        "C:/Program Files/LLVM/bin/run-clang-tidy"
        CACHE FILEPATH "Path to LLVM's run-clang-tidy (the LLVM Windows install drops it as an extension-less Python script in bin/)")
    include(${CMAKE_CURRENT_SOURCE_DIR}/CMake/ClangTidy.cmake)
endif()

add_subdirectory(tools/assetpack)
add_subdirectory(tools/scripts)
add_subdirectory(src/engine)
add_subdirectory(src/app)

option(AETHERCORE_BUILD_TESTS "Build the engine unit test target" ON)
if(AETHERCORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# App is the thing you actually run - make it the default F5 target.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT App)

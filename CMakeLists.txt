cmake_minimum_required(VERSION 4.0)
project(AetherCore VERSION 0.1.0 LANGUAGES CXX)

# Baseline C++20 for CMake's dialect check; C++26 preview is enabled via
# explicit /std:c++latest / -std=c++26 in TargetDefaults.cmake.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

# Build-speed knobs (especially useful with Visual Studio generator).
option(AETHERCORE_FAST_MSVC_DEBUG_INFO "Use /Z7 + /DEBUG:FASTLINK in Debug for faster iterative builds" ON)
option(AETHERCORE_ENABLE_UNITY_BUILD "Enable CMake unity build for first-party targets" ON)
option(AETHERCORE_ENABLE_IMGUI "Include Dear ImGui debug UI (disable for release/shipping builds)" ON)

# Generate compile_commands.json for clangd / tooling (ignored by VS generator).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(Dependencies)
include(TargetDefaults)
include(SlangShaders)

add_subdirectory(tools/assetpack)
add_subdirectory(src/engine)
add_subdirectory(src/app)

# App is the thing you actually run - make it the default F5 target.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT App)

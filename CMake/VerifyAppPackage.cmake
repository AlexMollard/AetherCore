cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PACKAGE_DIR)
    message(FATAL_ERROR "PACKAGE_DIR is required")
endif()
if(NOT EXISTS "${PACKAGE_DIR}")
    message(FATAL_ERROR "Package directory does not exist: ${PACKAGE_DIR}")
endif()

if(NOT DEFINED RUNTIME_EXE OR RUNTIME_EXE STREQUAL "")
    set(RUNTIME_EXE "AetherGame.exe")
endif()
if(NOT DEFINED EDITOR_EXE OR EDITOR_EXE STREQUAL "")
    set(EDITOR_EXE "App.exe")
endif()

set(_required_files
    "${RUNTIME_EXE}"
    "data/config/EngineSettings.toml"
    "data/engine.pak"
)

if(EXPECT_PROJECT_PAK)
    list(APPEND _required_files "data/project.pak")
endif()

if(EXPECT_DOTNET)
    list(APPEND _required_files
        "data/scripts/managed/AetherCore.dll"
        "data/scripts/managed/AetherCore.Interop.dll"
        "data/scripts/managed/AetherCore.Interop.deps.json"
        "data/scripts/managed/AetherCore.Interop.runtimeconfig.json"
        "data/scripts/managed/AetherGame.dll"
        "data/scripts/managed/AetherGame.deps.json"
    )
endif()

if(EXPECT_NETHOST)
    list(APPEND _required_files "nethost.dll")
endif()

foreach(_rel IN LISTS _required_files)
    if(NOT EXISTS "${PACKAGE_DIR}/${_rel}")
        message(FATAL_ERROR "Package is missing required file: ${_rel}")
    endif()
endforeach()

if(EXISTS "${PACKAGE_DIR}/${EDITOR_EXE}")
    message(FATAL_ERROR "Package contains editor executable: ${EDITOR_EXE}")
endif()

file(GLOB_RECURSE _dev_outputs
    "${PACKAGE_DIR}/*.exp"
    "${PACKAGE_DIR}/*.ilk"
    "${PACKAGE_DIR}/*.lib"
    "${PACKAGE_DIR}/*.pdb"
)
if(_dev_outputs)
    list(JOIN _dev_outputs "\n  " _dev_output_text)
    message(FATAL_ERROR "Package contains dev-only build outputs:\n  ${_dev_output_text}")
endif()

file(GLOB_RECURSE _loose_sources
    "${PACKAGE_DIR}/*.cs"
    "${PACKAGE_DIR}/*.csproj"
    "${PACKAGE_DIR}/*.vcxproj"
)
if(_loose_sources)
    list(JOIN _loose_sources "\n  " _loose_source_text)
    message(FATAL_ERROR "Package contains loose source/build files:\n  ${_loose_source_text}")
endif()

file(GLOB_RECURSE _editor_artifacts
    "${PACKAGE_DIR}/debug/*"
    "${PACKAGE_DIR}/editor/*"
    "${PACKAGE_DIR}/data/debug/*"
    "${PACKAGE_DIR}/data/editor/*"
)
if(_editor_artifacts)
    list(JOIN _editor_artifacts "\n  " _editor_artifact_text)
    message(FATAL_ERROR "Package contains editor/debug artifacts:\n  ${_editor_artifact_text}")
endif()

# NVIDIA Aftermath is a dev-only GPU-crash-diagnostics tool, editor-gated at
# runtime (VulkanContext.cpp) - GameRuntime never enables it and must not ship
# GFSDK_Aftermath_Lib.x64.dll (or any GFSDK_Aftermath* file).
file(GLOB_RECURSE _aftermath_artifacts "${PACKAGE_DIR}/GFSDK_Aftermath*")
if(_aftermath_artifacts)
    list(JOIN _aftermath_artifacts "\n  " _aftermath_artifact_text)
    message(FATAL_ERROR "Package contains NVIDIA Aftermath (dev-only, editor-gated) files:\n  ${_aftermath_artifact_text}")
endif()

message(STATUS "Verified redistributable package: ${PACKAGE_DIR}")

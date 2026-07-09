cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PACKAGE_DIR)
    message(FATAL_ERROR "PACKAGE_DIR is required")
endif()
if(NOT EXISTS "${PACKAGE_DIR}")
    message(FATAL_ERROR "Package directory does not exist: ${PACKAGE_DIR}")
endif()

set(_required_files
    "App.exe"
    "data/config/engine.toml"
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

message(STATUS "Verified redistributable package: ${PACKAGE_DIR}")

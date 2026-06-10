# AetherCore - clang-tidy custom target
#
# Adds a `clang-tidy` target that lints only first-party sources, using the
# `compile_commands.json` produced by the `clangd` CMake preset.
#
# Configure: cmake --preset clangd
# Run:       cmake --build --preset clangd --target clang-tidy
#
# Override the file filter to lint everything (e.g. when auditing third-party):
#   cmake -DAETHERCORE_CLANG_TIDY_FILE_FILTER=".*" --preset clangd

set(AETHERCORE_CLANG_TIDY_BUILD_DIR
    "${CMAKE_SOURCE_DIR}/build-ninja-clang"
    CACHE PATH "Build dir providing compile_commands.json for clang-tidy")

set(AETHERCORE_CLANG_TIDY_FILE_FILTER
    "^(src|include|tools)/"
    CACHE STRING
    "Regex matched against each 'file' in compile_commands.json; limits clang-tidy to first-party sources")

set(AETHERCORE_CLANG_TIDY_JOBS
    "0"
    CACHE STRING
    "Parallel jobs for run-clang-tidy.py (0 = CMAKE_HOST_SYSTEM_PROCESSOR_COUNT)")

if(AETHERCORE_CLANG_TIDY_JOBS STREQUAL "0")
    set(_clang_tidy_jobs "${CMAKE_HOST_SYSTEM_PROCESSOR_COUNT}")
    if(NOT _clang_tidy_jobs)
        set(_clang_tidy_jobs 8)
    endif()
else()
    set(_clang_tidy_jobs "${AETHERCORE_CLANG_TIDY_JOBS}")
endif()

set(AETHERCORE_CLANG_TIDY_COMPILE_DB
    "${AETHERCORE_CLANG_TIDY_BUILD_DIR}/compile_commands.json")

# The LLVM Windows install ships run-clang-tidy as an extension-less Python
# script (e.g. C:/Program Files/LLVM/bin/run-clang-tidy). Copy it next to the
# build dir with a .py extension so Python can resolve it as a module.
set(_clang_tidy_runner_src "${AETHERCORE_CLANG_TIDY_RUNNER}")
get_filename_component(_clang_tidy_runner_stem "${_clang_tidy_runner_src}" NAME_WLE)
set(_clang_tidy_runner_dst
    "${AETHERCORE_CLANG_TIDY_BUILD_DIR}/${_clang_tidy_runner_stem}.py")

add_custom_command(
    OUTPUT "${_clang_tidy_runner_dst}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_clang_tidy_runner_src}"
            "${_clang_tidy_runner_dst}"
    DEPENDS "${_clang_tidy_runner_src}"
    COMMENT "Staging run-clang-tidy.py next to compile_commands.json"
    VERBATIM
)

# Reconfigure the clangd preset if compile_commands.json is missing.
add_custom_command(
    OUTPUT "${AETHERCORE_CLANG_TIDY_COMPILE_DB}"
    COMMAND ${CMAKE_COMMAND} --preset clangd
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Configuring clangd preset to generate compile_commands.json"
    VERBATIM
)

add_custom_target(clang-tidy
    DEPENDS
        "${AETHERCORE_CLANG_TIDY_COMPILE_DB}"
        "${_clang_tidy_runner_dst}"
    COMMAND ${Python3_EXECUTABLE} ${_clang_tidy_runner_dst}
            -p "${AETHERCORE_CLANG_TIDY_BUILD_DIR}"
            -j ${_clang_tidy_jobs}
            -quiet
            ${AETHERCORE_CLANG_TIDY_FILE_FILTER}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM
    COMMENT "Running clang-tidy (filter: ${AETHERCORE_CLANG_TIDY_FILE_FILTER}, jobs: ${_clang_tidy_jobs})"
)

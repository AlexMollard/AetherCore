include_guard(GLOBAL)

# Opt-in only: including this file never discovers or requires a Rust toolchain.
function(aether_add_rust_gameplay name)
    cmake_parse_arguments(RUST "" "MANIFEST;TARGET_DIR;OUTPUT_DIR;PROFILE" "" ${ARGN})
    if(NOT EXISTS "${RUST_MANIFEST}")
        message(FATAL_ERROR "Gameplay Cargo manifest does not exist: ${RUST_MANIFEST}")
    endif()
    if(NOT AETHER_CARGO_EXECUTABLE)
        find_program(AETHER_CARGO_EXECUTABLE NAMES cargo REQUIRED)
    endif()
    if(NOT AETHER_RUSTC_EXECUTABLE)
        find_program(AETHER_RUSTC_EXECUTABLE NAMES rustc REQUIRED)
    endif()
    if(NOT RUST_TARGET_DIR OR NOT RUST_OUTPUT_DIR)
        message(FATAL_ERROR "Rust gameplay requires explicit target and output directories")
    endif()
    if(NOT RUST_PROFILE)
        set(RUST_PROFILE debug)
    endif()
    if(NOT RUST_PROFILE STREQUAL "release" AND NOT RUST_PROFILE STREQUAL "debug")
        message(FATAL_ERROR "Rust gameplay profile must be debug or release")
    endif()
    if(WIN32)
        set(module aether_gameplay.dll)
    elseif(APPLE)
        set(module libaether_gameplay.dylib)
    else()
        set(module libaether_gameplay.so)
    endif()
    # A successful Cargo command must precede staging; a failed build never stages an old DLL.
    add_custom_target(${name}
        COMMAND "${CMAKE_COMMAND}" "-DAETHER_CARGO_EXECUTABLE=${AETHER_CARGO_EXECUTABLE}"
            "-DAETHER_RUSTC_EXECUTABLE=${AETHER_RUSTC_EXECUTABLE}" "-DAETHER_GAMEPLAY_MANIFEST=${RUST_MANIFEST}"
            "-DAETHER_RUST_TARGET_DIR=${RUST_TARGET_DIR}" "-DAETHER_RUST_OUTPUT_DIR=${RUST_OUTPUT_DIR}"
            "-DAETHER_RUST_PROFILE=${RUST_PROFILE}" "-DAETHER_RUST_MODULE=${module}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BuildRustGameplay.cmake"
        VERBATIM USES_TERMINAL)
endfunction()

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
    set(profile_args)
    if(RUST_PROFILE STREQUAL "release")
        list(APPEND profile_args --release)
    elseif(NOT RUST_PROFILE STREQUAL "debug")
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
        COMMAND "${CMAKE_COMMAND}" -E env "RUSTC=${AETHER_RUSTC_EXECUTABLE}" "${AETHER_CARGO_EXECUTABLE}" build --lib --offline --manifest-path "${RUST_MANIFEST}"
            --target-dir "${RUST_TARGET_DIR}" --message-format=json ${profile_args}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${RUST_OUTPUT_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${RUST_TARGET_DIR}/${RUST_PROFILE}/${module}" "${RUST_OUTPUT_DIR}/${module}"
        VERBATIM USES_TERMINAL)
endfunction()

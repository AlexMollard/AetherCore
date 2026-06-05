# Slang shader compilation integration for AetherCore.

option(AETHERCORE_ENABLE_SLANG "Enable Slang shader compilation when slangc is available" ON)
set(AETHERCORE_SLANG_ROOT "$ENV{VULKAN_SDK}" CACHE PATH "Root path for Slang SDK/install (defaults to VULKAN_SDK)")
set(AETHERCORE_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders" CACHE PATH "Directory containing Slang shader sources")
set(AETHERCORE_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders" CACHE PATH "Directory for compiled shader outputs")
set(AETHERCORE_SLANG_SHADER_ARGS "-target spirv -emit-spirv-directly -fvk-use-scalar-layout -matrix-layout-column-major" CACHE STRING "Extra arguments passed to slangc for shader compilation")

function(aethercore_enable_slang_shader_compilation target_name)
    if (NOT AETHERCORE_ENABLE_SLANG)
        return()
    endif()

    if (NOT TARGET ${target_name})
        message(FATAL_ERROR "Target '${target_name}' does not exist; cannot attach Slang shader compilation")
    endif()

    set(_aethercore_slang_hints "")

    if (AETHERCORE_SLANG_ROOT)
        list(APPEND _aethercore_slang_hints
            "${AETHERCORE_SLANG_ROOT}"
            "${AETHERCORE_SLANG_ROOT}/Lib"
            "${AETHERCORE_SLANG_ROOT}/lib"
            "${AETHERCORE_SLANG_ROOT}/Bin"
            "${AETHERCORE_SLANG_ROOT}/bin"
        )
    endif()

    find_program(SLANGC_EXECUTABLE
        NAMES slangc
        HINTS ${_aethercore_slang_hints}
        PATH_SUFFIXES Bin bin
    )

    if (NOT SLANGC_EXECUTABLE)
        message(STATUS "slangc not found: continuing without Slang shader compilation")
        return()
    endif()

    message(STATUS "Slang compiler found: ${SLANGC_EXECUTABLE}")

    if (NOT EXISTS "${AETHERCORE_SHADER_SOURCE_DIR}")
        message(STATUS "Shader directory not found at ${AETHERCORE_SHADER_SOURCE_DIR}; create it to enable shader compilation")
        return()
    endif()

    file(GLOB_RECURSE AETHERCORE_SHADER_SOURCES CONFIGURE_DEPENDS
        "${AETHERCORE_SHADER_SOURCE_DIR}/*.slang"
    )

    if (NOT AETHERCORE_SHADER_SOURCES)
        message(STATUS "No .slang files found under ${AETHERCORE_SHADER_SOURCE_DIR}; skipping shader compile target")
        return()
    endif()

    # Collect shader header files so any change to an included .slangh triggers
    # recompilation of all shaders that might include it.
    file(GLOB_RECURSE AETHERCORE_SHADER_HEADERS CONFIGURE_DEPENDS
        "${AETHERCORE_SHADER_SOURCE_DIR}/*.slangh"
        "${AETHERCORE_SHADER_SOURCE_DIR}/*.slang-h"
    )

    # Convert the configurable args string into a proper argument list.
    set(_aethercore_slang_arg_list "")
    if (AETHERCORE_SLANG_SHADER_ARGS)
        separate_arguments(_aethercore_slang_arg_list UNIX_COMMAND "${AETHERCORE_SLANG_SHADER_ARGS}")
    endif()

    set(AETHERCORE_SHADER_OUTPUTS "")

    foreach(_shader_source IN LISTS AETHERCORE_SHADER_SOURCES)
        file(RELATIVE_PATH _shader_rel "${AETHERCORE_SHADER_SOURCE_DIR}" "${_shader_source}")
        get_filename_component(_shader_stem  "${_shader_rel}" NAME_WE)
        get_filename_component(_shader_parent "${_shader_rel}" DIRECTORY)
        set(_shader_output "${AETHERCORE_SHADER_OUTPUT_DIR}/${_shader_parent}/${_shader_stem}.spv")
        get_filename_component(_shader_output_dir "${_shader_output}" DIRECTORY)

        add_custom_command(
            OUTPUT "${_shader_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_shader_output_dir}"
            COMMAND "${SLANGC_EXECUTABLE}" ${_aethercore_slang_arg_list} "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:-g3>" -o "${_shader_output}" "${_shader_source}"
            DEPENDS "${_shader_source}" ${AETHERCORE_SHADER_HEADERS}
            COMMENT "Compiling Slang shader ${_shader_rel}"
            VERBATIM
        )

        list(APPEND AETHERCORE_SHADER_OUTPUTS "${_shader_output}")
    endforeach()

    add_custom_target(${target_name}_CompileShaders ALL DEPENDS ${AETHERCORE_SHADER_OUTPUTS})
    set_target_properties(${target_name}_CompileShaders PROPERTIES FOLDER "CMake")
    add_dependencies(${target_name} ${target_name}_CompileShaders)
endfunction()

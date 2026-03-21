# Slang shader compilation integration for MeowCore.

option(MEOWCORE_ENABLE_SLANG "Enable Slang shader compilation when slangc is available" ON)
set(MEOWCORE_SLANG_ROOT "$ENV{VULKAN_SDK}" CACHE PATH "Root path for Slang SDK/install (defaults to VULKAN_SDK)")
set(MEOWCORE_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders" CACHE PATH "Directory containing Slang shader sources")
set(MEOWCORE_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders" CACHE PATH "Directory for compiled shader outputs")
set(MEOWCORE_SLANG_SHADER_ARGS "-target spirv" CACHE STRING "Extra arguments passed to slangc for shader compilation")

function(meowcore_enable_slang_shader_compilation target_name)
    if (NOT MEOWCORE_ENABLE_SLANG)
        return()
    endif()

    if (NOT TARGET ${target_name})
        message(FATAL_ERROR "Target '${target_name}' does not exist; cannot attach Slang shader compilation")
    endif()

    set(_meowcore_slang_hints "")

    if (MEOWCORE_SLANG_ROOT)
        list(APPEND _meowcore_slang_hints
            "${MEOWCORE_SLANG_ROOT}"
            "${MEOWCORE_SLANG_ROOT}/Lib"
            "${MEOWCORE_SLANG_ROOT}/lib"
            "${MEOWCORE_SLANG_ROOT}/Bin"
            "${MEOWCORE_SLANG_ROOT}/bin"
        )
    endif()

    find_program(SLANGC_EXECUTABLE
        NAMES slangc
        HINTS ${_meowcore_slang_hints}
        PATH_SUFFIXES Bin bin
    )

    if (NOT SLANGC_EXECUTABLE)
        message(STATUS "slangc not found: continuing without Slang shader compilation")
        return()
    endif()

    message(STATUS "Slang compiler found: ${SLANGC_EXECUTABLE}")

    if (NOT EXISTS "${MEOWCORE_SHADER_SOURCE_DIR}")
        message(STATUS "Shader directory not found at ${MEOWCORE_SHADER_SOURCE_DIR}; create it to enable shader compilation")
        return()
    endif()

    file(GLOB_RECURSE MEOWCORE_SHADER_SOURCES CONFIGURE_DEPENDS
        "${MEOWCORE_SHADER_SOURCE_DIR}/*.slang"
    )

    if (NOT MEOWCORE_SHADER_SOURCES)
        message(STATUS "No .slang files found under ${MEOWCORE_SHADER_SOURCE_DIR}; skipping shader compile target")
        return()
    endif()

    # Convert the configurable args string into a proper argument list.
    set(_meowcore_slang_arg_list "")
    if (MEOWCORE_SLANG_SHADER_ARGS)
        separate_arguments(_meowcore_slang_arg_list NATIVE_COMMAND "${MEOWCORE_SLANG_SHADER_ARGS}")
    endif()

    set(MEOWCORE_SHADER_OUTPUTS "")

    foreach(_shader_source IN LISTS MEOWCORE_SHADER_SOURCES)
        file(RELATIVE_PATH _shader_rel "${MEOWCORE_SHADER_SOURCE_DIR}" "${_shader_source}")
        set(_shader_output "${MEOWCORE_SHADER_OUTPUT_DIR}/${_shader_rel}.spv")
        get_filename_component(_shader_output_dir "${_shader_output}" DIRECTORY)

        add_custom_command(
            OUTPUT "${_shader_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_shader_output_dir}"
            COMMAND "${SLANGC_EXECUTABLE}" ${_meowcore_slang_arg_list} -o "${_shader_output}" "${_shader_source}"
            DEPENDS "${_shader_source}"
            COMMENT "Compiling Slang shader ${_shader_rel}"
            VERBATIM
        )

        list(APPEND MEOWCORE_SHADER_OUTPUTS "${_shader_output}")
    endforeach()

    add_custom_target(CompileShaders ALL DEPENDS ${MEOWCORE_SHADER_OUTPUTS})
    add_dependencies(${target_name} CompileShaders)
endfunction()

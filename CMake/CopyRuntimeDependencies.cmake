cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

if(NOT DEFINED APP_EXE OR NOT EXISTS "${APP_EXE}")
    message(FATAL_ERROR "APP_EXE must point at the built executable")
endif()
if(NOT DEFINED PACKAGE_DIR)
    message(FATAL_ERROR "PACKAGE_DIR is required")
endif()

file(MAKE_DIRECTORY "${PACKAGE_DIR}")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${APP_EXE}"
    RESOLVED_DEPENDENCIES_VAR _resolved_deps
    UNRESOLVED_DEPENDENCIES_VAR _unresolved_deps
    PRE_EXCLUDE_REGEXES
        "api-ms-"
        "ext-ms-"
)

foreach(_dep IN LISTS _resolved_deps)
    get_filename_component(_dep_name "${_dep}" NAME)
    string(TOLOWER "${_dep}" _dep_lower)
    string(TOLOWER "${_dep_name}" _dep_name_lower)
    set(_is_windows_system_dll OFF)
    set(_is_msvc_runtime_dll OFF)
    if(_dep_lower MATCHES "[/\\\\]windows[/\\\\](system32|syswow64)[/\\\\]")
        set(_is_windows_system_dll ON)
    endif()
    if(_dep_name_lower MATCHES "^(concrt|msvcp|vccorlib|vcruntime).*\\.dll$")
        set(_is_msvc_runtime_dll ON)
    endif()
    if(_is_windows_system_dll AND NOT _is_msvc_runtime_dll)
        continue()
    endif()
    file(COPY_FILE "${_dep}" "${PACKAGE_DIR}/${_dep_name}" ONLY_IF_DIFFERENT)
endforeach()

if(_unresolved_deps)
    set(_filtered_unresolved "")
    foreach(_dep IN LISTS _unresolved_deps)
        if(_dep MATCHES "^(AzureAttestManager|AzureAttestNormal|HvsiFileTrust|PdmUtilities|wpaxholder)\\.dll$")
            continue()
        endif()
        list(APPEND _filtered_unresolved "${_dep}")
    endforeach()
endif()

if(_filtered_unresolved)
    list(JOIN _filtered_unresolved "\n  " _unresolved_text)
    message(WARNING "Unresolved runtime dependencies:\n  ${_unresolved_text}")
endif()

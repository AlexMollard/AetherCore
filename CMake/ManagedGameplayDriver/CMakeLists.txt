cmake_minimum_required(VERSION 3.25)
project(AetherManagedGameplay LANGUAGES NONE)
foreach(required AETHER_GAMEPLAY_PROJECT AETHER_DOTNET_EXECUTABLE AETHER_MANAGED_ARTIFACTS AETHER_MANAGED_OUTPUT)
    if(NOT ${required})
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()
if(NOT EXISTS "${AETHER_GAMEPLAY_PROJECT}")
    message(FATAL_ERROR "The gameplay project is missing")
endif()
add_custom_target(AetherManagedGameplay
    COMMAND "${CMAKE_COMMAND}" -E env --unset=LIB "${AETHER_DOTNET_EXECUTABLE}" build "${AETHER_GAMEPLAY_PROJECT}"
        --configuration Debug --nologo --verbosity minimal
        --artifacts-path "${AETHER_MANAGED_ARTIFACTS}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${AETHER_MANAGED_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${AETHER_MANAGED_ARTIFACTS}/bin/AetherGame/debug" "${AETHER_MANAGED_OUTPUT}"
    VERBATIM USES_TERMINAL)

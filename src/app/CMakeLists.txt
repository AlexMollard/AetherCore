add_executable(App)

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE APP_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

target_sources(App
    PRIVATE
        ${APP_SOURCES}
        ${APP_HEADERS}
)

target_include_directories(App PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(App PRIVATE Engine)

aethercore_target_defaults(App)
aethercore_enable_slang_shader_compilation(App)
aethercore_enable_dead_strip_report(App)

set_target_properties(App PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# In dev builds, let the scene serializer save/load committable scene and prefab
# files directly from the source tree.
target_compile_definitions(App PRIVATE
    AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes"
    AETHER_PREFABS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/prefabs"
)

# ── Managed (C#) assemblies ────────────────────────────────────────────────
# Build the .NET scripting assemblies with `dotnet build` and deploy them next to
# the app's data/ so DotNetHost can load them at startup. ArtifactsPath keeps all
# obj/bin output inside the CMake build tree (never in the source resources/).
# Gated on AETHER_HAS_DOTNET so a machine without the .NET SDK still builds.
if(AETHER_HAS_DOTNET)
    # Build the game scripts project; its ProjectReference pulls in AetherCore.Managed,
    # so a single `dotnet build` produces both assemblies. ArtifactsPath routes all
    # obj/bin into the CMake tree.
    set(_aether_scripts_proj "${CMAKE_SOURCE_DIR}/resources/scripts/AetherScripts.csproj")
    set(_aether_managed_cfg "$<IF:$<CONFIG:Debug>,Debug,Release>")
    set(_aether_managed_cfgdir "$<IF:$<CONFIG:Debug>,debug,release>")
    set(_aether_engine_out "${CMAKE_BINARY_DIR}/managed/bin/AetherCore.Managed/${_aether_managed_cfgdir}")
    set(_aether_scripts_out "${CMAKE_BINARY_DIR}/managed/bin/AetherScripts/${_aether_managed_cfgdir}")

    add_custom_target(ManagedAssemblies ALL
        COMMAND "${AETHER_DOTNET_EXE}" build "${_aether_scripts_proj}"
                -c ${_aether_managed_cfg} --nologo -v:m
                -p:ArtifactsPath=${CMAKE_BINARY_DIR}/managed
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/scripts/managed"
        # Engine assembly first (dll + runtimeconfig the host boots from), then the
        # game scripts assembly on top.
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_aether_engine_out}" "${CMAKE_BINARY_DIR}/data/scripts/managed"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_aether_scripts_out}" "${CMAKE_BINARY_DIR}/data/scripts/managed"
        COMMENT "Building C# managed assemblies (AetherCore.Managed + AetherScripts)"
        VERBATIM USES_TERMINAL)

    add_dependencies(App ManagedAssemblies)
    target_link_libraries(App PRIVATE DotNet::NetHost)

    # Mirror the managed assemblies next to the executable for standalone runs.
    add_custom_command(TARGET App POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:App>/data/scripts/managed"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_BINARY_DIR}/data/scripts/managed" "$<TARGET_FILE_DIR:App>/data/scripts/managed"
        COMMENT "Copying managed assemblies to app output"
        VERBATIM)

    # nethost.dll must sit next to App.exe (loader search path) so get_hostfxr_path
    # resolves at startup.
    if(WIN32 AND AETHER_NETHOST_DLL)
        add_custom_command(TARGET App POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${AETHER_NETHOST_DLL}" "$<TARGET_FILE_DIR:App>"
            COMMENT "Copying nethost.dll to app output"
            VERBATIM)
    endif()

    # Surface the C# projects in the Visual Studio solution for editing with full
    # IntelliSense. These are the same projects the ManagedAssemblies target
    # already builds via `dotnet build`, so they are excluded from the default
    # "Build Solution" batch (EXCLUDE_FROM_DEFAULT_BUILD) to avoid a redundant
    # second build - the App still consumes the ManagedAssemblies output. Editing
    # a script and rebuilding the solution recompiles it through that target.
    #
    # Requires an MSBuild that can load SDK-style .csproj, i.e. a full Visual
    # Studio install with a .NET workload. VS Build Tools alone cannot resolve
    # Microsoft.NET.Sdk and fails to even load the projects, so allow opting out
    # (e.g. on CI runners that only have Build Tools) with -DAETHER_ADD_CSHARP_PROJECTS=OFF.
    option(AETHER_ADD_CSHARP_PROJECTS "Add the C# projects to the Visual Studio solution for editing" ON)
    if(AETHER_ADD_CSHARP_PROJECTS AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(_aether_managed_proj "${CMAKE_SOURCE_DIR}/managed/AetherCore.Managed/AetherCore.Managed.csproj")
        # C# project type GUID so VS loads them as real C# projects.
        include_external_msproject(AetherCore.Managed "${_aether_managed_proj}"
            TYPE FAE04EC0-301F-11D3-BF4B-00C04F79EFBC)
        include_external_msproject(AetherScripts "${_aether_scripts_proj}"
            TYPE FAE04EC0-301F-11D3-BF4B-00C04F79EFBC)
        set_target_properties(AetherCore.Managed AetherScripts PROPERTIES
            FOLDER "Managed"
            EXCLUDE_FROM_DEFAULT_BUILD ON)
    endif()
endif()

# ── Asset pipeline ─────────────────────────────────────────────────────────
# Pack every file under resources/ into a single binary file that the runtime
# PakBackend mounts as "assets://".  The pak is regenerated on every build so
# newly added or changed source assets are always in sync.
# Note: $<TARGET_FILE:AssetPacker> creates an implicit dependency on AssetPacker,
# so an explicit add_dependencies() is not needed.
add_custom_command(TARGET App POST_BUILD
    COMMAND "$<TARGET_FILE:AssetPacker>"
        "--import-materials"
        "${CMAKE_SOURCE_DIR}/resources"
        "${CMAKE_BINARY_DIR}/data/assets.pak"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:App>/data"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_BINARY_DIR}/data/assets.pak"
        "$<TARGET_FILE_DIR:App>/data/assets.pak"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_BINARY_DIR}/data/assets.pak.log"
        "$<TARGET_FILE_DIR:App>/data/assets.pak.log"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_BINARY_DIR}/data/assets.pak.manifest"
        "$<TARGET_FILE_DIR:App>/data/assets.pak.manifest"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/engine.toml"
        "${CMAKE_BINARY_DIR}/data/config/engine.toml"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:App>/data/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/engine.toml"
        "$<TARGET_FILE_DIR:App>/data/config/engine.toml"
    COMMENT "Packing assets to data/assets.pak"
    VERBATIM
)

# Copy loose runtime data (resources/data) directly to data/data/ so it can be
# read from the filesystem without unpacking the pak. C# scripts are compiled
# into the managed assemblies (deployed to data/scripts/managed above), so the
# source scripts do not need to be copied.
add_custom_command(TARGET App POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/data"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/data"
        "${CMAKE_BINARY_DIR}/data/data"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:App>/data/data"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/data"
        "$<TARGET_FILE_DIR:App>/data/data"
    COMMENT "Copying runtime data to build output"
    VERBATIM
)

# NVIDIA Aftermath DLL - copy to output directory so the executable can find it.
if(NVIDIA_AFTERMATH_FOUND)
    get_target_property(_am_dll Nvidia::Aftermath IMPORTED_LOCATION)
    add_custom_command(TARGET App POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_am_dll}" "$<TARGET_FILE_DIR:App>"
        COMMENT "Copying GFSDK_Aftermath_Lib.x64.dll to build output"
        VERBATIM
    )
endif()

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

if(TARGET libDaScript)
    target_link_libraries(App PRIVATE libDaScript)
    # daScript headers are C++17; suppress warnings from them in our C++26 project.
    target_include_directories(App SYSTEM PRIVATE "${daScript_SOURCE_DIR}/include")
    # /bigobj needed for daScript's heavy template instantiation on MSVC / clang-cl.
    if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32))
        target_compile_options(App PRIVATE /bigobj)
    endif()
endif()

aethercore_target_defaults(App)
aethercore_enable_slang_shader_compilation(App)
aethercore_enable_dead_strip_report(App)

set_target_properties(App PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# In dev builds, let the scripting subsystem find .das files in the source tree
# so hot-reload (F5) picks up edits without a C++ rebuild, and let the scene
# serializer save/load committable scene files from the source tree.
target_compile_definitions(App PRIVATE
    AETHER_SCRIPTS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scripts"
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

# Copy scene scripts (*.das) directly to data/scripts/ so hot-reload can read
# them from the filesystem without unpacking the pak.
add_custom_command(TARGET App POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/scripts"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/scripts"
        "${CMAKE_BINARY_DIR}/data/scripts"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:App>/data/scripts"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/scripts"
        "$<TARGET_FILE_DIR:App>/data/scripts"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/data"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/data"
        "${CMAKE_BINARY_DIR}/data/data"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:App>/data/data"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/data"
        "$<TARGET_FILE_DIR:App>/data/data"
    COMMENT "Copying scripts and data to build output"
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

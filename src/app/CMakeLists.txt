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
# Build the C# projects with `dotnet build` and deploy the assemblies next to the
# app's data/ so DotNetHost can load them at startup. ArtifactsPath keeps all
# obj/bin output inside the CMake build tree (never in the source tree).
# Gated on AETHER_HAS_DOTNET so a machine without the .NET SDK still builds.
#
# Projects:
#   AetherCore          engine SDK, the public gameplay API   -> AetherCore.dll
#   AetherCore.Interop  ABI + host boot (what DotNetHost binds) -> AetherCore.Interop.dll (+ runtimeconfig)
#   AetherGame          game scripts (collectible, hot-reloaded) -> AetherGame.dll (+ deps.json)
#
# The build invokes `dotnet build` on the two leaf .csproj DIRECTLY (each pulls in
# the AetherCore SDK via ProjectReference). It deliberately does NOT build the
# checked-in AetherCore.slnx: the solution is only an IDE convenience (VS + Rider),
# so the CMake build stays generator-agnostic and cross-platform and never depends
# on a solution file. The C# projects are likewise NOT bolted onto this generated
# C++ solution via include_external_msproject - editing happens in AetherCore.slnx.
# (CMake cannot generate SDK-style .csproj, so those are hand-authored source.)
if(AETHER_HAS_DOTNET)
    set(_aether_interop_proj "${CMAKE_SOURCE_DIR}/managed/AetherCore.Interop/AetherCore.Interop.csproj")
    set(_aether_game_proj "${CMAKE_SOURCE_DIR}/game/AetherGame/AetherGame.csproj")
    set(_aether_managed_cfg "$<IF:$<CONFIG:Debug>,Debug,Release>")
    set(_aether_managed_cfgdir "$<IF:$<CONFIG:Debug>,debug,release>")
    # AetherCore.Interop's output carries the boot assembly + runtimeconfig AND the
    # SDK dll it references; AetherGame's carries the game dll + deps.json.
    set(_aether_interop_out "${CMAKE_BINARY_DIR}/managed/bin/AetherCore.Interop/${_aether_managed_cfgdir}")
    set(_aether_game_out "${CMAKE_BINARY_DIR}/managed/bin/AetherGame/${_aether_managed_cfgdir}")

    add_custom_target(ManagedAssemblies ALL
        COMMAND "${AETHER_DOTNET_EXE}" build "${_aether_interop_proj}"
                -c ${_aether_managed_cfg} --nologo -v:m
                -p:ArtifactsPath=${CMAKE_BINARY_DIR}/managed
        COMMAND "${AETHER_DOTNET_EXE}" build "${_aether_game_proj}"
                -c ${_aether_managed_cfg} --nologo -v:m
                -p:ArtifactsPath=${CMAKE_BINARY_DIR}/managed
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/scripts/managed"
        # Boot assembly + SDK first (dll + runtimeconfig the host boots from), then
        # the game scripts assembly on top.
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_aether_interop_out}" "${CMAKE_BINARY_DIR}/data/scripts/managed"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_aether_game_out}" "${CMAKE_BINARY_DIR}/data/scripts/managed"
        COMMENT "Building C# managed assemblies (AetherCore + AetherCore.Interop + AetherGame)"
        VERBATIM USES_TERMINAL)

    add_dependencies(App ManagedAssemblies)
    target_link_libraries(App PRIVATE DotNet::NetHost)

    # Dev-only F5 rebuild: let the running engine rebuild AetherGame from source and
    # redeploy before reloading (CSharpScriptingSubsystem::RebuildFromSource). These
    # paths only exist in a dev build tree; a packaged build leaves them undefined
    # and the engine falls back to reload-only.
    target_compile_definitions(App PRIVATE
        AETHER_DOTNET_EXE="${AETHER_DOTNET_EXE}"
        AETHER_GAME_PROJECT="${_aether_game_proj}"
        AETHER_MANAGED_ARTIFACTS="${CMAKE_BINARY_DIR}/managed"
        AETHER_MANAGED_CONFIG="$<IF:$<CONFIG:Debug>,Debug,Release>"
        AETHER_MANAGED_CONFIGDIR="$<IF:$<CONFIG:Debug>,debug,release>")

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
    # Shipped default settings (layer 2). Source lives under resources/config/,
    # not the repo root. Deployed beside the exe (data/config/) as a dev
    # convenience so running from the build tree finds it; the runtime resolves it
    # executable-relative, independent of CMake and the working directory. For a
    # packaged build, install this same file next to the shipped executable.
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/config/engine.toml"
        "${CMAKE_BINARY_DIR}/data/config/engine.toml"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:App>/data/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/config/engine.toml"
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

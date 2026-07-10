file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE APP_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

set(APP_EDITOR_SOURCES ${APP_SOURCES})
set(APP_RUNTIME_SOURCES ${APP_SOURCES})
set(APP_RUNTIME_HEADERS ${APP_HEADERS})
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]debug[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]editor[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]layers[/\\\\]DebugLayer\\.cpp$")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]debug[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]editor[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]layers[/\\\\]DebugLayer\\.hpp$")

add_executable(App)
add_executable(GameRuntime)

target_sources(App
    PRIVATE
        ${APP_EDITOR_SOURCES}
        ${APP_HEADERS}
)

target_sources(GameRuntime
    PRIVATE
        ${APP_RUNTIME_SOURCES}
        ${APP_RUNTIME_HEADERS}
)

target_include_directories(App PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(GameRuntime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(App PRIVATE Engine AssetPipeline)
target_link_libraries(GameRuntime PRIVATE Engine)

aethercore_target_defaults(App)
aethercore_target_defaults(GameRuntime)
aethercore_enable_slang_shader_compilation(App)
aethercore_enable_dead_strip_report(App)
aethercore_enable_dead_strip_report(GameRuntime)

set_target_properties(App PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
set_target_properties(GameRuntime PROPERTIES
    OUTPUT_NAME "AetherGame"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# In dev builds, let the scene serializer save/load committable scene and prefab
# files directly from the source tree.
target_compile_definitions(App PRIVATE
    AETHERCORE_EDITOR_APP=1
    AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes"
    AETHER_PREFABS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/prefabs"
    AETHER_MANAGED_SDK_PROJECT="${CMAKE_SOURCE_DIR}/managed/AetherCore/AetherCore.csproj"
    AETHER_GAME_RUNTIME_EXE_NAME="$<TARGET_FILE_NAME:GameRuntime>"
)

target_compile_definitions(GameRuntime PRIVATE
    AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes"
    AETHER_PREFABS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/prefabs"
    AETHER_MANAGED_SDK_PROJECT="${CMAKE_SOURCE_DIR}/managed/AetherCore/AetherCore.csproj"
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
    get_filename_component(_aether_project_dir_abs "${AETHERCORE_PROJECT_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    set(_aether_project_game_proj "${_aether_project_dir_abs}/scripts/AetherGame.csproj")
    set(_aether_fallback_game_proj "${CMAKE_SOURCE_DIR}/game/AetherGame/AetherGame.csproj")
    if(AETHERCORE_PROJECT_DIR AND EXISTS "${_aether_project_game_proj}")
        set(_aether_game_proj "${_aether_project_game_proj}")
    else()
        set(_aether_game_proj "${_aether_fallback_game_proj}")
    endif()
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

    foreach(_aether_runtime_target IN ITEMS App GameRuntime)
        add_dependencies(${_aether_runtime_target} ManagedAssemblies)
        target_link_libraries(${_aether_runtime_target} PRIVATE DotNet::NetHost)
    endforeach()

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
    foreach(_aether_runtime_target IN ITEMS App GameRuntime)
        add_custom_command(TARGET ${_aether_runtime_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${_aether_runtime_target}>/data/scripts/managed"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_BINARY_DIR}/data/scripts/managed" "$<TARGET_FILE_DIR:${_aether_runtime_target}>/data/scripts/managed"
        COMMENT "Copying managed assemblies to app output"
        VERBATIM)
    endforeach()

    # nethost.dll must sit next to each executable (loader search path) so
    # get_hostfxr_path resolves at startup.
    if(WIN32 AND AETHER_NETHOST_DLL)
        foreach(_aether_runtime_target IN ITEMS App GameRuntime)
            add_custom_command(TARGET ${_aether_runtime_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${AETHER_NETHOST_DLL}" "$<TARGET_FILE_DIR:${_aether_runtime_target}>"
            COMMENT "Copying nethost.dll to app output"
            VERBATIM)
        endforeach()
    endif()
endif()

if(NVIDIA_AFTERMATH_FOUND)
    get_target_property(_am_dll Nvidia::Aftermath IMPORTED_LOCATION)
endif()

# ── Asset pipeline ─────────────────────────────────────────────────────────
# Pack engine runtime resources into engine.pak. Project content is packed
# separately below so user assets keep their project-relative paths. Do not pack
# the whole resources/ tree here: that directory may still contain legacy/sample
# project content, while engine.pak should stay engine-owned.
function(aethercore_add_runtime_payload target_name)
    # Note: $<TARGET_FILE:AssetPacker> creates an implicit dependency on
    # AssetPacker, so an explicit add_dependencies() is not needed.
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${CMAKE_BINARY_DIR}/engine_assets"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_BINARY_DIR}/engine_assets/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/fonts"
            "${CMAKE_BINARY_DIR}/engine_assets/fonts"
        COMMAND "$<TARGET_FILE:AssetPacker>"
            "${CMAKE_BINARY_DIR}/engine_assets"
            "${CMAKE_BINARY_DIR}/data/engine.pak"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target_name}>/data"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/data/engine.pak"
            "$<TARGET_FILE_DIR:${target_name}>/data/engine.pak"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/data/engine.pak.log"
            "$<TARGET_FILE_DIR:${target_name}>/data/engine.pak.log"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/data/engine.pak.manifest"
            "$<TARGET_FILE_DIR:${target_name}>/data/engine.pak.manifest"
        # Shipped default settings (layer 2). Source lives under resources/config/,
        # not the repo root. Deployed beside the exe (data/config/) as a dev
        # convenience so running from the build tree finds it; the runtime resolves it
        # executable-relative, independent of CMake and the working directory. For a
        # packaged build, install this same file next to the shipped executable.
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/config"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/config/EngineSettings.toml"
            "${CMAKE_BINARY_DIR}/data/config/EngineSettings.toml"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target_name}>/data/config"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/config/EngineSettings.toml"
            "$<TARGET_FILE_DIR:${target_name}>/data/config/EngineSettings.toml"
        COMMENT "Packing engine runtime assets to data/engine.pak"
        VERBATIM
    )

    if(AETHERCORE_PROJECT_DIR AND EXISTS "${AETHERCORE_PROJECT_DIR}/ProjectSettings.toml")
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "$<TARGET_FILE:AssetPacker>"
                "--project"
                "--import-materials"
                "${AETHERCORE_PROJECT_DIR}"
                "${CMAKE_BINARY_DIR}/data/project.pak"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target_name}>/data"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_BINARY_DIR}/data/project.pak"
                "$<TARGET_FILE_DIR:${target_name}>/data/project.pak"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_BINARY_DIR}/data/project.pak.log"
                "$<TARGET_FILE_DIR:${target_name}>/data/project.pak.log"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_BINARY_DIR}/data/project.pak.manifest"
                "$<TARGET_FILE_DIR:${target_name}>/data/project.pak.manifest"
            COMMENT "Packing project content to data/project.pak"
            VERBATIM
        )
    endif()

    # NVIDIA Aftermath DLL - copy to output directory so the executable can find it.
    if(NVIDIA_AFTERMATH_FOUND)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_am_dll}" "$<TARGET_FILE_DIR:${target_name}>"
            COMMENT "Copying GFSDK_Aftermath_Lib.x64.dll to build output"
            VERBATIM
        )
    endif()
endfunction()

if(AETHERCORE_PROJECT_DIR AND NOT EXISTS "${AETHERCORE_PROJECT_DIR}/ProjectSettings.toml")
    message(WARNING "AETHERCORE_PROJECT_DIR='${AETHERCORE_PROJECT_DIR}' is not an Aether project root; skipping data/project.pak")
endif()

aethercore_add_runtime_payload(App)
aethercore_add_runtime_payload(GameRuntime)
add_dependencies(App GameRuntime)

# ── Redistributable package ─────────────────────────────────────────────────
# The build output directory is a developer run tree and may contain stale files,
# import libraries, or PDBs. PackageGame creates a clean folder that can be handed
# to another PC: runtime executable + runtime DLLs + data/ payload only.
set(_aether_package_dir "${CMAKE_BINARY_DIR}/package/$<CONFIG>/AetherCore")
set(_aether_package_expect_project_pak OFF)

set(_aether_package_commands
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_aether_package_dir}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_aether_package_dir}"
        "${_aether_package_dir}/data"
        "${_aether_package_dir}/data/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:GameRuntime>"
        "${_aether_package_dir}/$<TARGET_FILE_NAME:GameRuntime>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_BINARY_DIR}/data/engine.pak"
        "${_aether_package_dir}/data/engine.pak"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_BINARY_DIR}/data/config/EngineSettings.toml"
        "${_aether_package_dir}/data/config/EngineSettings.toml"
    COMMAND "${CMAKE_COMMAND}"
        -DAPP_EXE=$<TARGET_FILE:GameRuntime>
        -DPACKAGE_DIR=${_aether_package_dir}
        -P "${CMAKE_SOURCE_DIR}/CMake/CopyRuntimeDependencies.cmake"
)

if(AETHERCORE_PROJECT_DIR AND EXISTS "${AETHERCORE_PROJECT_DIR}/ProjectSettings.toml")
    set(_aether_package_expect_project_pak ON)
    list(APPEND _aether_package_commands
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/data/project.pak"
            "${_aether_package_dir}/data/project.pak")
endif()

if(AETHER_HAS_DOTNET)
    list(APPEND _aether_package_commands
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${_aether_package_dir}/data/scripts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_BINARY_DIR}/data/scripts/managed"
            "${_aether_package_dir}/data/scripts/managed")
endif()

if(WIN32 AND AETHER_NETHOST_DLL)
    list(APPEND _aether_package_commands
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${AETHER_NETHOST_DLL}"
            "${_aether_package_dir}/nethost.dll")
endif()

if(NVIDIA_AFTERMATH_FOUND)
    list(APPEND _aether_package_commands
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_am_dll}"
            "${_aether_package_dir}")
endif()

list(APPEND _aether_package_commands
    COMMAND "${CMAKE_COMMAND}"
        -DPACKAGE_DIR=${_aether_package_dir}
        -P "${CMAKE_SOURCE_DIR}/CMake/PrunePackageDevFiles.cmake"
    COMMAND "${CMAKE_COMMAND}"
        -DPACKAGE_DIR=${_aether_package_dir}
        -DEXPECT_PROJECT_PAK=${_aether_package_expect_project_pak}
        -DEXPECT_DOTNET=${AETHER_HAS_DOTNET}
        -DEXPECT_NETHOST=$<BOOL:${AETHER_NETHOST_DLL}>
        -DRUNTIME_EXE=$<TARGET_FILE_NAME:GameRuntime>
        -DEDITOR_EXE=$<TARGET_FILE_NAME:App>
        -P "${CMAKE_SOURCE_DIR}/CMake/VerifyAppPackage.cmake")

add_custom_target(PackageGame
    ${_aether_package_commands}
    DEPENDS GameRuntime
    COMMENT "Creating redistributable package at ${_aether_package_dir}"
    VERBATIM USES_TERMINAL)

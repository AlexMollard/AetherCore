file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE APP_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# ── Editor sources ──────────────────────────────────────────────────────────
# The full editor. Excludes only launcher/ (its LauncherMain.cpp is a second main()).
set(APP_EDITOR_SOURCES ${APP_SOURCES})
list(FILTER APP_EDITOR_SOURCES EXCLUDE REGEX "[/\\\\]launcher[/\\\\]")

# ── GameRuntime sources ─────────────────────────────────────────────────────
# Shipped runtime: no editor UI, no imgui (tooling-only; see rendering/IUiOverlay.hpp +
# the AETHERCORE_WITH_IMGUI gate in Application.cpp), no launcher, no project/ tooling.
set(APP_RUNTIME_SOURCES ${APP_SOURCES})
set(APP_RUNTIME_HEADERS ${APP_HEADERS})
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]debug[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]editor[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]layers[/\\\\]DebugLayer\\.cpp$")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]imgui[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "EditorGuiExports")  # ImGui bridge: editor-only, never in a shipped runtime
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]launcher[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]project[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]debug[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]editor[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]layers[/\\\\]DebugLayer\\.hpp$")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]imgui[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]launcher[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]project[/\\\\]")

# ── Launcher sources ────────────────────────────────────────────────────────
# Slim project-hub front end (own entry launcher/LauncherMain.cpp + LauncherLayer + shared
# ProjectCommon + app framework + imgui). Excludes the editor, C# host, scene systems, and
# serialization - it runs on a UiShell engine and only spawns Editor. From debug/ it keeps
# only ProjectLauncherWindow (its other deps are header-only).
set(APP_LAUNCHER_SOURCES ${APP_SOURCES})
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]main\\.cpp$")
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]PlaySession\\.cpp$")
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]debug[/\\\\]")
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]editor[/\\\\]")
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]scripting[/\\\\]")
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]systems[/\\\\]")
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]scene[/\\\\]")
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]layers[/\\\\]DebugLayer\\.cpp$")
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]layers[/\\\\]ScriptedSceneLayer\\.cpp$")
list(APPEND APP_LAUNCHER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/debug/ProjectLauncherWindow.cpp")
# Shared localhost ENet transport only. The Launcher supplies its own small method
# table in LauncherLayer.cpp; the editor-only ControlMethods.cpp stays excluded.
list(APPEND APP_LAUNCHER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/editor/ControlServer.cpp")

add_executable(Editor)
add_executable(GameRuntime)

target_sources(Editor
    PRIVATE
        ${APP_EDITOR_SOURCES}
        ${APP_HEADERS}
)

target_sources(GameRuntime
    PRIVATE
        ${APP_RUNTIME_SOURCES}
        ${APP_RUNTIME_HEADERS}
)

if(WIN32)
    set(AETHERCORE_APP_ICON_RC_PATH "${CMAKE_SOURCE_DIR}/resources/branding/aethercore-app-icon.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/AetherCoreEditor.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/AetherCoreEditor.rc"
        @ONLY
    )
    target_sources(Editor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/AetherCoreEditor.rc")
endif()

target_include_directories(Editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(GameRuntime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(Editor PRIVATE Engine AssetPipeline imgui)
target_link_libraries(GameRuntime PRIVATE Engine)

# Editor control endpoint (localhost JSON-over-ENet for the AetherCore MCP / aether-ctl).
# Linked ONLY into Editor - editor/ is excluded from GameRuntime, so a shipped game links
# neither ENet nor JSON.
target_link_libraries(Editor PRIVATE nlohmann_json::nlohmann_json enet)
if(WIN32)
    target_link_libraries(Editor PRIVATE ws2_32 winmm)
endif()

aethercore_target_defaults(Editor)
aethercore_target_defaults(GameRuntime)
aethercore_enable_slang_shader_compilation(Editor)
aethercore_enable_dead_strip_report(Editor)
aethercore_enable_dead_strip_report(GameRuntime)

set_target_properties(Editor PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    # F5 boots straight into TestingProject; the Launcher passes --project when it spawns.
    VS_DEBUGGER_COMMAND_ARGUMENTS "--project \"${AETHERCORE_PROJECT_DIR}\""
)
set_target_properties(GameRuntime PROPERTIES
    OUTPUT_NAME "AetherGame"
    # Published games are windowed applications and should not allocate a console.
    WIN32_EXECUTABLE TRUE
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
# GameRuntime shares a conventional `main` entry point with Editor. Preserve that
# entry point while selecting the Windows GUI subsystem above.
if(MSVC)
    target_compile_options(Editor PRIVATE /bigobj)
    target_compile_options(GameRuntime PRIVATE /bigobj)
	target_link_options(GameRuntime PRIVATE /ENTRY:mainCRTStartup)
endif()

target_compile_definitions(Editor PRIVATE
    AETHERCORE_EDITOR_APP=1
    # SCENE_APP wires ECS scene systems; WITH_IMGUI builds the overlay. Editor is both;
    # launcher is tooling-only; GameRuntime is scene-only.
    AETHERCORE_SCENE_APP=1
    AETHERCORE_WITH_IMGUI=1
    AETHER_ENGINE_RESOURCES_DIR="${CMAKE_SOURCE_DIR}/resources"
    AETHER_SHADER_BUILD_DIR="${AETHERCORE_SHADER_OUTPUT_DIR}"
    AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes"
    AETHER_PREFABS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/prefabs"
    AETHER_MANAGED_SDK_PROJECT="${CMAKE_SOURCE_DIR}/managed/AetherCore/AetherCore.csproj"
    AETHER_GAME_RUNTIME_EXE_NAME="$<TARGET_FILE_NAME:GameRuntime>"
    AETHER_LAUNCHER_EXE_NAME="$<TARGET_FILE_NAME:Launcher>"
    # So the publish verifier can reject a package that bundles the editor exe.
    AETHER_EDITOR_EXE_NAME="$<TARGET_FILE_NAME:Editor>"
)

# Dev-only project shader compilation (ShaderCompiler, src/app/editor). Defined only when
# CMake found slangc; otherwise ShaderCompiler::CanCompileShaders() is a graceful no-op.
# GameRuntime never gets it (editor/ is excluded from its sources).
if(SLANGC_EXECUTABLE)
    target_compile_definitions(Editor PRIVATE
        AETHER_SLANGC_EXE="${SLANGC_EXECUTABLE}"
        AETHER_SLANG_ARGS="${AETHERCORE_SLANG_SHADER_ARGS}"
        # Engine shader include dir, so project shaders can #include the engine's shared shader
        # headers ("include/DevicePointer.slangh", "include/FrameConstants.slangh", etc.).
        AETHER_SHADER_INCLUDE_DIR="${AETHERCORE_SHADER_SOURCE_DIR}")
endif()

target_compile_definitions(GameRuntime PRIVATE
    AETHERCORE_SCENE_APP=1
    AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes"
    AETHER_PREFABS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/prefabs"
    AETHER_MANAGED_SDK_PROJECT="${CMAKE_SOURCE_DIR}/managed/AetherCore/AetherCore.csproj"
)

# ── Managed (C#) assemblies ────────────────────────────────────────────────
# Build the engine C# projects and deploy them next to the app's data/ so DotNetHost can
# load them at startup. ArtifactsPath keeps obj/bin in the build tree; gated on
# AETHER_HAS_DOTNET so a machine without the .NET SDK still builds. We `dotnet build` the
# leaf AetherCore.Interop.csproj directly (it pulls in the SDK via ProjectReference), NOT
# the checked-in .slnx, so the build stays generator-agnostic. Project game scripts
# (AetherGame) are NOT built here - the editor builds them at runtime and publish bakes them.
if(AETHER_HAS_DOTNET)
    set(_aether_interop_proj "${CMAKE_SOURCE_DIR}/managed/AetherCore.Interop/AetherCore.Interop.csproj")
    set(_aether_managed_cfg "$<IF:$<CONFIG:Debug>,Debug,Release>")
    set(_aether_managed_cfgdir "$<IF:$<CONFIG:Debug>,debug,release>")
    # Interop's output dir carries both the boot assembly (+runtimeconfig) and the SDK dll.
    set(_aether_interop_out "${CMAKE_BINARY_DIR}/managed/bin/AetherCore.Interop/${_aether_managed_cfgdir}")

    add_custom_target(ManagedAssemblies ALL
        # MSVC's LIB environment variable can contain optional toolset paths that
        # are absent on some installations. They are irrelevant to C# compilation
        # but cause CSC to emit CS1668, so do not propagate LIB to dotnet.
        COMMAND "${CMAKE_COMMAND}" -E env --unset=LIB "${AETHER_DOTNET_EXE}" build "${_aether_interop_proj}"
                -c ${_aether_managed_cfg} --nologo -v:m
                -p:ArtifactsPath=${CMAKE_BINARY_DIR}/managed
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/scripts/managed"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_aether_interop_out}" "${CMAKE_BINARY_DIR}/data/scripts/managed"
        COMMENT "Building C# engine managed assemblies (AetherCore SDK + AetherCore.Interop)"
        VERBATIM USES_TERMINAL)

    foreach(_aether_runtime_target IN ITEMS Editor GameRuntime)
        add_dependencies(${_aether_runtime_target} ManagedAssemblies)
        target_link_libraries(${_aether_runtime_target} PRIVATE DotNet::NetHost)
    endforeach()

    # Build-machine facts the editor/publisher use to build a project's scripts at runtime
    # (CSharpScriptingSubsystem::RebuildFromSource / EditorProjectPublisher). Editor-only; a
    # packaged build leaves these undefined so a source rebuild is a reload-only no-op.
    target_compile_definitions(Editor PRIVATE
        AETHER_DOTNET_EXE="${AETHER_DOTNET_EXE}"
        AETHER_MANAGED_CONFIG="$<IF:$<CONFIG:Debug>,Debug,Release>"
        AETHER_MANAGED_CONFIGDIR="$<IF:$<CONFIG:Debug>,debug,release>")

    # (managed assemblies + nethost.dll are staged into the shared app bundle once by
    # StageAppBundle below, not copied per target.)
endif()

if(NVIDIA_AFTERMATH_FOUND)
    get_target_property(_am_dll Nvidia::Aftermath IMPORTED_LOCATION)
endif()

# ── Asset pipeline ─────────────────────────────────────────────────────────
# engine.pak (fonts + branding + compiled engine shaders) is engine-owned content every
# runtime target needs. EngineAssetsPak packs it exactly ONCE; each target then only COPIES
# it next to its exe. Do not pack the whole resources/ tree (may hold sample project
# content) - engine.pak stays engine-owned.
#
# project.pak (a project's baked content) is NOT built by any normal target: the editor
# mounts project:// at the raw project folder + bakes on demand, and the launcher loads no
# project. It is a shipping artifact, built only by PackageGame (below).
add_custom_target(EngineAssetsPak
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
        "${CMAKE_BINARY_DIR}/engine_assets"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_BINARY_DIR}/engine_assets/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/fonts"
        "${CMAKE_BINARY_DIR}/engine_assets/fonts"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_BINARY_DIR}/engine_assets/branding"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/branding/aethercore-icon-white.png"
        "${CMAKE_BINARY_DIR}/engine_assets/branding/aethercore-icon-white.png"
    # .spv ship inside engine.pak (shaders://, see FileSystem::InitializeDefaultMounts). The
    # make_directory keeps this working when shader compilation was skipped (no slangc / no
    # .slang) - copy_directory would otherwise hard-fail on a missing source.
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${AETHERCORE_SHADER_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_BINARY_DIR}/engine_assets/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${AETHERCORE_SHADER_OUTPUT_DIR}"
        "${CMAKE_BINARY_DIR}/engine_assets/shaders"
    COMMAND "$<TARGET_FILE:AssetPacker>"
        "${CMAKE_BINARY_DIR}/engine_assets"
        "${CMAKE_BINARY_DIR}/data/engine.pak"
    # Shipped default settings (layer 2), staged in build-root data/config so every target
    # and PackageGame copy from one place.
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/config/EngineSettings.toml"
        "${CMAKE_BINARY_DIR}/data/config/EngineSettings.toml"
    COMMENT "Packing engine runtime assets to data/engine.pak (once)"
    VERBATIM
)
# engine.pak embeds compiled .spv, so wait for CompileShaders. AssetPacker is a genex ref
# in the commands; make the tool dependency explicit for custom-target robustness.
if(TARGET CompileShaders)
    add_dependencies(EngineAssetsPak CompileShaders)
endif()
add_dependencies(EngineAssetsPak AssetPacker)

# ── App bundle staging ──────────────────────────────────────────────────────
# Editor, GameRuntime and Launcher share one output directory - the "editor bundle" the
# publisher ships wholesale (EditorProjectPublisher copies the exe dir as a unit). The
# runtime payload is staged into it exactly ONCE here, not copied per target; that removes
# the old 3x-redundant copies and the copy-race serialisation between the apps.
set(_aether_app_bundle_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")

set(_aether_stage_cmds
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aether_app_bundle_dir}/data/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_BINARY_DIR}/data/engine.pak" "${_aether_app_bundle_dir}/data/engine.pak"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_BINARY_DIR}/data/config/EngineSettings.toml" "${_aether_app_bundle_dir}/data/config/EngineSettings.toml"
)
if(AETHER_HAS_DOTNET)
    list(APPEND _aether_stage_cmds
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_BINARY_DIR}/data/scripts/managed" "${_aether_app_bundle_dir}/data/scripts/managed")
endif()
if(WIN32 AND AETHER_NETHOST_DLL)
    list(APPEND _aether_stage_cmds
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${AETHER_NETHOST_DLL}" "${_aether_app_bundle_dir}/nethost.dll")
endif()

add_custom_target(StageAppBundle ${_aether_stage_cmds}
    COMMENT "Staging the app bundle (engine.pak, settings, managed, nethost)"
    VERBATIM)
add_dependencies(StageAppBundle EngineAssetsPak)
if(AETHER_HAS_DOTNET)
    add_dependencies(StageAppBundle ManagedAssemblies)
endif()
add_dependencies(Editor StageAppBundle)
add_dependencies(GameRuntime StageAppBundle)

# Aftermath's DLL is dev-only + editor-only (VulkanContext gate + Engine's /DELAYLOAD);
# only the editor gets it, and the publisher explicitly skips it.
if(NVIDIA_AFTERMATH_FOUND)
    add_custom_command(TARGET Editor POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_am_dll}" "$<TARGET_FILE_DIR:Editor>"
        COMMENT "Copying GFSDK_Aftermath_Lib.x64.dll to the editor"
        VERBATIM)
endif()

if(AETHERCORE_PROJECT_DIR AND NOT EXISTS "${AETHERCORE_PROJECT_DIR}/ProjectSettings.toml")
    message(WARNING "AETHERCORE_PROJECT_DIR='${AETHERCORE_PROJECT_DIR}' is not an Aether project root; skipping data/project.pak")
endif()

# The editor's publish flow packages AetherGame.exe from its own (shared) dir, so the
# runtime must be built and co-located whenever the editor is.
add_dependencies(Editor GameRuntime)

# ── Launcher ────────────────────────────────────────────────────────────────
# The project hub, its own process built from APP_LAUNCHER_SOURCES (above). Runs on a
# UiShell engine and, on open/create, spawns a full-engine Editor process for the chosen
# project. It reuses only the generic local control transport; no editor methods, C# host,
# or scene systems are linked into the hub.
add_executable(Launcher)
target_sources(Launcher PRIVATE ${APP_LAUNCHER_SOURCES} ${APP_HEADERS})
target_include_directories(Launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Launcher PRIVATE Engine imgui nlohmann_json::nlohmann_json enet)
if(WIN32)
    target_link_libraries(Launcher PRIVATE ws2_32 winmm)
endif()

# Same Windows icon as the Editor (reuse the .rc configured above). 101 ICON is the
# Explorer/taskbar icon; GLFW_ICON is the runtime window icon GLFW loads.
if(WIN32)
    target_sources(Launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/AetherCoreEditor.rc")
endif()

# Tooling app (imgui) but NOT a scene app: no ECS systems, no CoreCLR. The editor name +
# build-injected dev path feed ResolveToolExecutable so the launcher can spawn the editor;
# the AETHER_* paths let ProjectCommon scaffold new projects.
target_compile_definitions(Launcher PRIVATE
    AETHERCORE_WITH_IMGUI=1
    AETHER_EDITOR_EXE_NAME="$<TARGET_FILE_NAME:Editor>"
    AETHER_EDITOR_EXE_PATH="${_aether_app_bundle_dir}/$<TARGET_FILE_NAME:Editor>"
    AETHER_MANAGED_SDK_PROJECT="${CMAKE_SOURCE_DIR}/managed/AetherCore/AetherCore.csproj"
    AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes")

aethercore_target_defaults(Launcher)
aethercore_enable_dead_strip_report(Launcher)
set_target_properties(Launcher PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

# The launcher shares the app bundle (engine.pak etc. staged by StageAppBundle) and spawns
# Editor.exe, so it depends on both.
add_dependencies(Launcher StageAppBundle)
add_dependencies(Launcher Editor)

# ── Redistributable package ─────────────────────────────────────────────────
# PackageGame creates a clean handoff folder (runtime exe + runtime DLLs + data/ only),
# separate from the developer build tree.
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
    # project.pak is built HERE (publish-only): no normal build produces it. Bake it
    # straight into the package so the standalone GameRuntime carries its content.
    list(APPEND _aether_package_commands
        COMMAND "$<TARGET_FILE:AssetPacker>"
            "--project"
            "--import-materials"
            "${AETHERCORE_PROJECT_DIR}"
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

# Aftermath's DLL is NOT packaged: GameRuntime never enables it, and VerifyAppPackage.cmake
# fails the build if a GFSDK_Aftermath* file ends up in the package.

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
        -DEDITOR_EXE=$<TARGET_FILE_NAME:Editor>
        -P "${CMAKE_SOURCE_DIR}/CMake/VerifyAppPackage.cmake")

add_custom_target(PackageGame
    ${_aether_package_commands}
    # GameRuntime (shipped exe) + EngineAssetsPak (build-root engine.pak/config) +
    # AssetPacker (bakes project.pak above).
    DEPENDS GameRuntime EngineAssetsPak AssetPacker
    COMMENT "Creating redistributable package at ${_aether_package_dir}"
    VERBATIM USES_TERMINAL)

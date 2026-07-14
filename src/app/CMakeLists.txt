file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE APP_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# ── Editor sources ──────────────────────────────────────────────────────────
# The full editor. Excludes only launcher/, whose LauncherMain.cpp would be a
# second main() - the editor neither spawns nor embeds the launcher.
set(APP_EDITOR_SOURCES ${APP_SOURCES})
list(FILTER APP_EDITOR_SOURCES EXCLUDE REGEX "[/\\\\]launcher[/\\\\]")

# ── GameRuntime sources ─────────────────────────────────────────────────────
# The shipped runtime: no editor UI, no imgui, no launcher, no in-app project
# tooling (project/ scaffolds/enumerates projects - an editor/launcher concern).
set(APP_RUNTIME_SOURCES ${APP_SOURCES})
set(APP_RUNTIME_HEADERS ${APP_HEADERS})
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]debug[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]editor[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]layers[/\\\\]DebugLayer\\.cpp$")
# Dear ImGui integration (src/app/imgui/) is tooling-only: GameRuntime must
# neither link nor initialize imgui (see rendering/IUiOverlay.hpp in Engine
# and the AETHERCORE_WITH_IMGUI-gated construction in Application.cpp).
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]imgui[/\\\\]")
# Launcher + project tooling are editor-family only; GameRuntime never spawns
# editors nor scaffolds projects.
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]launcher[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]project[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]debug[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]editor[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]layers[/\\\\]DebugLayer\\.hpp$")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]imgui[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]launcher[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]project[/\\\\]")

# ── Launcher sources ────────────────────────────────────────────────────────
# The slim project-hub front end: its own entry (launcher/LauncherMain.cpp), the
# LauncherLayer hub, the shared ProjectCommon, the app framework, and imgui. It
# deliberately excludes the editor (panels, publisher, shader compiler, control
# server, project manager), the C# scripting host, the app-level scene systems,
# and scene serialization - it runs on a UiShell engine and only spawns Editor
# processes. From debug/ it keeps only ProjectLauncherWindow (the hub widget,
# whose other deps - Icons/EditorChrome/Color - are header-only).
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

# ── Editor control endpoint deps (ENet transport + JSON) ────────────────────
# ControlServer (src/app/editor) exposes a localhost JSON-over-ENet control
# endpoint that the AetherCore MCP / aether-ctl drive. Both deps come from CPM
# (see CMake/Dependencies.cmake) and are linked ONLY into Editor: the editor/
# directory is excluded from GameRuntime's source list, so a shipped game links
# neither ENet nor the JSON library.
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
    # F5 boots straight into TestingProject (bypassing the launcher) - the editor is
    # always project-scoped. The Launcher passes --project when it spawns the editor.
    VS_DEBUGGER_COMMAND_ARGUMENTS "--project \"${AETHERCORE_PROJECT_DIR}\""
)
set_target_properties(GameRuntime PROPERTIES
    OUTPUT_NAME "AetherGame"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# In dev builds, let the scene serializer save/load committable scene and prefab
# files directly from the source tree.
target_compile_definitions(Editor PRIVATE
    AETHERCORE_EDITOR_APP=1
    # Scene app: wire ECS scene systems (Application.cpp). Tooling app: build the Dear
    # ImGui overlay. Both hold for the editor; the launcher is tooling but NOT a scene
    # app; the game runtime is a scene app but NOT tooling.
    AETHERCORE_SCENE_APP=1
    AETHERCORE_WITH_IMGUI=1
    AETHER_ENGINE_RESOURCES_DIR="${CMAKE_SOURCE_DIR}/resources"
    AETHER_SHADER_BUILD_DIR="${AETHERCORE_SHADER_OUTPUT_DIR}"
    AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes"
    AETHER_PREFABS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/prefabs"
    AETHER_MANAGED_SDK_PROJECT="${CMAKE_SOURCE_DIR}/managed/AetherCore/AetherCore.csproj"
    AETHER_GAME_RUNTIME_EXE_NAME="$<TARGET_FILE_NAME:GameRuntime>"
    # The editor's own file name, so the publish verifier rejects a game package
    # that wrongly bundles the editor executable (EditorProjectPublisher).
    AETHER_EDITOR_EXE_NAME="$<TARGET_FILE_NAME:Editor>"
)

# Dev-only project shader compilation (ShaderCompiler, src/app/editor). Only
# defined when CMake found slangc (aethercore_enable_slang_shader_compilation
# above populates the SLANGC_EXECUTABLE cache var) - absent in a shipped editor
# or on a machine without the Vulkan SDK, where ShaderCompiler::CanCompileShaders()
# degrades to a graceful no-op, mirroring the AETHER_HAS_DOTNET gating below for
# the C# rebuild-from-source path. GameRuntime never gets this define (or the
# ShaderCompiler translation unit at all - it lives under src/app/editor, which
# GameRuntime's source list excludes).
if(SLANGC_EXECUTABLE)
    target_compile_definitions(Editor PRIVATE
        AETHER_SLANGC_EXE="${SLANGC_EXECUTABLE}"
        AETHER_SLANG_ARGS="${AETHERCORE_SLANG_SHADER_ARGS}")
endif()

target_compile_definitions(GameRuntime PRIVATE
    # Scene app (wires ECS scene systems) but NOT a tooling app (no imgui overlay).
    AETHERCORE_SCENE_APP=1
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
# checked-in managed/AetherCore.slnx: the solution is only an IDE convenience (VS + Rider),
# so the CMake build stays generator-agnostic and cross-platform and never depends
# on a solution file. The C# projects are likewise NOT bolted onto this generated
# C++ solution via include_external_msproject - editing happens in managed/AetherCore.slnx.
# (CMake cannot generate SDK-style .csproj, so those are hand-authored source.)
if(AETHER_HAS_DOTNET)
    set(_aether_interop_proj "${CMAKE_SOURCE_DIR}/managed/AetherCore.Interop/AetherCore.Interop.csproj")
    set(_aether_managed_cfg "$<IF:$<CONFIG:Debug>,Debug,Release>")
    set(_aether_managed_cfgdir "$<IF:$<CONFIG:Debug>,debug,release>")
    # AetherCore.Interop's output carries the boot assembly + runtimeconfig AND the
    # SDK dll it references. Project game scripts (AetherGame) are deliberately NOT
    # built here: they belong to a project, so the editor builds them at runtime from
    # the OPEN project (CSharpScriptingSubsystem) and publish bakes them into the
    # shipped package. This keeps the engine build free of any single project.
    set(_aether_interop_out "${CMAKE_BINARY_DIR}/managed/bin/AetherCore.Interop/${_aether_managed_cfgdir}")

    # Build ONLY the engine managed code: AetherCore.Interop (boot) pulls in the
    # AetherCore SDK via ProjectReference, so its output dir carries both dlls.
    add_custom_target(ManagedAssemblies ALL
        COMMAND "${AETHER_DOTNET_EXE}" build "${_aether_interop_proj}"
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

    # Build-machine facts the editor + publisher use to build a PROJECT's game
    # scripts at runtime (CSharpScriptingSubsystem::RebuildFromSource /
    # EditorProjectPublisher). No project path is baked in: the editor supplies the
    # open project's scripts/AetherGame.csproj at runtime. Editor-only - GameRuntime
    # never builds scripts (it loads the deployed/baked assembly), and a packaged
    # build leaves these undefined so a source rebuild is a no-op (reload-only).
    target_compile_definitions(Editor PRIVATE
        AETHER_DOTNET_EXE="${AETHER_DOTNET_EXE}"
        AETHER_MANAGED_CONFIG="$<IF:$<CONFIG:Debug>,Debug,Release>"
        AETHER_MANAGED_CONFIGDIR="$<IF:$<CONFIG:Debug>,debug,release>")

    # Mirror the managed assemblies next to the executable for standalone runs.
    foreach(_aether_runtime_target IN ITEMS Editor GameRuntime)
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
        foreach(_aether_runtime_target IN ITEMS Editor GameRuntime)
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
# engine.pak (fonts + branding + compiled engine shaders) is engine-owned content
# that every runtime target needs. It is packed exactly ONCE by the EngineAssetsPak
# target below; each runtime target then only COPIES it (and the shipped settings)
# next to its executable. It used to be re-packed in every target's POST_BUILD - i.e.
# 2-3x per build, to the same output. Do not pack the whole resources/ tree: it may
# hold legacy/sample project content, whereas engine.pak must stay engine-owned.
#
# project.pak (a project's baked content) is deliberately NOT built by any normal
# target: the editor mounts project:// at the RAW project folder and bakes assets on
# demand, and the launcher loads no project at all - so nothing in the dev build needs
# a pre-baked project.pak. It is a shipping artifact, built only by PackageGame (below)
# for the standalone GameRuntime redistributable.
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
    # Engine .spv shaders ship inside engine.pak (under shaders/) so a published game
    # needs no loose .spv next to the exe - see shaders:// in FileSystem::Initialize
    # DefaultMounts. The make_directory keeps this working even when shader compilation
    # was skipped (no slangc / AETHERCORE_ENABLE_SLANG=OFF / no .slang sources - all
    # graceful early-returns in SlangShaders.cmake): copy_directory would otherwise
    # hard-fail on a missing source. An empty source copies nothing, so engine.pak just
    # ships without shaders in that case.
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
    # Shipped default settings (layer 2). Staged in build-root data/config so every
    # runtime target AND PackageGame copy from one place; the runtime resolves it
    # executable-relative at load, independent of CMake and the working directory.
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/config/EngineSettings.toml"
        "${CMAKE_BINARY_DIR}/data/config/EngineSettings.toml"
    COMMENT "Packing engine runtime assets to data/engine.pak (once)"
    VERBATIM
)
# CompileShaders is the only Slang compile target (registered on Editor); engine.pak
# embeds the compiled .spv, so wait for it. AssetPacker is referenced via a genex in
# the commands, but make the tool dependency explicit for custom-target robustness.
if(TARGET CompileShaders)
    add_dependencies(EngineAssetsPak CompileShaders)
endif()
add_dependencies(EngineAssetsPak AssetPacker)

# Deploy the shared engine.pak (+ its sidecar log/manifest) and shipped settings next
# to a runtime target's executable. Packing happens once, in EngineAssetsPak; this step
# only copies, so building Editor + GameRuntime + Launcher packs engine.pak a single
# time instead of once per target.
function(aethercore_add_runtime_payload target_name)
    add_dependencies(${target_name} EngineAssetsPak)
    add_custom_command(TARGET ${target_name} POST_BUILD
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
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target_name}>/data/config"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/data/config/EngineSettings.toml"
            "$<TARGET_FILE_DIR:${target_name}>/data/config/EngineSettings.toml"
        COMMENT "Deploying engine.pak + shipped settings next to ${target_name}"
        VERBATIM
    )

    # NVIDIA Aftermath DLL - dev-only GPU crash diagnostics, editor-only (see the
    # vendor/editor runtime gate in VulkanContext.cpp + the /DELAYLOAD link
    # option on Engine in src/engine/CMakeLists.txt). Only Editor calls into it, so
    # only Editor gets the DLL copied next to it; GameRuntime never enables
    # Aftermath and must not carry the DLL into a shipped game.
    if(NVIDIA_AFTERMATH_FOUND AND target_name STREQUAL "Editor")
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

aethercore_add_runtime_payload(Editor)
aethercore_add_runtime_payload(GameRuntime)
add_dependencies(Editor GameRuntime)

# ── Launcher ────────────────────────────────────────────────────────────────
# The project hub is its own process, built from its OWN slim source set
# (APP_LAUNCHER_SOURCES above): its own entry (launcher/LauncherMain.cpp), the
# LauncherLayer hub, the shared ProjectCommon, the app framework, and imgui. It runs
# on a UiShell engine (window + Vulkan + Dear ImGui only) and, on open/create, spawns
# a separate full-engine Editor process for the chosen project - which gives a real
# separate OS window that the ImGui multi-viewport path could not. It links neither
# the editor, the C# scripting host, nor the scene systems.
add_executable(Launcher)
target_sources(Launcher PRIVATE ${APP_LAUNCHER_SOURCES} ${APP_HEADERS})
target_include_directories(Launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Launcher PRIVATE Engine imgui)

# Give the Launcher the same Windows icon as the Editor by reusing the .rc
# configured above (WIN32 block near the top). 101 ICON is the Explorer/taskbar
# executable icon; GLFW_ICON is loaded by GLFW for the runtime window icon.
if(WIN32)
    target_sources(Launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/AetherCoreEditor.rc")
endif()

# Tooling app (imgui overlay) but NOT a scene app: no ECS scene systems, no CoreCLR.
# AETHER_EDITOR_EXE_NAME lets LauncherProcess spawn the editor; the two AETHER_* paths
# let ProjectCommon scaffold a new project (scripts .csproj + seed scene).
target_compile_definitions(Launcher PRIVATE
    AETHERCORE_WITH_IMGUI=1
    AETHER_EDITOR_EXE_NAME="$<TARGET_FILE_NAME:Editor>"
    AETHER_MANAGED_SDK_PROJECT="${CMAKE_SOURCE_DIR}/managed/AetherCore/AetherCore.csproj"
    AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes")

aethercore_target_defaults(Launcher)
aethercore_enable_dead_strip_report(Launcher)
set_target_properties(Launcher PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

# The launcher draws ImGui, so it needs engine.pak (fonts + the branding logo) beside
# it - exactly like the editor. It does NOT deploy the managed assemblies or nethost:
# it never boots CoreCLR. It spawns Editor.exe at runtime, so it depends on Editor.
aethercore_add_runtime_payload(Launcher)
add_dependencies(Launcher Editor)

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
    # project.pak is built HERE (publish-only): no normal build produces it, since the
    # editor uses the raw project folder + on-demand baking. Bake it straight into the
    # package so the standalone GameRuntime redistributable carries its content.
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

# NVIDIA Aftermath's DLL is deliberately NOT packaged here: PackageGame builds
# a GameRuntime redistributable, and GameRuntime never enables Aftermath (dev-
# only diagnostics, editor-gated - see VulkanContext.cpp + Engine's /DELAYLOAD
# link option) and does not need the DLL to launch. VerifyAppPackage.cmake
# fails the build if a GFSDK_Aftermath* file ever ends up in the package.

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
    # GameRuntime (the shipped exe) + EngineAssetsPak (build-root engine.pak/config this
    # copies) + AssetPacker (used above to bake project.pak into the package).
    DEPENDS GameRuntime EngineAssetsPak AssetPacker
    COMMENT "Creating redistributable package at ${_aether_package_dir}"
    VERBATIM USES_TERMINAL)

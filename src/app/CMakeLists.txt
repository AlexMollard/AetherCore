file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE APP_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# ── Editor sources ──────────────────────────────────────────────────────────
# The full editor. Excludes only launcher/ (its LauncherMain.cpp is a second main()).
set(APP_EDITOR_SOURCES ${APP_SOURCES})
list(FILTER APP_EDITOR_SOURCES EXCLUDE REGEX "[/\\\\]launcher[/\\\\]")

# ── GameRuntime sources ─────────────────────────────────────────────────────
# Shipped runtime: no editor UI, no imgui (tooling-only; see rendering/IUiOverlay.hpp +
# the AETHERCORE_WITH_IMGUI gate in Application.cpp), no launcher. project/ STAYS in:
# it is runtime code (ProjectPaths, ProjectStartupScene, ProjectCommon), used by
# RuntimeProjectSettings and EngineTests - not editor tooling.
set(APP_RUNTIME_SOURCES ${APP_SOURCES})
set(APP_RUNTIME_HEADERS ${APP_HEADERS})
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]debug[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]editor[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]layers[/\\\\]DebugLayer\\.cpp$")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]imgui[/\\\\]")
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "EditorGuiExports")  # ImGui bridge: editor-only, never in a shipped runtime
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]launcher[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]debug[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]editor[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]layers[/\\\\]DebugLayer\\.hpp$")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]imgui[/\\\\]")
list(FILTER APP_RUNTIME_HEADERS EXCLUDE REGEX "[/\\\\]launcher[/\\\\]")

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
# The hub links no networking at all, and net/ reaches into scripting/ and systems/,
# both already excluded above - exclude the whole directory rather than chase it TU by TU.
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]net[/\\\\]")
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]layers[/\\\\]DebugLayer\\.cpp$")
list(FILTER APP_LAUNCHER_SOURCES EXCLUDE REGEX "[/\\\\]layers[/\\\\]ScriptedSceneLayer\\.cpp$")
list(APPEND APP_LAUNCHER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/debug/ProjectLauncherWindow.cpp")
# Shared localhost ENet transport only. The Launcher supplies its own small method
# table in LauncherLayer.cpp; the editor-only ControlMethods.cpp stays excluded.
list(APPEND APP_LAUNCHER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/editor/ControlServer.cpp")
# ControlServer's refusal text lives here, and it is pure string/JSON work over the
# method table - no editor dependency, so the slim Launcher can take it. It has to be
# listed: editor/ is excluded wholesale above, so a file ControlServer.cpp needs is not
# picked up just because it sits next to it.
list(APPEND APP_LAUNCHER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/editor/ControlDiagnostics.cpp")

# ── RuntimeFramework ────────────────────────────────────────────────────────
# The runtime source set, compiled ONCE and consumed by Editor, GameRuntime and EngineTests.
# Before this library, Editor and GameRuntime compiled most of src/app identically and
# EngineTests compiled a third copy by path; measured on a clean build, 20% of all compile
# time went on recompiling the same sources for a second and third target.
#
# It can be shared because almost nothing in src/app actually reads the macros that differ
# between the targets: AETHERCORE_EDITOR_APP, AETHERCORE_WITH_IMGUI, AETHERCORE_SCENE_APP
# and AETHER_SCRIPT_BUILD_ENABLED are consumed by exactly four files, listed below, and
# those stay per-target. Everything else produces byte-identical objects either way.
#
# An OBJECT library rather than STATIC, deliberately: a static library only contributes the
# objects something references, and much of this code registers itself through static
# initialisers - the component reflection tables above all. Those objects have no referenced
# symbol, so a static library would drop them and the components would quietly disappear.
set(APP_CONFIG_SENSITIVE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Application.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/layers/ScriptedSceneLayer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripting/CSharpScriptingSubsystem.cpp"
)

set(APP_FRAMEWORK_SOURCES ${APP_RUNTIME_SOURCES})
list(REMOVE_ITEM APP_FRAMEWORK_SOURCES ${APP_CONFIG_SENSITIVE_SOURCES})

add_library(RuntimeFramework OBJECT ${APP_FRAMEWORK_SOURCES} ${APP_RUNTIME_HEADERS})
# PUBLIC: the OBJECT lib needs src/app to compile its own sources, and consumers
# (Editor/GameRuntime/EngineTests) get it transitively - no per-target copy needed.
target_include_directories(RuntimeFramework PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(RuntimeFramework PUBLIC Engine)
# Header-only, so this adds nothing to a shipped binary. Needed because runtime sources
# (PlaySession.cpp -> editor/UndoStack.hpp -> debug/EditorCommand.hpp) transitively include
# <nlohmann/json.hpp>; the Editor previously supplied it per-target.
# shortcut: runtime still compiles the editor command header; split PlaySession's
# UndoStack usage into a runtime-owned dirty flag when the editor moves out for real.
target_link_libraries(RuntimeFramework PUBLIC nlohmann_json::nlohmann_json)
# Definitions every consumer agrees on, carried INTERFACE so Editor/GameRuntime/EngineTests
# all see them. Anything target-specific belongs on the target - if a shared source ever
# needs one, it stops being shared and moves to APP_CONFIG_SENSITIVE_SOURCES.
target_compile_definitions(RuntimeFramework INTERFACE
    AETHERCORE_SCENE_APP=1
    AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes"
    AETHER_PREFABS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/prefabs"
    AETHER_MANAGED_SDK_PROJECT="${CMAKE_SOURCE_DIR}/managed/AetherCore/AetherCore.csproj"
)
aethercore_target_defaults(RuntimeFramework)

# What each executable still compiles for itself: its own sources, minus everything the
# framework objects already cover. Both keep the four config-sensitive files, compiled with
# their own target definitions.
list(REMOVE_ITEM APP_EDITOR_SOURCES ${APP_FRAMEWORK_SOURCES})
list(REMOVE_ITEM APP_RUNTIME_SOURCES ${APP_FRAMEWORK_SOURCES})

# Shader compilation is declared independently of the Editor: engine.pak baking (below)
# depends on CompileShaders even in runtime-only builds where AETHERCORE_BUILD_EDITOR=OFF.
aethercore_declare_shader_compilation()

if(AETHERCORE_BUILD_EDITOR)
add_executable(Editor)
target_sources(Editor
    PRIVATE
        ${APP_EDITOR_SOURCES}
        ${APP_HEADERS})
endif()
add_executable(GameRuntime)
target_sources(GameRuntime
    PRIVATE
        ${APP_RUNTIME_SOURCES}
        ${APP_RUNTIME_HEADERS})

if(WIN32 AND (AETHERCORE_BUILD_EDITOR OR AETHERCORE_BUILD_LAUNCHER))
    set(AETHERCORE_APP_ICON_RC_PATH "${CMAKE_SOURCE_DIR}/resources/branding/aethercore-app-icon.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/AetherCoreEditor.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/AetherCoreEditor.rc"
        @ONLY
    )
    if(AETHERCORE_BUILD_EDITOR)
        target_sources(Editor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/AetherCoreEditor.rc")
    endif()
endif()

# src/app headers come from RuntimeFramework's INTERFACE include dir; no per-target copy.

if(AETHERCORE_BUILD_EDITOR)
target_link_libraries(Editor PRIVATE Engine AssetPipeline imgui RuntimeFramework)
endif()
target_link_libraries(GameRuntime PRIVATE Engine RuntimeFramework)

# Editor control endpoint (localhost JSON-over-ENet for the AetherCore MCP / aether-ctl).
# Linked ONLY into Editor - editor/ is excluded from GameRuntime, so a shipped game links
# neither ENet nor JSON.
if(AETHERCORE_BUILD_EDITOR)
target_link_libraries(Editor PRIVATE nlohmann_json::nlohmann_json enet)
if(WIN32)
    target_link_libraries(Editor PRIVATE ws2_32 winmm)
endif()
endif()

if(AETHERCORE_BUILD_EDITOR)
aethercore_target_defaults(Editor)
endif()
aethercore_target_defaults(GameRuntime)
if(AETHERCORE_BUILD_EDITOR)
aethercore_enable_slang_shader_compilation(Editor)
aethercore_enable_dead_strip_report(Editor)
endif()
aethercore_enable_dead_strip_report(GameRuntime)

if(AETHERCORE_BUILD_EDITOR)
set_target_properties(Editor PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    # F5 boots straight into TestingProject; the Launcher passes --project when it spawns.
    VS_DEBUGGER_COMMAND_ARGUMENTS "--project \"${AETHERCORE_PROJECT_DIR}\""
)
endif()
set_target_properties(GameRuntime PROPERTIES
    OUTPUT_NAME "AetherGame"
    # Published games are windowed applications and should not allocate a console.
    WIN32_EXECUTABLE TRUE
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
# GameRuntime shares a conventional `main` entry point with Editor. Preserve that
# entry point while selecting the Windows GUI subsystem above.
if(MSVC)
    if(AETHERCORE_BUILD_EDITOR)
        target_compile_options(Editor PRIVATE /bigobj)
    endif()
    target_compile_options(GameRuntime PRIVATE /bigobj)
	target_link_options(GameRuntime PRIVATE /ENTRY:mainCRTStartup)
endif()

if(AETHERCORE_BUILD_EDITOR)
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
    # Cache strings (root CMakeLists), not $<TARGET_FILE_NAME> genexes, so the publish
    # verifier's expectations survive a runtime-only configure without the real targets.
    AETHER_GAME_RUNTIME_EXE_NAME="${AETHER_GAME_RUNTIME_EXE_NAME}"
    # So the publish verifier can reject a package that bundles the editor exe.
    AETHER_EDITOR_EXE_NAME="${AETHER_EDITOR_EXE_NAME}"
    # The Editor's CWD is the build-tree root (see AGENTS.md), not the repo root, so a
    # plain relative path from CWD would resolve to the wrong place - the same problem
    # AETHER_SHADER_INCLUDE_DIR below already solves for shaders. Read by
    # debug/KenneyBrowserPanel.cpp.
    AETHERCORE_KENNEY_MANIFEST="${CMAKE_SOURCE_DIR}/tools/assetpack/kenney_packs.toml"
    AETHERCORE_KENNEY_CACHE_DIR="${CMAKE_SOURCE_DIR}/.temp/kenney-cache"
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

    add_dependencies(GameRuntime ManagedAssemblies)
    target_link_libraries(GameRuntime PRIVATE DotNet::NetHost)
    if(AETHERCORE_BUILD_EDITOR)
        add_dependencies(Editor ManagedAssemblies)
        target_link_libraries(Editor PRIVATE DotNet::NetHost)
    endif()

    # Compiling a project's scripts is an EDITOR capability, and this is what says so. A
    # shipped GameRuntime loads the assembly it was given and never builds one, so it must
    # not carry the compiler path at all.
    #
    # The compiling itself is done in-process by Roslyn (managed/AetherCore.Interop/
    # ScriptCompiler.cs), not by invoking `dotnet` - which is why the editor runs on a
    # machine with no .NET SDK on it.
    if(AETHERCORE_BUILD_EDITOR)
        target_compile_definitions(Editor PRIVATE
            AETHER_SCRIPT_BUILD_ENABLED=1
            AETHER_MANAGED_CONFIG="$<IF:$<CONFIG:Debug>,Debug,Release>"
            AETHER_MANAGED_CONFIGDIR="$<IF:$<CONFIG:Debug>,debug,release>")
    endif()

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
# project. It is a shipping artifact, produced by the editor's publish flow only.
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
    # Prototype greybox textures (Kenney CC0 "Prototype Textures", see
    # resources/textures/prototype/CREDITS.md): the default material every
    # newly-added primitive wears until a real one is assigned, so it needs to
    # ship in engine.pak the same way fonts/branding do.
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_BINARY_DIR}/engine_assets/textures/prototype"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/textures/prototype"
        "${CMAKE_BINARY_DIR}/engine_assets/textures/prototype"
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
    # and the editor's publish staging copy from one place.
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
# The bundle IS the executable's own directory: EditorProjectPublisher takes
# GetExecutableDir() and ships it wholesale. Multi-config generators (Visual Studio) put the
# exe under <config>/, single-config ones (Ninja) put it straight in the target binary dir -
# so hard-coding the config subdirectory staged the payload one level BELOW the exe under
# Ninja. Publishing then failed looking for src/app/data/engine.pak, and nethost.dll never
# landed beside the editor, which is why launching it needed the dotnet host pack on PATH.
get_property(_aether_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_aether_multi_config)
    set(_aether_app_bundle_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
else()
    set(_aether_app_bundle_dir "${CMAKE_CURRENT_BINARY_DIR}")
endif()

# The runtime payload, declared ONCE. Two consumers read it: the build-tree staging
# below, and the install rules that produce a redistributable. Those used to be the same
# list written twice, which is a list that drifts - and the failure mode is the worst
# kind, a release missing one file that every developer's machine still has lying around
# from their build tree.
#
# Each entry is "<kind>|<source>|<destination relative to the executable>", where kind is
# `file` for one file or `dir` for a directory's contents.
set(_aether_payload
    "file|${CMAKE_BINARY_DIR}/data/engine.pak|data/engine.pak"
    "file|${CMAKE_BINARY_DIR}/data/config/EngineSettings.toml|data/config/EngineSettings.toml"

    # What "New Project" copies FROM. These used to be read straight out of the source
    # tree through AETHER_SCENES_SOURCE_DIR and friends, which is a path that exists only
    # on the machine that built the engine - so scaffolding a project worked for a
    # developer and failed for everyone who was handed the binaries. Shipped here, and
    # found through PlatformPaths::ResolveBundlePath, they travel with the executable.
    # Only scenes: resources/prefabs does not exist and never has, so
    # AETHER_PREFABS_SOURCE_DIR has always named a dead path. EnginePrefabTemplatesDir
    # resolves to nothing and the prefab lookup falls through to the settings-resolved
    # location, which is what it should always have done.
    "dir|${CMAKE_SOURCE_DIR}/resources/scenes|data/templates/scenes"

    # The SDK C# SOURCE, not just the built assembly: a generated game .csproj carries a
    # ProjectReference to it, and the generated solution pairs the two so an IDE resolves
    # engine types. Referencing the shipped AetherCore.dll instead would work for the
    # compile and lose both. The nesting mirrors the source tree (managed/AetherCore next
    # to managed/Directory.Build.props) because the csproj resolves the props by walking
    # up from itself - flattening it here would silently drop TargetFramework.
    "dir|${CMAKE_SOURCE_DIR}/managed/AetherCore|data/sdk/managed/AetherCore"
    "file|${CMAKE_SOURCE_DIR}/managed/Directory.Build.props|data/sdk/managed/Directory.Build.props"
)
if(AETHER_HAS_DOTNET)
    list(APPEND _aether_payload "dir|${CMAKE_BINARY_DIR}/data/scripts/managed|data/scripts/managed")
endif()
if(WIN32 AND AETHER_NETHOST_DLL)
    list(APPEND _aether_payload "file|${AETHER_NETHOST_DLL}|nethost.dll")
endif()

set(_aether_stage_cmds "")
foreach(_entry IN LISTS _aether_payload)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _kind)
    list(GET _parts 1 _src_raw)
    list(GET _parts 2 _dst)
    # nethost.dll is found through the Windows environment, so its path arrives with
    # backslashes. install() re-parses its arguments as CMake source, where "\P" of
    # "C:\Program Files" is an invalid escape and a hard configure error.
    file(TO_CMAKE_PATH "${_src_raw}" _src)
    if(_kind STREQUAL "dir")
        list(APPEND _aether_stage_cmds
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_src}" "${_aether_app_bundle_dir}/${_dst}")
        install(DIRECTORY "${_src}/" DESTINATION "${_dst}" COMPONENT Runtime)
    else()
        list(APPEND _aether_stage_cmds
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src}" "${_aether_app_bundle_dir}/${_dst}")
        get_filename_component(_dst_dir "${_dst}" DIRECTORY)
        get_filename_component(_dst_name "${_dst}" NAME)
        # A payload entry that sits at the bundle root (nethost.dll) has no directory
        # part, and install(FILES) rejects an empty DESTINATION.
        if(_dst_dir STREQUAL "")
            set(_dst_dir ".")
        endif()
        get_filename_component(_src_name "${_src}" NAME)
        if(_src_name STREQUAL _dst_name)
            install(FILES "${_src}" DESTINATION "${_dst_dir}" COMPONENT Runtime)
        else()
            install(FILES "${_src}" DESTINATION "${_dst_dir}" RENAME "${_dst_name}" COMPONENT Runtime)
        endif()
    endif()
endforeach()

add_custom_target(StageAppBundle ${_aether_stage_cmds}
    COMMENT "Staging the app bundle (engine.pak, settings, templates, SDK, managed, nethost)"
    VERBATIM)
add_dependencies(StageAppBundle EngineAssetsPak)
if(AETHER_HAS_DOTNET)
    add_dependencies(StageAppBundle ManagedAssemblies)
endif()
if(AETHERCORE_BUILD_EDITOR)
add_dependencies(Editor StageAppBundle)
endif()
add_dependencies(GameRuntime StageAppBundle)


# Aftermath's DLL is dev-only + editor-only (VulkanContext gate + Engine's /DELAYLOAD);
# only the editor gets it, and the publisher explicitly skips it.
if(NVIDIA_AFTERMATH_FOUND AND TARGET Editor)
    add_custom_command(TARGET Editor POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_am_dll}" "$<TARGET_FILE_DIR:Editor>"
        COMMENT "Copying GFSDK_Aftermath_Lib.x64.dll to the editor"
        VERBATIM)
endif()

if(AETHERCORE_PROJECT_DIR AND NOT EXISTS "${AETHERCORE_PROJECT_DIR}/ProjectSettings.toml")
    message(WARNING "AETHERCORE_PROJECT_DIR='${AETHERCORE_PROJECT_DIR}' is not an Aether project root; the dev runtime will boot with no project.")
endif()

# The editor's publish flow packages AetherGame.exe from its own (shared) dir, so the
# runtime must be built and co-located whenever the editor is.
if(AETHERCORE_BUILD_EDITOR)
add_dependencies(Editor GameRuntime)
endif()

# ── Launcher ────────────────────────────────────────────────────────────────
# The project hub, its own process built from APP_LAUNCHER_SOURCES (above). Runs on a
# UiShell engine and, on open/create, spawns a full-engine Editor process for the chosen
# project. It reuses only the generic local control transport; no editor methods, C# host,
# or scene systems are linked into the hub.
if(AETHERCORE_BUILD_LAUNCHER)
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
# the AETHER_* paths let ProjectCommon scaffold new projects. The editor exe name is a
# cache string (root CMakeLists) so it resolves even without the Editor target.
target_compile_definitions(Launcher PRIVATE
    AETHERCORE_WITH_IMGUI=1
    AETHER_EDITOR_EXE_NAME="${AETHER_EDITOR_EXE_NAME}"
    AETHER_EDITOR_EXE_PATH="${_aether_app_bundle_dir}/${AETHER_EDITOR_EXE_NAME}"
    AETHER_MANAGED_SDK_PROJECT="${CMAKE_SOURCE_DIR}/managed/AetherCore/AetherCore.csproj"
    AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes")

aethercore_target_defaults(Launcher)
aethercore_enable_dead_strip_report(Launcher)
set_target_properties(Launcher PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

# The launcher shares the app bundle (engine.pak etc. staged by StageAppBundle) and spawns
# Editor.exe, so it depends on both.
add_dependencies(Launcher StageAppBundle)
if(TARGET Editor)
add_dependencies(Launcher Editor)
endif()
endif()

# The three executables a redistributable carries. Deliberately NOT "everything in the
# bundle directory": that also holds import libraries, PDBs, CMake scaffolding and the
# Aftermath DLL, which is a dev-only crash-analysis dependency the publisher already
# refuses to ship.
#
# Everything here is in the `Runtime` component so that `cmake --install --component
# Runtime` yields ONLY the app. The third-party libraries CPM fetches register install
# rules of their own, and an uncomponented install collects all of them - the first
# attempt produced 51 MB of Jolt/box2d/freetype headers and import libraries around a
# handful of actual product files.
if(AETHERCORE_BUILD_LAUNCHER)
    install(TARGETS Launcher RUNTIME DESTINATION "." COMPONENT Runtime)
endif()
if(AETHERCORE_BUILD_EDITOR)
    install(TARGETS Editor RUNTIME DESTINATION "." COMPONENT Runtime)
endif()
install(TARGETS GameRuntime RUNTIME DESTINATION "." COMPONENT Runtime)

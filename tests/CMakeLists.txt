# Engine/tooling unit tests. Test sources are globbed (CONFIGURE_DEPENDS) like the rest of
# the build; the app-side TUs below live in src/app and are listed explicitly since they
# can't be globbed from here.
file(GLOB_RECURSE ENGINE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(EngineTests
    ${ENGINE_TEST_SOURCES}
    # The shared runtime/framework TUs (scene, scene/reflection, net, scripting,
    # systems, project, materialgraph, EngineContentPaths, RuntimeProjectSettings) come
    # from the RuntimeFramework OBJECT library below - no third copy is compiled here.
    #
    # What remains listed is either editor/tooling-only code the framework does not
    # carry (src/app/editor, src/app/debug, src/app/imgui) or a per-target file
    # (CSharpScriptingSubsystem.cpp - macro-config-sensitive, see src/app/CMakeLists.txt).
    "${CMAKE_SOURCE_DIR}/src/app/editor/ShaderCompiler.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/debug/ScenePicker.cpp"
    # Autosave's recovery rules: pure filesystem logic, and the half where a mistake
    # destroys work rather than merely failing to save it.
    "${CMAKE_SOURCE_DIR}/src/app/editor/AutosaveRecovery.cpp"
    # What a refused control call says: pure text over pure data, and the difference
    # between a caller that can fix the call and one that has to go and read describe.
    "${CMAKE_SOURCE_DIR}/src/app/editor/ControlDiagnostics.cpp"
    # Compile/link-only: the managed assemblies are never staged next to EngineTests.exe,
    # so scripting has no scripts to load here. (nethost.dll IS staged by the POST_BUILD
    # below - Engine imports it unconditionally once the .NET host pack is found.) The
    # macro that turns on project-script compilation (AETHER_SCRIPT_BUILD_ENABLED) is an
    # Editor-only define and is deliberately NOT set here. DotNetHost.cpp
    # (src/engine/scripting) needs no listing - it is globbed into Engine already.
    "${CMAKE_SOURCE_DIR}/src/app/scripting/CSharpScriptingSubsystem.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/imgui/ImguiFrameData.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/editor/AsepriteSpriteImporter.cpp"
    # Publish planning and package verification. Pure/near-pure halves of the publish
    # pipeline; the IO steps stay out of the test build.
    "${CMAKE_SOURCE_DIR}/src/app/editor/publish/PublishPlan.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/editor/publish/PublishVerify.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/imgui/UiInputScript.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/imgui/UiAutomation.cpp"
    # Editor undo/redo command logic under test. The typed component commands
    # (add/remove/set) resolve components through the editor catalog and the
    # hand-authored field sets, so both TUs are compiled in alongside them.
    "${CMAKE_SOURCE_DIR}/src/app/editor/UndoStack.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/debug/EditorCommand.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/editor/ComponentCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/editor/ComponentFields.cpp"
)
target_link_libraries(EngineTests PRIVATE Engine AssetPipeline RuntimeFramework doctest::doctest imgui nlohmann_json::nlohmann_json)
# Engine carries a real DotNetHost (AETHER_HAS_DOTNET is a PUBLIC define), so this exe
# imports nethost.dll even though no test touches scripting. Stage the DLL beside it;
# IsAvailable() may then return true, but no test depends on it being false.
if(AETHER_HAS_DOTNET AND WIN32)
    add_custom_command(TARGET EngineTests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${AETHER_NETHOST_DLL}" "$<TARGET_FILE_DIR:EngineTests>"
        COMMENT "Staging nethost.dll next to EngineTests")
endif()
target_include_directories(EngineTests PRIVATE "${xxHash_SOURCE_DIR}")   # pak index-hash tests
# Box2D headers for the direct dependency smoke test in tests/physics2d (Engine links
# box2d PRIVATE; everything else goes through Physics2DSystem).
target_include_directories(EngineTests PRIVATE "${box2d_SOURCE_DIR}/include")
# Mirror App's scenes dir so ReadSceneFile("default") resolves the committed template.
target_compile_definitions(EngineTests PRIVATE AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes")
target_compile_definitions(EngineTests PRIVATE AETHER_TESTS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/tests")
# The generated-shader compile test drives the editor's ShaderCompiler, which needs the same
# slangc, arguments and engine include dir the app is given. Absent when CMake found no
# slangc, and the test skips itself in that case.
if(SLANGC_EXECUTABLE)
    target_compile_definitions(EngineTests PRIVATE
        AETHER_SLANGC_EXE="${SLANGC_EXECUTABLE}"
        AETHER_SLANG_ARGS="${AETHERCORE_SLANG_SHADER_ARGS}"
        AETHER_SHADER_INCLUDE_DIR="${AETHERCORE_SHADER_SOURCE_DIR}")
endif()

aethercore_target_defaults(EngineTests)
if(MSVC)
    target_compile_options(EngineTests PRIVATE /bigobj)
    # Incremental links of this target intermittently produce a binary whose
    # indirect calls trip CFG (FAST_FAIL_GUARD_ICALL_CHECK_FAILURE) after a
    # test-TU-only rebuild. Full links are cheap here; force them.
    target_link_options(EngineTests PRIVATE /INCREMENTAL:NO)
endif()
set_target_properties(EngineTests PROPERTIES FOLDER "Tests")
add_test(NAME EngineTests COMMAND EngineTests)

# ── Managed (C#) SDK tests ──────────────────────────────────────────────────
# The xUnit suite over managed/AetherCore (the C# SDK). Registered as a CTest test so
# one `ctest` run covers both suites; `dotnet test` restores and builds the project
# itself, so nothing is added to the default build and a developer who never runs the
# tests pays nothing for them.
#
# Gated on AETHER_HAS_DOTNET exactly like every other managed step: with no .NET SDK on
# the machine the test is simply not registered, and the C++ build is untouched. The
# .csproj is invoked directly, never the checked-in .slnx, so the build stays
# generator-agnostic (see the managed assemblies block in src/app/CMakeLists.txt).
#
# ArtifactsPath is its own directory rather than the engine's managed/ one: the test run
# must never be able to overwrite the assemblies the editor and the runtime load.
if(AETHER_HAS_DOTNET)
    add_test(NAME ManagedTests
        COMMAND "${CMAKE_COMMAND}" -E env --unset=LIB
                "${AETHER_DOTNET_EXE}" test
                "${CMAKE_SOURCE_DIR}/managed/AetherCore.Tests/AetherCore.Tests.csproj"
                -c $<IF:$<CONFIG:Debug>,Debug,Release> --nologo
                -p:ArtifactsPath=${CMAKE_BINARY_DIR}/managed-tests)
    set_tests_properties(ManagedTests PROPERTIES TIMEOUT 900)
else()
    message(STATUS "AetherCore: .NET SDK not found - managed SDK tests skipped")
endif()

# Engine/tooling unit tests. Test sources are globbed (CONFIGURE_DEPENDS) like the rest of
# the build; the app-side TUs below live in src/app and are listed explicitly since they
# can't be globbed from here.
file(GLOB_RECURSE ENGINE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(EngineTests
    ${ENGINE_TEST_SOURCES}
    # App-side TUs under test: EngineTests links Engine only, so the scene serializer (plus
    # the reflection TUs that populate the component registry at static-init) and the
    # ImguiFrameData type its test exercises are compiled straight in here.
    "${CMAKE_SOURCE_DIR}/src/app/EngineContentPaths.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/editor/ShaderCompiler.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/materialgraph/MaterialGraph.cpp"
"${CMAKE_SOURCE_DIR}/src/app/scene/SceneSerializer.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/SceneSerializerApply.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/SceneSerializerCapture.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/SceneSerializerToml.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/SceneComponentSerde.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/serde/EffectSerde.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/serde/CameraSerde.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/serde/ScriptSerde.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/serde/MaterialSerde.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/serde/RenderSerde.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/serde/PhysicsSerde.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/serde/UiImageSerde.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/serde/MeshSerde.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/SceneWorkflow.cpp"
    # Autosave's recovery rules: pure filesystem logic, and the half where a mistake
    # destroys work rather than merely failing to save it.
    "${CMAKE_SOURCE_DIR}/src/app/editor/AutosaveRecovery.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/reflection/Reflection.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/reflection/CoreComponents.reflect.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/reflection/MoreComponents.reflect.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/reflection/Physics.reflect.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/reflection/Physics2D.reflect.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/net/NetSerialize.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/net/ReplicationSchema.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/net/NetSession.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/net/NetSnapshot.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/net/NetRelevancy.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/net/NetInterpolation.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/net/NetSpawn.cpp"
    # NetScriptFields only, NOT CSharpScriptFieldBridge.cpp: the packet logic goes
    # through the abstract ScriptFieldBridge so it links without the CoreCLR host.
    "${CMAKE_SOURCE_DIR}/src/app/net/NetScriptFields.cpp"
    # NetRpc only, NOT CSharpRpcBridge.cpp/NetRpcExports.cpp: encode/decode is pure
    # and links without the CoreCLR host; the dispatch bridge does not.
    "${CMAKE_SOURCE_DIR}/src/app/net/NetRpc.cpp"
    # The integration layer. Both TUs became linkable here once NetworkContext stopped
    # constructing the CLR-backed script bridges itself and started taking them from
    # its wiring site (Application.cpp) - see NetworkContext::SetFieldBridge. This is
    # where the role gating, the net-id lifecycle and the session teardown rules live,
    # so it is the half of the framework most worth pinning.
    "${CMAKE_SOURCE_DIR}/src/app/net/NetworkContext.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/net/NetworkSystems.cpp"
    # The one definition of g_activeContext, which ActiveContextScope publishes and
    # every script/RPC callback path depends on.
    "${CMAKE_SOURCE_DIR}/src/app/scripting/SceneContext.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scripting/modules/TagSlots.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/imgui/ImguiFrameData.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/editor/AsepriteSpriteImporter.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/project/ProjectPaths.cpp"
    # The one store for the startup scene, shared by the editor panels and the publish
    # bake - the rule that a build never boots a scene the project does not have.
    "${CMAKE_SOURCE_DIR}/src/app/project/ProjectStartupScene.cpp"
    # Which project file a game runtime layers over its own settings - the rule that
    # keeps a published build off the build machine's project directory.
    "${CMAKE_SOURCE_DIR}/src/app/RuntimeProjectSettings.cpp"
    # Publish planning and package verification. Pure/near-pure halves of the publish
    # pipeline; the IO steps stay out of the test build.
    "${CMAKE_SOURCE_DIR}/src/app/editor/publish/PublishPlan.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/editor/publish/PublishVerify.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/imgui/UiInputScript.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/imgui/UiAutomation.cpp"
    # Editor undo/redo command logic under test. The typed component commands
    # (add/remove/set) resolve components through the editor catalog and the
    # hand-authored field sets, so both TUs are compiled in alongside them.
    "${CMAKE_SOURCE_DIR}/src/app/debug/UndoStack.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/debug/EditorCommand.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/editor/ComponentCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/editor/ComponentFields.cpp"
)
target_include_directories(EngineTests PRIVATE "${CMAKE_SOURCE_DIR}/src/app")
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
target_link_libraries(EngineTests PRIVATE Engine AssetPipeline doctest::doctest imgui nlohmann_json::nlohmann_json)
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

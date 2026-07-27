# Engine/tooling unit tests. Test sources are globbed (CONFIGURE_DEPENDS) like the rest of
# the build; the app-side TUs below live in src/app and are listed explicitly since they
# can't be globbed from here.
file(GLOB_RECURSE ENGINE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(EngineTests
    ${ENGINE_TEST_SOURCES}
    # App-side TUs under test: EngineTests links Engine only, so the scene serializer (plus
    # the reflection TUs that populate the component registry at static-init) and the
    # ImguiFrameData type its test exercises are compiled straight in here.
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
    "${CMAKE_SOURCE_DIR}/src/app/scripting/modules/TagSlots.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/imgui/ImguiFrameData.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/editor/AsepriteSpriteImporter.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/project/ProjectPaths.cpp"
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

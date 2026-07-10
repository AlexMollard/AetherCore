add_executable(EngineTests
    TestMain.cpp
    scene/HierarchyTests.cpp
    scene/WorldTests.cpp
    scene/SceneSerializerTests.cpp
    scene/SceneWorkflowTests.cpp
    scene/TransformEditTests.cpp
    scene/TransformUtilsTests.cpp
    utils/RayTests.cpp
    utils/FuzzyMatchTests.cpp
    utils/LogRingBufferTests.cpp
    utils/LayoutPresetStoreTests.cpp
    utils/EngineSettingsTests.cpp
    utils/SettingsServiceTests.cpp
    utils/TomlConfigTests.cpp
    material/MaterialDescribeTests.cpp
    material/PackMaterialTests.cpp
    material/MaterialRegistryTests.cpp
    material/MaterialSystemTests.cpp
    material/DeferredSlotFreeListTests.cpp
    material/MaterialInstanceTests.cpp
    material/MaterialAuthoringTests.cpp
    material/PipelineCacheTests.cpp
    material/EffectParamBufferTests.cpp
    material/TextureRegistryTests.cpp
    ui/UiLayoutTests.cpp
    ui/UiEntitiesTests.cpp
    ui/UiDrawBuilderTests.cpp
    ui/FontShapingTests.cpp
    imgui/ImguiFrameDataTests.cpp
    # App-side translation units under test: EngineTests links Engine only, so
    # the scene serializer and the app pieces it needs compile in directly.
    "${CMAKE_SOURCE_DIR}/src/app/scene/SceneSerializer.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/SceneWorkflow.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scripting/modules/TagSlots.cpp"
)
target_include_directories(EngineTests PRIVATE "${CMAKE_SOURCE_DIR}/src/app")
# Mirror the App target's scenes dir so ReadSceneFile("default") resolves the
# committed template from within the headless test binary (SceneSerializer.cpp
# reads it via std::ifstream on this absolute path).
target_compile_definitions(EngineTests PRIVATE AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes")
target_link_libraries(EngineTests PRIVATE Engine doctest::doctest)
aethercore_target_defaults(EngineTests)
set_target_properties(EngineTests PROPERTIES FOLDER "Tests")
add_test(NAME EngineTests COMMAND EngineTests)

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
    "${CMAKE_SOURCE_DIR}/src/app/scene/SceneWorkflow.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/reflection/Reflection.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/reflection/CoreComponents.reflect.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/reflection/MoreComponents.reflect.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scene/reflection/Physics.reflect.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scripting/modules/TagSlots.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/imgui/ImguiFrameData.cpp"
)
target_include_directories(EngineTests PRIVATE "${CMAKE_SOURCE_DIR}/src/app")
target_include_directories(EngineTests PRIVATE "${xxHash_SOURCE_DIR}")   # pak index-hash tests
# Mirror App's scenes dir so ReadSceneFile("default") resolves the committed template.
target_compile_definitions(EngineTests PRIVATE AETHER_SCENES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/scenes")
target_compile_definitions(EngineTests PRIVATE AETHER_TESTS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/tests")
target_link_libraries(EngineTests PRIVATE Engine AssetPipeline doctest::doctest imgui)
aethercore_target_defaults(EngineTests)
set_target_properties(EngineTests PROPERTIES FOLDER "Tests")
add_test(NAME EngineTests COMMAND EngineTests)

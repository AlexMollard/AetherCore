add_executable(EngineTests
    TestMain.cpp
    material/PackMaterialTests.cpp
    material/MaterialRegistryTests.cpp
    material/MaterialSystemTests.cpp
    material/DeferredSlotFreeListTests.cpp
    material/MaterialInstanceTests.cpp
    material/MaterialAuthoringTests.cpp
    material/PipelineCacheTests.cpp
    material/EffectParamBufferTests.cpp
    material/TextureRegistryTests.cpp
)
target_link_libraries(EngineTests PRIVATE Engine doctest::doctest)
aethercore_target_defaults(EngineTests)
set_target_properties(EngineTests PROPERTIES FOLDER "Tests")
add_test(NAME EngineTests COMMAND EngineTests)

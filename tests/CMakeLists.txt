add_executable(EngineTests
    TestMain.cpp
    material/PackMaterialTests.cpp
    material/MaterialRegistryTests.cpp
    material/MaterialSystemTests.cpp
)
target_link_libraries(EngineTests PRIVATE Engine doctest::doctest)
aethercore_target_defaults(EngineTests)
set_target_properties(EngineTests PROPERTIES FOLDER "Tests")
add_test(NAME EngineTests COMMAND EngineTests)

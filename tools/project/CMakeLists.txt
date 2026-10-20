add_executable(aether-project main.cpp ProjectPipeline.cpp DesktopGameplayRun.cpp
    "${CMAKE_SOURCE_DIR}/src/app/Application.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/layers/ScriptedSceneLayer.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/scripting/CSharpScriptingSubsystem.cpp"
    "${CMAKE_SOURCE_DIR}/src/editor/editor/publish/WebPublish.cpp"
    "${CMAKE_SOURCE_DIR}/src/editor/editor/publish/PublishPlan.cpp"
    "${CMAKE_SOURCE_DIR}/src/editor/editor/publish/PublishSteps.cpp"
    "${CMAKE_SOURCE_DIR}/src/editor/editor/publish/PublishReport.cpp"
    "${CMAKE_SOURCE_DIR}/src/editor/editor/publish/PublishVerify.cpp"
    "${CMAKE_SOURCE_DIR}/src/editor/editor/ShaderCompiler.cpp"
    "${CMAKE_SOURCE_DIR}/src/editor/materialgraph/MaterialGraph.cpp")
target_link_libraries(aether-project PRIVATE Engine RuntimeFramework AssetPipeline)
target_compile_definitions(aether-project PRIVATE
    AETHER_MANAGED_BUILD_PROJECT="${CMAKE_SOURCE_DIR}/CMake/ManagedGameplayDriver"
    AETHER_MANAGED_CONFIG="$<IF:$<CONFIG:Debug>,Debug,Release>"
    AETHER_MANAGED_CONFIGDIR="$<IF:$<CONFIG:Debug>,debug,release>")
if(SLANGC_EXECUTABLE)
    target_compile_definitions(aether-project PRIVATE
        AETHER_SLANGC_EXE="${SLANGC_EXECUTABLE}"
        AETHER_SLANG_ARGS="${AETHERCORE_SLANG_SHADER_ARGS}"
        AETHER_SHADER_INCLUDE_DIR="${AETHERCORE_SHADER_SOURCE_DIR}")
endif()
aethercore_target_defaults(aether-project)
set_target_properties(aether-project PROPERTIES ENABLE_EXPORTS ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/app" FOLDER "Tools")
if(MSVC)
    target_compile_options(aether-project PRIVATE /bigobj)
endif()
add_dependencies(aether-project StageAppBundle GameRuntime AssetPacker)
add_custom_command(TARGET aether-project POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/CMake/ManagedGameplayDriver"
        "$<TARGET_FILE_DIR:aether-project>/data/sdk/managed-build")
if(TARGET Editor)
    add_dependencies(Editor aether-project)
endif()

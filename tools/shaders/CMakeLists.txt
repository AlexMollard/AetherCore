add_executable(WebMaterialCompiler
    web-material-compiler.cpp
    "${CMAKE_SOURCE_DIR}/src/editor/materialgraph/MaterialGraph.cpp")
target_link_libraries(WebMaterialCompiler PRIVATE Engine)
target_include_directories(WebMaterialCompiler PRIVATE "${CMAKE_SOURCE_DIR}/src/editor")
aethercore_target_defaults(WebMaterialCompiler)
set_target_properties(WebMaterialCompiler PROPERTIES FOLDER "Tools")
if(AETHER_HAS_DOTNET AND WIN32)
    add_custom_command(TARGET WebMaterialCompiler POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${AETHER_NETHOST_DLL}" "$<TARGET_FILE_DIR:WebMaterialCompiler>")
endif()

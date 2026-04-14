add_executable(App)

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE APP_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

target_sources(App
    PRIVATE
        ${APP_SOURCES}
    PRIVATE FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES ${APP_HEADERS}
)

# Engine propagates Vulkan, GLFW, GLM, and cxx_std_20 automatically.
target_link_libraries(App PRIVATE Engine)

aethercore_target_defaults(App)
aethercore_enable_slang_shader_compilation(App)

# Copy textures from resources/ into the build output so the VFS "assets://" mount can find them.
add_custom_command(TARGET App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/textures"
        "${CMAKE_BINARY_DIR}/assets/textures"
    COMMENT "Copying textures → build/assets/textures"
)

# Copy fonts from resources/fonts/ into the build output.
# Place .ttf files in resources/fonts/ before building.
add_custom_command(TARGET App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_SOURCE_DIR}/resources/fonts"
        "${CMAKE_BINARY_DIR}/assets/fonts"
    COMMENT "Copying fonts → build/assets/fonts"
)

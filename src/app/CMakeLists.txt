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

if(AETHERCORE_ENABLE_UNITY_BUILD)
    set_target_properties(App PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE 16
    )
endif()

aethercore_target_defaults(App)
aethercore_enable_slang_shader_compilation(App)

# ── Asset pipeline ─────────────────────────────────────────────────────────
# AssetPacker must be built before App's post-build step can run it.
add_dependencies(App AssetPacker)

# Pack every file under resources/ into a single binary file that the runtime
# PakBackend mounts as "assets://".  The pak is regenerated on every build so
# newly added or changed source assets are always in sync.
add_custom_command(TARGET App POST_BUILD
    COMMAND "$<TARGET_FILE:AssetPacker>"
        "${CMAKE_SOURCE_DIR}/resources"
        "${CMAKE_BINARY_DIR}/data/assets.pak"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/engine.ini"
        "${CMAKE_BINARY_DIR}/data/config/engine.ini"
    COMMENT "Packing assets → data/assets.pak"
)

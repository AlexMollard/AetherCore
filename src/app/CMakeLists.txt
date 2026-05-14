add_executable(App)

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE APP_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

target_sources(App
    PRIVATE
        ${APP_SOURCES}
        ${APP_HEADERS}
)

target_include_directories(App PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(App PRIVATE Engine)

aethercore_target_defaults(App)
aethercore_enable_slang_shader_compilation(App)

# ── Asset pipeline ─────────────────────────────────────────────────────────
# Pack every file under resources/ into a single binary file that the runtime
# PakBackend mounts as "assets://".  The pak is regenerated on every build so
# newly added or changed source assets are always in sync.
# Note: $<TARGET_FILE:AssetPacker> creates an implicit dependency on AssetPacker,
# so an explicit add_dependencies() is not needed.
add_custom_command(TARGET App POST_BUILD
    COMMAND "$<TARGET_FILE:AssetPacker>"
        "--import-materials"
        "${CMAKE_SOURCE_DIR}/resources"
        "${CMAKE_BINARY_DIR}/data/assets.pak"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/data/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/engine.toml"
        "${CMAKE_BINARY_DIR}/data/config/engine.toml"
    COMMENT "Packing assets to data/assets.pak"
    VERBATIM
)

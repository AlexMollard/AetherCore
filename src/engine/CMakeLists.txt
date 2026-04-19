add_library(Engine STATIC)

# CONFIGURE_DEPENDS re-runs glob at build time when the directory changes,
# so adding a new .cpp/.hpp no longer requires manually editing this file.
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
file(GLOB_RECURSE IO_HEADERS     CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/io/*.hpp")

# ENGINE_HEADERS covers #include "io/FileSystem.hpp" (rooted at src/engine/).
# IO_HEADERS covers #include "FileSystem.hpp"        (rooted at src/engine/io/).
target_sources(Engine
    PRIVATE ${ENGINE_SOURCES}
    PUBLIC FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES ${ENGINE_HEADERS}
    PUBLIC FILE_SET io_headers TYPE HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/io
        FILES ${IO_HEADERS}
)

# Propagate C++20 to any target that links Engine.
target_compile_features(Engine PUBLIC cxx_std_20)

target_link_libraries(Engine
    PUBLIC
        volk::volk_headers
        glfw
        glm::glm
        EnTT::EnTT
        vk-bootstrap
        # dbghelp is Windows-only; generator expression avoids the if(WIN32) block.
        $<$<PLATFORM_ID:Windows>:dbghelp>
        # libdl provides dladdr for symbol resolution in crash handler on Linux
        $<$<PLATFORM_ID:Linux>:dl>
        Tracy::TracyClient
    PRIVATE
        freetype
)

target_compile_definitions(Engine PUBLIC
    VK_NO_PROTOTYPES
    $<$<PLATFORM_ID:Windows>:VK_USE_PLATFORM_WIN32_KHR>
    GLM_FORCE_DEPTH_ZERO_TO_ONE   # Vulkan depth range is [0, 1], not [-1, 1]
    GLM_ENABLE_EXPERIMENTAL     # For glm::value_ptr() and other utilities in <glm/gtc/type_ptr.hpp>
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    $<$<BOOL:${AETHERCORE_ENABLE_TRACY}>:TRACY_ENABLE>
)

# VMA is header-only from the consumer's perspective.
target_include_directories(Engine PUBLIC "${vma_SOURCE_DIR}/include")

# stb is header-only: add its source directory as a private include path.
# Texture.cpp defines STB_IMAGE_IMPLEMENTATION once to pull in the implementation.
target_include_directories(Engine PRIVATE
    "${stb_SOURCE_DIR}"
    "${cgltf_SOURCE_DIR}"
)

if(TARGET tomlplusplus::tomlplusplus)
    target_link_libraries(Engine PUBLIC tomlplusplus::tomlplusplus)
elseif(TARGET tomlplusplus_tomlplusplus)
    target_link_libraries(Engine PUBLIC tomlplusplus_tomlplusplus)
else()
    target_include_directories(Engine PUBLIC "${tomlplusplus_SOURCE_DIR}/include")
endif()

if(AETHERCORE_ENABLE_UNITY_BUILD)
    set_target_properties(Engine PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE 16
    )

    # Keep one-definition implementation units out of unity amalgamation.
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Texture.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/VmaImplementation.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/GltfAsset.cpp"
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
    )
endif()

aethercore_target_defaults(Engine)

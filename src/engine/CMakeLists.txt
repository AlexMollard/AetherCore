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
        Vulkan::Vulkan
        glfw
        glm::glm
        vk-bootstrap
        # dbghelp is Windows-only; generator expression avoids the if(WIN32) block.
        $<$<PLATFORM_ID:Windows>:dbghelp>
    PRIVATE
        freetype
)

target_compile_definitions(Engine PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE   # Vulkan depth range is [0, 1], not [-1, 1]
)

# VMA is header-only from the consumer's perspective.
target_include_directories(Engine PUBLIC "${vma_SOURCE_DIR}/include")

# stb is header-only: add its source directory as a private include path.
# Texture.cpp defines STB_IMAGE_IMPLEMENTATION once to pull in the implementation.
target_include_directories(Engine PRIVATE
    "${stb_SOURCE_DIR}"
    "${cgltf_SOURCE_DIR}"
)

aethercore_target_defaults(Engine)

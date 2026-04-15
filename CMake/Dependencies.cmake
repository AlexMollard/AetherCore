include(FetchContent)

# After the initial clone, do not re-check remotes on every configure.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# Silence CMake's own progress spam for dependency sub-builds.
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw
    GIT_TAG        a74efa0d5628b74adc0426af4c5710e287fa7c2c # 3.4
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
    FIND_PACKAGE_ARGS NAMES glfw3
)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm
    GIT_TAG        8d1fd52e5ab5590e2c81768ace50c72bae28f2ed # 1.0.3
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
    FIND_PACKAGE_ARGS
)

FetchContent_Declare(vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap
    GIT_TAG        29777173ac64752fe9ce3d2ff2821d1dd9c7f79b # v1.4.341
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(VMA
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        1d8f600fd424278486eade7ed3e877c99f0846b1 # v3.3.0
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG        VER-2-14-3
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

find_package(Vulkan REQUIRED)

# Suppress GLFW's own build warnings — we don't own that code.
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

# Disable FreeType extras we don't need.
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw glm vk-bootstrap VMA stb freetype cgltf entt)

# ---------------------------------------------------------------------------
# Solution folder organisation (Visual Studio only — ignored by other generators)
# Everything that isn't App or Engine goes into "Dependencies/".
# CMake's own ZERO_CHECK and ALL_BUILD go into "CMake/".
# ---------------------------------------------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

foreach(_dep IN ITEMS
        glfw update_mappings          # GLFW + its gamepad mappings helper
        glm vk-bootstrap
        VulkanMemoryAllocator
        freetype
    cgltf
    EnTT
)
    if(TARGET ${_dep})
        set_target_properties(${_dep} PROPERTIES FOLDER "Dependencies")
    endif()
endforeach()

set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")


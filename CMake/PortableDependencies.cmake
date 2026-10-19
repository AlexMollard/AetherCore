# Shared native/browser versions. No desktop discovery or workers here.
CPMAddPackage(
    NAME glm
    GIT_REPOSITORY https://github.com/g-truc/glm
    GIT_TAG        8d1fd52e5ab5590e2c81768ace50c72bae28f2ed # 1.0.3
    GIT_SHALLOW    FALSE
)

if(AETHERCORE_WEB_RUNTIME)
    CPMAddPackage(
        NAME tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE
    )
    CPMAddPackage(
        NAME xxHash
        GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
        GIT_TAG v0.8.2
        GIT_SHALLOW TRUE
        DOWNLOAD_ONLY YES
    )
    CPMAddPackage(
        NAME zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
        OPTIONS "ZSTD_BUILD_PROGRAMS OFF" "ZSTD_BUILD_TESTS OFF"
            "ZSTD_BUILD_CONTRIB OFF" "ZSTD_BUILD_SHARED OFF" "ZSTD_MULTITHREAD_SUPPORT OFF"
    )
    if(NOT TARGET zstd::libzstd_static)
        add_library(zstd::libzstd_static ALIAS libzstd_static)
    endif()
endif()

CPMAddPackage(
    NAME EnTT
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
)

CPMAddPackage(
    NAME box2d
    GIT_REPOSITORY https://github.com/erincatto/box2d.git
    GIT_TAG        v3.1.1
    GIT_SHALLOW    TRUE
    OPTIONS
        "BOX2D_SAMPLES OFF"
        "BOX2D_BENCHMARKS OFF"
        "BOX2D_DOCS OFF"
        "BOX2D_UNIT_TESTS OFF"
        "BOX2D_AVX2 OFF"          # match Jolt: no AVX2 so one binary runs everywhere
)

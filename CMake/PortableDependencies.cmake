# Shared native/browser versions. No desktop discovery or workers here.
# ── Audio (mixing + decoding) ─────────────────────────────────────────────────
# miniaudio: single-header, public domain, Windows/Linux (WASAPI/ALSA + null),
# built-in decoders for WAV/MP3/FLAC. Chosen over OpenAL/SDL_mixer
# because it needs no SDK or system library - CPM fetches one header and the
# engine compiles its implementation into exactly one TU (AudioSubsystem.cpp).
CPMAddPackage(
    NAME miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.22
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  YES
)
include(MiniaudioFailureCleanup)

# Shared runtime streaming metadata and editor/control JSON use one pinned header library.
CPMAddPackage(
    NAME nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    OPTIONS "JSON_BuildTests OFF"
)
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
    # Same Jolt revision and determinism policy as native (Dependencies.cmake). The browser
    # is single threaded: no pthread flags are added by Jolt under Emscripten, WASM SIMD stays
    # OFF so the scalar deterministic path runs, and nothing here enables fast-math.
    CPMAddPackage(
        NAME JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG        v5.5.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  Build
        OPTIONS
            "CROSS_PLATFORM_DETERMINISTIC ON"
            "OVERRIDE_CXX_FLAGS OFF"
            "ENABLE_ALL_WARNINGS OFF"
            "INTERPROCEDURAL_OPTIMIZATION OFF"
            "ENABLE_INSTALL OFF"
            "GENERATE_DEBUG_SYMBOLS OFF"
            "USE_WASM_SIMD OFF"
            "JPH_USE_WASM64 OFF"
            "DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF"
            "PROFILER_IN_DEBUG_AND_RELEASE OFF"
    )
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

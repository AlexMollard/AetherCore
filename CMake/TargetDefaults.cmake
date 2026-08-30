option(AETHERCORE_ENABLE_ASAN "Enable AddressSanitizer on all first-party targets" OFF)
# Fast (non-IEEE) floating point. OFF by default so engine-side math stays reproducible -
# fast-math would undermine the cross-platform determinism Jolt is built for (lockstep /
# rollback networking). Turn ON when you want the speed and don't need determinism.
option(AETHERCORE_ENABLE_FAST_MATH "Use fast, non-IEEE floating point (/fp:fast, -ffast-math)" OFF)

# Apply project-wide compiler/linker flags to a first-party target. Call after the
# target's sources are declared.
function(aethercore_target_defaults target)
    # /MP tells one cl.exe invocation to compile its inputs across several processes, which
    # only helps a generator that hands cl many files at once - the Visual Studio one. Ninja
    # invokes cl per file and schedules the parallelism itself, so there /MP buys nothing and
    # actively costs: sccache refuses to cache a command it reads as having multiple inputs,
    # which is 502 of 820 compiles going uncached on CI.
    set(_aethercore_mp "")
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        set(_aethercore_mp /MP)
    endif()

    # Check Clang first: clang-cl on Windows is COMPILER_ID=Clang AND MSVC=TRUE and wants
    # Clang-style flags, not MSVC ones.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32)
        # ── Clang-cl on Windows ──────────────────────────────────────────────
        target_compile_options(${target} PRIVATE
            -W4
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
            -Werror=switch-enum
            -Werror=return-type
            -Wno-unused-command-line-argument
            -Wno-missing-designated-field-initializers
            -Wno-missing-field-initializers
            -fms-compatibility-version=19.40
            ${_aethercore_mp}
            /FS
            $<$<BOOL:${AETHERCORE_ENABLE_FAST_MATH}>:/fp:fast>
            /Gy
            /external:anglebrackets
            /external:W0
        )

        # /OPT:REF,ICF dead-strips unreferenced code + folds identical COMDATs (for
        # link-map audits); still debuggable alongside /DEBUG.
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF,ICF>
            $<$<CONFIG:RelWithDebInfo>:/OPT:REF,ICF>
            # CMake enables incremental linking for RelWithDebInfo by default,
            # but /OPT:ICF disables it. Make the intended non-incremental link
            # explicit so every first-party target links without LNK4075.
            $<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>
            $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
            $<$<CONFIG:Debug>:/DEBUG:FULL>
        )

        target_compile_options(${target} PRIVATE /guard:cf)   # Control Flow Guard
        target_link_options(${target} PRIVATE /guard:cf /CETCOMPAT)

        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )

        if(AETHERCORE_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    elseif(MSVC)
        # ── Pure MSVC ────────────────────────────────────────────────────────
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:preprocessor
            /Zc:__cplusplus
            /Zc:inline
            /Zc:templateScope
            /we4062
            /we4063
            /we4715
            ${_aethercore_mp}
            /FS
            $<$<BOOL:${AETHERCORE_ENABLE_FAST_MATH}>:/fp:fast>
            /Gy
            # Keeps switch jump tables out of the instruction cache. sccache does not know
            # this option (MSVC 17.7+) and so reads it as a second input file, which made
            # every first-party translation unit uncacheable - 437 of 754 compiles on CI.
            # It buys cache locality in a shipped binary and nothing in a test build, so it
            # is scoped to the config that ships rather than dropped.
            $<$<CONFIG:Release>:/jumptablerdata>
            /external:anglebrackets
            /external:W0
        )

        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF,ICF>
            $<$<CONFIG:RelWithDebInfo>:/OPT:REF,ICF>
            # CMake enables incremental linking for RelWithDebInfo by default,
            # but /OPT:ICF disables it. Make the intended non-incremental link
            # explicit so every first-party target links without LNK4075.
            $<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>
            $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
        )

        if(AETHERCORE_FAST_MSVC_DEBUG_INFO)
            if (MSVC_VERSION LESS 1940)
                target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/Z7>)
                target_link_options(${target} PRIVATE $<$<CONFIG:Debug>:/DEBUG:FASTLINK>)
            else()
                target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/Z7>)
                target_link_options(${target} PRIVATE $<$<CONFIG:Debug>:/DEBUG:FULL>)
            endif()
        endif()

        target_compile_options(${target} PRIVATE /guard:cf)
        target_link_options(${target} PRIVATE /guard:cf /CETCOMPAT)

        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )

        if(AETHERCORE_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        endif()
    else()
        # ── GCC/Clang on Linux/macOS ─────────────────────────────────────────
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror=switch-enum
            -Werror=return-type
            $<$<BOOL:${AETHERCORE_ENABLE_FAST_MATH}>:-ffast-math>
            -ffunction-sections
            -fdata-sections
            $<$<CONFIG:Debug>:-g3>
        )

        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:-Wl,--gc-sections>
            $<$<CONFIG:Release>:-Wl,--icf=all>
            $<$<CONFIG:RelWithDebInfo>:-Wl,--gc-sections,-Wl,--icf=all>
        )

        if(AETHERCORE_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

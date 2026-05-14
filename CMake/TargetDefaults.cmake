option(AETHERCORE_ENABLE_ASAN "Enable AddressSanitizer on all first-party targets" OFF)

# aethercore_target_defaults(<target>)
#
# Applies project-wide compiler flags to a first-party target.
# Call this on every Engine / App target after its sources are declared.
function(aethercore_target_defaults target)
    # clang-cl on Windows: CMAKE_CXX_COMPILER_ID=Clang AND MSVC=TRUE.
    # Check Clang first so clang-cl gets Clang-style flags, not MSVC ones.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32)
        # ── Clang-cl on Windows ──────────────────────────────────────────────
        target_compile_options(${target} PRIVATE
            # --- Warning level & conformance ---
            -W4
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
            -Werror=switch-enum
            -Werror=return-type

            # --- Code generation ---
            -fms-compatibility-version=19.40
            /MP
            /FS
            /fp:fast
            /Gy

            # --- External header suppression ---
            /external:anglebrackets
            /external:W0
        )

        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF,ICF>
            $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
            $<$<CONFIG:Debug>:/DEBUG:FULL>
        )

        # Control Flow Guard (Clang-cl supports /guard:cf)
        target_compile_options(${target} PRIVATE /guard:cf)
        target_link_options(${target} PRIVATE /guard:cf /CETCOMPAT)

        # Windows.h guard
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )

        if(AETHERCORE_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    elseif(MSVC)
        # --- Pure MSVC compiler ---
        target_compile_options(${target} PRIVATE
            # --- Warning level & conformance ---
            /W4
            /permissive-
            /Zc:preprocessor
            /Zc:__cplusplus
            /Zc:inline
            /Zc:templateScope

            # --- Promoted warnings -> errors ---
            /we4062
            /we4063
            /we4715

            # --- Code generation ---
            /MP
            /FS
            /fp:fast
            /Gy
            /jumptablerdata

            # --- External header suppression ---
            /external:anglebrackets
            /external:W0
        )

        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF,ICF>
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

        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/GS->)
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
        # --- GCC/Clang on Linux/macOS ---
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror=switch-enum
            -Werror=return-type
            -ffast-math
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

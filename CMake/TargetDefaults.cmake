option(AETHERCORE_ENABLE_ASAN "Enable AddressSanitizer on all first-party targets" OFF)
# Fast (non-IEEE) floating point. OFF by default so engine-side math stays reproducible -
# fast-math would undermine the cross-platform determinism Jolt is built for (lockstep /
# rollback networking). Turn ON when you want the speed and don't need determinism.
option(AETHERCORE_ENABLE_FAST_MATH "Use fast, non-IEEE floating point (/fp:fast, -ffast-math)" OFF)

# A filesystem::path narrow conversion follows the process code page on Windows.
# Windows10 1903+ honors activeCodePage in this fusion manifest. A manifest source
# is merged/embedded by both Visual Studio and Ninja's CMake MSVC link rules;
# changing it relinks the executable without recompiling all C++ translation units.
function(aethercore_enable_utf8_process target)
    if(NOT WIN32)
        return()
    endif()
    get_target_property(_aether_utf8_type ${target} TYPE)
    if(NOT _aether_utf8_type STREQUAL "EXECUTABLE")
        return() # A DLL cannot choose its host process's active code page.
    endif()
    get_target_property(_aether_utf8_applied ${target} AETHERCORE_UTF8_PROCESS)
    if(_aether_utf8_applied)
        return()
    endif()
    set(_aether_utf8_manifest "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WindowsUtf8.manifest")
    target_sources(${target} PRIVATE "${_aether_utf8_manifest}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${_aether_utf8_manifest}")
    set_property(TARGET ${target} PROPERTY AETHERCORE_UTF8_PROCESS TRUE)
endfunction()

# CMake's Windows clang-cl rules invoke lld-link directly, bypassing the clang
# driver's sanitizer-runtime selection. Query this compiler's resource directory
# and use the same DLL import/thunk/SEH roots emitted by clang-cl -### /MD.
if(AETHERCORE_ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
        RESULT_VARIABLE _aether_asan_resource_result OUTPUT_VARIABLE _aether_asan_resource
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE _aether_asan_resource_error)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-target-triple
        RESULT_VARIABLE _aether_asan_triple_result OUTPUT_VARIABLE _aether_asan_triple
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _aether_asan_resource_result EQUAL 0 OR NOT _aether_asan_triple_result EQUAL 0)
        message(FATAL_ERROR "Cannot identify this clang-cl ASAN runtime: ${_aether_asan_resource_error}")
    endif()
    file(TO_CMAKE_PATH "${_aether_asan_resource}" _aether_asan_resource)
    if(_aether_asan_triple MATCHES "^x86_64-")
        set(_aether_asan_arch x86_64)
        set(_aether_asan_seh __asan_seh_interceptor)
    elseif(_aether_asan_triple MATCHES "^i[3-6]86-")
        set(_aether_asan_arch i386)
        set(_aether_asan_seh ___asan_seh_interceptor)
    elseif(_aether_asan_triple MATCHES "^(aarch64|arm64)-")
        set(_aether_asan_arch aarch64)
        set(_aether_asan_seh __asan_seh_interceptor)
    else()
        message(FATAL_ERROR "Unsupported Windows clang-cl ASAN target: ${_aether_asan_triple}")
    endif()
    set(_aether_asan_runtime_dirs "${_aether_asan_resource}/lib/${_aether_asan_triple}" "${_aether_asan_resource}/lib/windows")
    find_file(_aether_asan_import NAMES "clang_rt.asan_dynamic-${_aether_asan_arch}.lib" PATHS ${_aether_asan_runtime_dirs} NO_DEFAULT_PATH NO_CACHE REQUIRED)
    find_file(_aether_asan_thunk NAMES "clang_rt.asan_dynamic_runtime_thunk-${_aether_asan_arch}.lib" PATHS ${_aether_asan_runtime_dirs} NO_DEFAULT_PATH NO_CACHE REQUIRED)
    find_file(_aether_asan_dll NAMES "clang_rt.asan_dynamic-${_aether_asan_arch}.dll" PATHS ${_aether_asan_runtime_dirs} NO_DEFAULT_PATH NO_CACHE REQUIRED)
    set(AETHERCORE_CLANG_ASAN_RESOURCE_DIR "${_aether_asan_resource}" CACHE INTERNAL "ASAN runtime selected from the actual compiler" FORCE)
    set(AETHERCORE_CLANG_ASAN_IMPORT "${_aether_asan_import}" CACHE INTERNAL "ASAN DLL import library" FORCE)
    set(AETHERCORE_CLANG_ASAN_THUNK "${_aether_asan_thunk}" CACHE INTERNAL "ASAN runtime initialization thunk" FORCE)
    set(AETHERCORE_CLANG_ASAN_DLL "${_aether_asan_dll}" CACHE INTERNAL "ASAN runtime DLL staged beside each executable" FORCE)
    set(AETHERCORE_CLANG_ASAN_SEH "${_aether_asan_seh}" CACHE INTERNAL "ASAN SEH initialization root" FORCE)
    message(STATUS "clang-cl ASAN: compiler=${CMAKE_CXX_COMPILER}; target=${_aether_asan_triple}; resource=${_aether_asan_resource}; import=${_aether_asan_import}; thunk=${_aether_asan_thunk}; dll=${_aether_asan_dll}")
endif()

# Sanitizer coverage is separate from the warning policy: AssetPipeline also
# compiles vendor codec TUs, but must share STL instrumentation with AssetIdentity
# and its consumers. Keep all sanitizer flag selection in this one function.
function(aethercore_enable_sanitizers target)
    if(NOT AETHERCORE_ENABLE_ASAN)
        return()
    endif()
    get_target_property(_aether_sanitizer_target_type ${target} TYPE)
    if(_aether_sanitizer_target_type STREQUAL "INTERFACE_LIBRARY")
        return() # Header-only code is instrumented by each consuming target.
    endif()
    if(MSVC)
        # LLVM22/lld folds ASAN-padded narrow "." and wide L"." COMDATs to one
        # address under ICF, then diagnoses their different logical sizes as ODR.
        # Keep global identities distinct while retaining ASAN's ODR checks.
        target_link_options(${target} PRIVATE /OPT:NOICF)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_libraries(${target} PRIVATE "${AETHERCORE_CLANG_ASAN_IMPORT}")
        target_link_options(${target} PRIVATE "/WHOLEARCHIVE:${AETHERCORE_CLANG_ASAN_THUNK}" "/INCLUDE:${AETHERCORE_CLANG_ASAN_SEH}" /INCREMENTAL:NO)
        if(_aether_sanitizer_target_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${AETHERCORE_CLANG_ASAN_DLL}" "$<TARGET_FILE_DIR:${target}>"
                COMMENT "Staging the compiler-matched ASAN runtime for ${target}" VERBATIM)
        endif()
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

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

    endif()
    aethercore_enable_utf8_process(${target})
    aethercore_enable_sanitizers(${target})
endfunction()

# These compiled C++ dependencies instantiate/pass MSVC STL containers across
# first-party boundaries (notably imgui_stdlib's std::string). Their annotation
# mode must match ASAN consumers; retain the annotations rather than disabling
# the container checks. C dependencies expose C ABI and do not have this issue.
if(AETHERCORE_ENABLE_ASAN AND MSVC)
    foreach(_aether_stl_dependency IN ITEMS imgui Jolt TracyClient vk-bootstrap glm doctest_with_main)
        if(TARGET ${_aether_stl_dependency})
            aethercore_enable_sanitizers(${_aether_stl_dependency})
        endif()
    endforeach()
endif()

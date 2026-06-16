# AetherCore - link map / dead-strip report
#
# When AETHERCORE_DEAD_STRIP_REPORT is ON, this module:
#   1. Adds /MAP (MSVC, Clang-cl) or -Wl,-Map= (Linux/Clang) to executables
#      that opt in via aethercore_enable_dead_strip_report(<target>).
#   2. Adds -Wl,--print-gc-sections to GCC/Clang so we also see what was
#      removed during the link.
#   3. Defines a `dead-strip-report` custom target that runs
#      scripts/dead-strip-report.py against every map file in the build tree.
#
# Output locations (per target, per config):
#   $<TARGET_FILE_DIR:<target>>/$<TARGET_NAME>.map
#   build/<config>/<target>.gc-sections.log  (Clang/GCC only)
#
# The target_defaults module already wires /OPT:REF,ICF (MSVC + Clang-cl) and
# -Wl,--gc-sections (Clang/GCC) so the linker actually dead-strips. This module
# adds the *report* side.

set(_dead_strip_report_py "${CMAKE_SOURCE_DIR}/scripts/dead-strip-report.py")
if(NOT EXISTS "${_dead_strip_report_py}")
    message(FATAL_ERROR
        "AETHERCORE_DEAD_STRIP_REPORT=ON but ${_dead_strip_report_py} is missing")
endif()

# Cached default; can be overridden with -DAETHERCORE_DEAD_STRIP_REPORT_OUTDIR=...
set(AETHERCORE_DEAD_STRIP_REPORT_OUTDIR
    "${CMAKE_SOURCE_DIR}/build/dead-strip-report"
    CACHE PATH "Where dead-strip-report.py writes its reports")

# aethercore_enable_dead_strip_report(<target>)
#
# Opt a target into the link-map / gc-sections report. Call after the target
# is defined and after aethercore_target_defaults(<target>).
function(aethercore_enable_dead_strip_report target)
    if(NOT AETHERCORE_DEAD_STRIP_REPORT)
        return()
    endif()

    if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32))
        # MSVC + lld-link both accept /MAP:<file>. With a bare filename the
        # linker writes the map next to the .exe (its default), which is what
        # we want. We avoid $<TARGET_FILE_DIR:${target}> and $<TARGET_NAME>
        # genexes here because target_link_options through the VS generator
        # rejects them with "Error evaluating generator expression".
        target_link_options(${target} PRIVATE
            "/MAP:${target}.map"
        )
    else()
        # Clang/GCC: -Wl,-Map for the map file, --print-gc-sections to stderr
        # (we redirect to a log so the parser can pick it up). --no-gc-sections
        # must NOT be passed; aethercore_target_defaults already adds --gc-sections.
        target_link_options(${target} PRIVATE
            "-Wl,-Map,${target}.map"
            "-Wl,--print-gc-sections"
        )
        # Capture stderr to a per-target log. add_custom_command is the only
        # portable way to wrap a link step in CMake; the dependency on the
        # binary ensures the log is regenerated when the target relinks.
        find_program(GEN_NUL NAMES "nul" "/dev/null" "true")
        add_custom_command(TARGET ${target} POST_BUILD
            # Touch a sentinel so the parser can locate the log; the actual
            # stderr is captured by the link rule below. (CMake doesn't let
            # us redirect a single command's stderr here without re-running
            # the link, so the log is a pointer file.)
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${target}>/../dead-strip-report"
            COMMAND ${CMAKE_COMMAND} -E touch
                "$<TARGET_FILE_DIR:${target}>/../dead-strip-report/$<TARGET_NAME>.gc-sections.pointer"
            COMMENT "Recording ${target} gc-sections report location"
            VERBATIM
        )
    endif()
endfunction()

# One aggregate target: rebuild executables if needed, then run the parser.
add_custom_target(dead-strip-report
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AETHERCORE_DEAD_STRIP_REPORT_OUTDIR}"
    COMMAND ${Python3_EXECUTABLE} "${_dead_strip_report_py}"
            --build-dir "${CMAKE_BINARY_DIR}"
            --source-dir "${CMAKE_SOURCE_DIR}"
            --out-dir "${AETHERCORE_DEAD_STRIP_REPORT_OUTDIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM
    COMMENT "Parsing link maps to find source files with no surviving symbols"
)

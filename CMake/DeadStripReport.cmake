# AetherCore link-map / dead-strip report (opt-in via AETHERCORE_DEAD_STRIP_REPORT).
# Adds /MAP (MSVC/clang-cl) or -Wl,-Map= + --print-gc-sections (GCC/Clang) to targets
# that opt in via aethercore_enable_dead_strip_report(<target>), and defines a
# `dead-strip-report` target that runs scripts/dead-strip-report.py over the map files.
# The linker actually dead-strips via /OPT:REF,ICF / --gc-sections (see TargetDefaults);
# this module only adds the reporting.

set(_dead_strip_report_py "${CMAKE_SOURCE_DIR}/scripts/dead-strip-report.py")
if(NOT EXISTS "${_dead_strip_report_py}")
    message(FATAL_ERROR
        "AETHERCORE_DEAD_STRIP_REPORT=ON but ${_dead_strip_report_py} is missing")
endif()

set(AETHERCORE_DEAD_STRIP_REPORT_OUTDIR
    "${CMAKE_SOURCE_DIR}/build/dead-strip-report"
    CACHE PATH "Where dead-strip-report.py writes its reports")

# Opt a target into the map / gc-sections report. Call after aethercore_target_defaults().
function(aethercore_enable_dead_strip_report target)
    if(NOT AETHERCORE_DEAD_STRIP_REPORT)
        return()
    endif()

    if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32))
        # /MAP:<file> writes the map next to the .exe. Bare filename only - the VS
        # generator rejects $<TARGET_FILE_DIR>/$<TARGET_NAME> genexes in link options.
        target_link_options(${target} PRIVATE
            "/MAP:${target}.map"
        )
    else()
        # Clang/GCC: -Wl,-Map for the map, --print-gc-sections to stderr. Do NOT add
        # --no-gc-sections; TargetDefaults already enables --gc-sections.
        target_link_options(${target} PRIVATE
            "-Wl,-Map,${target}.map"
            "-Wl,--print-gc-sections"
        )
        # CMake can't redirect a single command's stderr without re-linking, so drop a
        # sentinel pointer file the parser locates the log by.
        find_program(GEN_NUL NAMES "nul" "/dev/null" "true")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${target}>/../dead-strip-report"
            COMMAND ${CMAKE_COMMAND} -E touch
                "$<TARGET_FILE_DIR:${target}>/../dead-strip-report/$<TARGET_NAME>.gc-sections.pointer"
            COMMENT "Recording ${target} gc-sections report location"
            VERBATIM
        )
    endif()
endfunction()

# Aggregate target: (re)build executables as needed, then run the parser.
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

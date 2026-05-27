# ── daScript source files ─────────────────────────────────────────────────────
# Exposes all .das files as a Visual Studio project so they appear in Solution
# Explorer with proper folder structure.  No compilation occurs here - the
# scripts are compiled at runtime by ScriptingSubsystem.

file(GLOB_RECURSE DAS_SCRIPTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/scripts/*.das")

add_custom_target(Scripts SOURCES ${DAS_SCRIPTS})

# Mirror the source tree in Solution Explorer under "Scripts".
source_group(TREE "${CMAKE_SOURCE_DIR}/resources/scripts" FILES ${DAS_SCRIPTS})

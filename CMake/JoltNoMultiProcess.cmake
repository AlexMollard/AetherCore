# Included by CMAKE_PROJECT_JoltPhysics_INCLUDE, i.e. at the end of Jolt's project() call.
#
# Jolt appends /MP and /Zi to CMAKE_CXX_FLAGS in its own directory scope, so no target
# property can reach them - and the append happens well after this file runs. Defer instead:
# the call below executes at the END of Jolt's directory, once the flags are final and before
# they are used to generate the build.
cmake_language(DEFER CALL aethercore_strip_uncacheable_flags)

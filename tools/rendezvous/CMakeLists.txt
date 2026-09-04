# aether-rendezvous: tiny long-running UDP server that pairs NAT-traversal peers by
# room code and forwards their candidate blobs (see tools/rendezvous/README.md).
# Deliberately depends on nothing from the engine - only ENet (from CPM) - so it is
# a small binary an operator can drop on a cheap VPS with no engine build alongside it.
add_executable(aether-rendezvous main.cpp)

target_link_libraries(aether-rendezvous PRIVATE enet)
if(WIN32)
    target_link_libraries(aether-rendezvous PRIVATE ws2_32 winmm)
endif()

set_target_properties(aether-rendezvous PROPERTIES FOLDER "Tools")

# aether-ctl: one-shot CLI client for the editor's JSON-over-ENet control
# endpoint. Deliberately depends on nothing from the engine - only ENet + JSON
# (both from CPM) - so it stays a tiny, fast standalone tool the MCP can shell.
add_executable(aether-ctl main.cpp)

target_link_libraries(aether-ctl PRIVATE enet nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(aether-ctl PRIVATE ws2_32 winmm)
endif()

set_target_properties(aether-ctl PROPERTIES FOLDER "Tools")

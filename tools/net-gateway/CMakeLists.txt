enable_language(C)
set_target_properties(enet PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(aether_net_gateway SHARED gateway.c)
target_link_libraries(aether_net_gateway PRIVATE enet)
if(WIN32)
    target_link_libraries(aether_net_gateway PRIVATE ws2_32 winmm)
endif()
set_target_properties(aether_net_gateway PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)

if(AETHER_DOTNET_EXE)
    set(_gateway_output "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/managed")
    add_custom_target(NetGateway
        COMMAND "${AETHER_DOTNET_EXE}" build "${CMAKE_CURRENT_SOURCE_DIR}/NetGateway.csproj"
            --configuration Release --nologo
            --artifacts-path "${CMAKE_BINARY_DIR}/managed/net-gateway"
            --output "${_gateway_output}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:aether_net_gateway>" "${_gateway_output}/$<TARGET_FILE_NAME:aether_net_gateway>"
        DEPENDS aether_net_gateway
        COMMENT "Building the self-hosted WebSocket/ENet gateway"
        VERBATIM)
endif()

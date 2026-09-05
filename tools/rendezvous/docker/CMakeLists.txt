# Standalone build for ../Dockerfile, and for anyone building aether-rendezvous
# directly on a bare VPS with no AetherCore engine tree checked out beside it.
#
# The tool's own ../CMakeLists.txt is meant to be add_subdirectory()'d from the
# AetherCore root, which configures the WHOLE engine tree first - and that
# unconditionally calls find_package(Vulkan REQUIRED) in CMake/Dependencies.cmake,
# a GPU SDK this UDP-only server never touches. Configuring that just to get this one
# small binary is the opposite of "an operator can drop this on a cheap VPS with no
# engine build alongside it" (see ../CMakeLists.txt's own comment) - this project is
# what actually delivers on it: it fetches the one dependency this tool has (ENet)
# itself and builds only main.cpp and RendezvousServerLogic.hpp, both included
# unmodified from the parent directory.
cmake_minimum_required(VERSION 3.20)
project(aether-rendezvous LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
# Same repository and tag as CMake/Dependencies.cmake's enet package, so a container
# build and an engine build link the identical ENet source.
FetchContent_Declare(enet
	GIT_REPOSITORY https://github.com/lsalzman/enet.git
	GIT_TAG        v1.3.18
	GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(enet)
# enet's own CMakeLists scopes its include dir to its own build only - export it here
# the same way CMake/Dependencies.cmake does, or <enet/enet.h> will not resolve.
target_include_directories(enet PUBLIC "${enet_SOURCE_DIR}/include")

add_executable(aether-rendezvous ../main.cpp)
target_link_libraries(aether-rendezvous PRIVATE enet)
if(WIN32)
	target_link_libraries(aether-rendezvous PRIVATE ws2_32 winmm)
endif()

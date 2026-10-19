cmake_minimum_required(VERSION 3.24)
project(AetherRustGameplay LANGUAGES NONE)
include("${CMAKE_CURRENT_LIST_DIR}/../RustGameplay.cmake")
aether_add_rust_gameplay(AetherRustGameplay
    MANIFEST "${AETHER_GAMEPLAY_MANIFEST}"
    TARGET_DIR "${AETHER_RUST_TARGET_DIR}"
    OUTPUT_DIR "${AETHER_RUST_OUTPUT_DIR}"
    PROFILE "${AETHER_RUST_PROFILE}")

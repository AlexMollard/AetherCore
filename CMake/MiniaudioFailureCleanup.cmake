# miniaudio 0.11.22 frees a failed resource-manager node before inspecting it
# during notification cleanup (ma_resource_manager_data_buffer_node_acquire).
# Keep the pinned download untouched; compile a generated copy with cleanup
# ordered before the free. Revisit this exact-match patch on dependency upgrades.
file(READ "${miniaudio_SOURCE_DIR}/miniaudio.h" _ae_miniaudio_header)
set(_ae_miniaudio_free [=[    /* If we failed to initialize the data buffer we need to free it. */
    if (result != MA_SUCCESS) {
        if (nodeAlreadyExists == MA_FALSE) {
            ma_resource_manager_data_buffer_node_remove(pResourceManager, pDataBufferNode);
            ma_free(pDataBufferNode, &pResourceManager->config.allocationCallbacks);
        }
    }
]=])
set(_ae_miniaudio_notification [=[    /*
    The init notification needs to be uninitialized. This will be used if the node does not already
    exist, and we've specified ASYNC | WAIT_INIT.
    */
    if (nodeAlreadyExists == MA_FALSE && pDataBufferNode->isDataOwnedByResourceManager && (flags & MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_ASYNC) != 0) {
        if ((flags & MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_WAIT_INIT) != 0) {
            ma_resource_manager_inline_notification_uninit(&initNotification);
        }
    }
]=])
set(_ae_miniaudio_old "${_ae_miniaudio_free}\n${_ae_miniaudio_notification}")
string(FIND "${_ae_miniaudio_header}" "${_ae_miniaudio_old}" _ae_miniaudio_match)
if(_ae_miniaudio_match EQUAL -1)
    message(FATAL_ERROR "Pinned miniaudio failure cleanup changed; review MiniaudioFailureCleanup.cmake before upgrading.")
endif()
string(REPLACE "${_ae_miniaudio_old}" "${_ae_miniaudio_notification}\n${_ae_miniaudio_free}" _ae_miniaudio_patched "${_ae_miniaudio_header}")
set(AETHER_MINIAUDIO_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/miniaudio")
file(MAKE_DIRECTORY "${AETHER_MINIAUDIO_INCLUDE_DIR}")
# Preserve the timestamp when the generated contents are unchanged.
set(_ae_miniaudio_generated "${AETHER_MINIAUDIO_INCLUDE_DIR}/miniaudio.h")
if(EXISTS "${_ae_miniaudio_generated}")
    file(READ "${_ae_miniaudio_generated}" _ae_miniaudio_existing)
endif()
if(NOT _ae_miniaudio_existing STREQUAL _ae_miniaudio_patched)
    file(WRITE "${_ae_miniaudio_generated}" "${_ae_miniaudio_patched}")
endif()

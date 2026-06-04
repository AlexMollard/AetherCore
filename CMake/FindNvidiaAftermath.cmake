# NVIDIA Aftermath SDK - GPU crash dump collection and fault diagnostics.
# If Nsight Graphics is installed the SDK ships alongside it.
# Override NVIDIA_AFTERMATH_ROOT to point to a standalone SDK location.

if(NOT NVIDIA_AFTERMATH_ROOT)
    # Auto-detect via Nsight Graphics installations.
    file(GLOB _nsight_dirs "$ENV{ProgramFiles}/NVIDIA Corporation/Nsight Graphics *")
    foreach(_nsight IN LISTS _nsight_dirs)
        file(GLOB _am_dirs "${_nsight}/SDKs/NsightAftermathSDK/*")
        foreach(_am IN LISTS _am_dirs)
            if(EXISTS "${_am}/include/GFSDK_Aftermath.h")
                set(NVIDIA_AFTERMATH_ROOT "${_am}")
                break()
            endif()
        endforeach()
        if(NVIDIA_AFTERMATH_ROOT)
            break()
        endif()
    endforeach()
endif()

if(NVIDIA_AFTERMATH_ROOT)
    set(NVIDIA_AFTERMATH_INCLUDE_DIR "${NVIDIA_AFTERMATH_ROOT}/include")

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_am_lib_subdir "lib/x64")
    else()
        set(_am_lib_subdir "lib/x86")
    endif()

    set(_am_lib "${NVIDIA_AFTERMATH_ROOT}/${_am_lib_subdir}/GFSDK_Aftermath_Lib.x64.lib")
    set(_am_dll "${NVIDIA_AFTERMATH_ROOT}/${_am_lib_subdir}/GFSDK_Aftermath_Lib.x64.dll")
    if(EXISTS "${_am_lib}")
        set(NVIDIA_AFTERMATH_LIBRARY "${_am_lib}")
        set(NVIDIA_AFTERMATH_DLL "${_am_dll}")
    endif()
endif()

# FPHSA has inconsistent behavior with QUIET finds in some CMake versions.
# Manually set the _FOUND variable based on required vars.
if(NVIDIA_AFTERMATH_INCLUDE_DIR AND NVIDIA_AFTERMATH_LIBRARY)
    set(NVIDIA_AFTERMATH_FOUND TRUE)
else()
    set(NVIDIA_AFTERMATH_FOUND FALSE)
endif()

if(NVIDIA_AFTERMATH_FOUND AND NOT TARGET Nvidia::Aftermath)
    add_library(Nvidia::Aftermath SHARED IMPORTED)
    set_target_properties(Nvidia::Aftermath PROPERTIES
        IMPORTED_LOCATION "${NVIDIA_AFTERMATH_DLL}"
        IMPORTED_IMPLIB "${NVIDIA_AFTERMATH_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NVIDIA_AFTERMATH_INCLUDE_DIR}"
    )
endif()

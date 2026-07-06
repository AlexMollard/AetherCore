# ── .NET (CoreCLR) hosting detection ──────────────────────────────────────────
#
# AetherCore hosts the .NET runtime in-process to run C# gameplay scripts. This
# module locates the `dotnet` SDK (to build the managed assemblies) and the
# `nethost` static library + hostfxr headers (to bootstrap CoreCLR from C++).
#
# Everything downstream is gated on AETHER_HAS_DOTNET. When the SDK or nethost
# pack is missing (e.g. a bare CI runner), the flag stays OFF and the engine
# compiles a stubbed DotNetHost that reports scripting as unavailable - the build
# never fails just because .NET is absent.
#
# Result variables:
#   AETHER_HAS_DOTNET   BOOL   TRUE when dotnet + nethost + headers were found
#   AETHER_DOTNET_EXE   PATH   the `dotnet` driver (used by the managed build step)
#   DotNet::NetHost     TARGET imported static lib carrying the include dir + defines

set(AETHER_HAS_DOTNET FALSE)

# ── 1. The `dotnet` driver (needed to build the C# projects) ──────────────────
# Honor an explicit DOTNET_ROOT before falling back to PATH.
if(DEFINED ENV{DOTNET_ROOT})
    find_program(AETHER_DOTNET_EXE
        NAMES dotnet
        HINTS "$ENV{DOTNET_ROOT}"
        NO_DEFAULT_PATH)
endif()
find_program(AETHER_DOTNET_EXE NAMES dotnet)

# ── 2. Locate the newest nethost pack (static lib + hostfxr headers) ──────────
# The host pack ships nethost.lib/.a and the hostfxr/coreclr headers under a
# versioned directory. We pick the highest version available.
# On Windows we link nethost.dll via its import lib (nethost.lib) and ship the
# DLL - the static libnethost.lib is built with the static CRT (/MT) and clashes
# with AetherCore's dynamic CRT (/MD). On Unix the static archive links cleanly.
if(WIN32)
    set(_aether_host_rid "win-x64")
    set(_aether_host_implib "nethost.lib") # import lib for nethost.dll
    set(_aether_host_dll "nethost.dll")
elseif(APPLE)
    set(_aether_host_rid "osx-x64")
    set(_aether_host_libname "libnethost.a")
else()
    set(_aether_host_rid "linux-x64")
    set(_aether_host_libname "libnethost.a")
endif()

set(_aether_host_search_roots "")
if(DEFINED ENV{DOTNET_ROOT})
    list(APPEND _aether_host_search_roots "$ENV{DOTNET_ROOT}")
endif()
if(WIN32)
    list(APPEND _aether_host_search_roots
        "$ENV{ProgramFiles}/dotnet"
        "$ENV{ProgramW6432}/dotnet")
else()
    list(APPEND _aether_host_search_roots
        "/usr/share/dotnet"
        "/usr/lib/dotnet"
        "/usr/local/share/dotnet")
endif()

set(_aether_nethost_native_dir "")
foreach(_root IN LISTS _aether_host_search_roots)
    file(GLOB _packs
        "${_root}/packs/Microsoft.NETCore.App.Host.${_aether_host_rid}/*/runtimes/${_aether_host_rid}/native")
    if(_packs)
        # Highest version wins (natural sort so 10.0.9 > 9.0.17).
        list(SORT _packs COMPARE NATURAL ORDER DESCENDING)
        list(GET _packs 0 _aether_nethost_native_dir)
        break()
    endif()
endforeach()

# ── 3. Validate and build the imported target ─────────────────────────────────
set(_aether_nethost_ok FALSE)
if(WIN32)
    if(_aether_nethost_native_dir
            AND EXISTS "${_aether_nethost_native_dir}/${_aether_host_implib}"
            AND EXISTS "${_aether_nethost_native_dir}/${_aether_host_dll}")
        set(_aether_nethost_ok TRUE)
    endif()
elseif(_aether_nethost_native_dir AND EXISTS "${_aether_nethost_native_dir}/${_aether_host_libname}")
    set(_aether_nethost_ok TRUE)
endif()

if(AETHER_DOTNET_EXE AND _aether_nethost_ok
        AND EXISTS "${_aether_nethost_native_dir}/nethost.h"
        AND EXISTS "${_aether_nethost_native_dir}/hostfxr.h"
        AND EXISTS "${_aether_nethost_native_dir}/coreclr_delegates.h")

    if(WIN32)
        # Shared: link the import lib, ship nethost.dll (AETHER_NETHOST_DLL).
        add_library(DotNet::NetHost SHARED IMPORTED GLOBAL)
        set_target_properties(DotNet::NetHost PROPERTIES
            IMPORTED_IMPLIB "${_aether_nethost_native_dir}/${_aether_host_implib}"
            IMPORTED_LOCATION "${_aether_nethost_native_dir}/${_aether_host_dll}"
            INTERFACE_INCLUDE_DIRECTORIES "${_aether_nethost_native_dir}")
        set(AETHER_NETHOST_DLL "${_aether_nethost_native_dir}/${_aether_host_dll}"
            CACHE INTERNAL "Path to nethost.dll to deploy next to the app")
    else()
        # Static archive links cleanly on Unix; call the in-process resolver.
        add_library(DotNet::NetHost STATIC IMPORTED GLOBAL)
        set_target_properties(DotNet::NetHost PROPERTIES
            IMPORTED_LOCATION "${_aether_nethost_native_dir}/${_aether_host_libname}"
            INTERFACE_INCLUDE_DIRECTORIES "${_aether_nethost_native_dir}"
            INTERFACE_COMPILE_DEFINITIONS "NETHOST_USE_AS_STATIC")
    endif()

    set(AETHER_HAS_DOTNET TRUE)
    message(STATUS "AetherCore: .NET SDK found (${AETHER_DOTNET_EXE})")
    message(STATUS "AetherCore: nethost pack -> ${_aether_nethost_native_dir}")
else()
    message(STATUS "AetherCore: .NET SDK/nethost not found - C# scripting disabled")
endif()

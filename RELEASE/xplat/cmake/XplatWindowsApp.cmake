# xplat_windows_app(<target> [GUI] [WMAIN] [MANIFEST <file> [RC <file>]])
# [rexglue-native-platform-fix] 2026-11-23. A Windows program's application manifest, embedded the right way for the
# toolchain that builds it; nothing anywhere else.
#
# The manifest (xplat/win/xplat.manifest, the one every game shares) asks Windows for:
#   activeCodePage UTF-8  (Windows 10 1903 and later): the program's narrow strings are UTF-8, so the narrow file
#                         functions, path::string() and the libraries that use them (the SDK's log and settings,
#                         spdlog, toml++) take any letter instead of only the PC's ANSI code page;
#   longPathAware         paths over 260 characters, where Windows has long paths on (LongPathsEnabled).
# Nothing else (no DPI or compatibility settings: SDL sets DPI awareness itself).
#
# How it is embedded:
#   MinGW (llvm-mingw, the Windows build made on Linux): a resource script, xplat/win/xplat_manifest.rc
#     ("1 24 xplat.manifest"), compiled by windres. MinGW's linker makes no manifest of its own.
#   The MSVC ABI (clang or clang-cl for x86_64-pc-windows-msvc with lld-link, or MSVC itself): the manifest file
#     as a source. CMake hands it to the linker with /MANIFESTINPUT:, and the linker merges it into the manifest it
#     makes anyway (CMake links with /MANIFEST:EMBED). A resource script there would be a second RT_MANIFEST #1
#     next to the linker's, which link.exe refuses (CVT1100) and lld-link may resolve either way.
# GUI   the program has a window and no console: WIN32_EXECUTABLE (WinMain / wWinMain).
# WMAIN its entry point is wmain or wWinMain (the ReXGlue SDK's is wWinMain): MinGW's C runtime calls those only
#       for a program linked with -municode. (The MSVC ABI picks the entry point by itself.)
# MANIFEST another manifest file in place of the shared one. RC: MinGW's resource script for it, when the game has its
#       own ("1 24 <manifest>"); without RC one is made in the build folder. (CoD3 adopts with its own app.manifest and
#       app.rc, byte for byte the shared ones, so its MinGW build stays identical to the last object name; moving
#       to the shared files later changes only that name.)
#
# A macro, not a function: enable_language(RC) must run in the directory's own scope.
include_guard(GLOBAL)

get_filename_component(_xplat_win "${CMAKE_CURRENT_LIST_DIR}/../win" ABSOLUTE)
set(XPLAT_WINDOWS_MANIFEST "${_xplat_win}/xplat.manifest" CACHE INTERNAL "")
set(XPLAT_WINDOWS_MANIFEST_RC "${_xplat_win}/xplat_manifest.rc" CACHE INTERNAL "")
unset(_xplat_win)

# TRUE when this build makes Windows programs with the MSVC ABI (its linker makes a manifest of its own).
function(xplat_windows_msvc_abi out)
    if(WIN32 AND NOT MINGW AND NOT CYGWIN)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

macro(xplat_windows_app _xplat_target)
    cmake_parse_arguments(_XPLAT_APP "GUI;WMAIN" "MANIFEST;RC" "" ${ARGN})
    if(WIN32)
        if(_XPLAT_APP_GUI)
            set_target_properties(${_xplat_target} PROPERTIES WIN32_EXECUTABLE ON)
        endif()
        if(_XPLAT_APP_WMAIN AND MINGW)
            target_link_options(${_xplat_target} PRIVATE -municode)
        endif()
        xplat_windows_msvc_abi(_xplat_msvc_abi)
        if(_xplat_msvc_abi)
            if(_XPLAT_APP_MANIFEST)
                target_sources(${_xplat_target} PRIVATE "${_XPLAT_APP_MANIFEST}")
            else()
                target_sources(${_xplat_target} PRIVATE "${XPLAT_WINDOWS_MANIFEST}")
            endif()
        else()
            enable_language(RC)  # MinGW's windres (the toolchain file's CMAKE_RC_COMPILER)
            if(_XPLAT_APP_RC)
                target_sources(${_xplat_target} PRIVATE "${_XPLAT_APP_RC}")
            elseif(_XPLAT_APP_MANIFEST)
                # a resource script of the given manifest, made in the build folder
                get_filename_component(_xplat_manifest_abs "${_XPLAT_APP_MANIFEST}" ABSOLUTE)
                set(_xplat_rc "${CMAKE_CURRENT_BINARY_DIR}/${_xplat_target}_manifest.rc")
                file(WRITE "${_xplat_rc}" "1 24 \"${_xplat_manifest_abs}\"\n")
                target_sources(${_xplat_target} PRIVATE "${_xplat_rc}")
            else()
                target_sources(${_xplat_target} PRIVATE "${XPLAT_WINDOWS_MANIFEST_RC}")
            endif()
        endif()
        unset(_xplat_msvc_abi)
    endif()
    unset(_XPLAT_APP_GUI)
    unset(_XPLAT_APP_WMAIN)
    unset(_XPLAT_APP_MANIFEST)
    unset(_XPLAT_APP_RC)
endmacro()

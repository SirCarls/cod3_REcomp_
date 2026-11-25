# Everything xplat gives a game's CMake: XPLAT_DIR, xplat_windows_app (XplatWindowsApp.cmake), xplat_find_ngl_vk,
# xplat_find_glslc and xplat_rex_renderdoc_include (XplatNglVk.cmake). [rexglue-native-platform-fix] 2026-11-23
# A game finds this file through XPLAT_DIR (the setup tool passes it), else next to its own folder:
#     if(NOT XPLAT_DIR)
#         get_filename_component(XPLAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../xplat" ABSOLUTE)
#     endif()
#     include("${XPLAT_DIR}/cmake/Xplat.cmake")
include_guard(GLOBAL)
get_filename_component(_xplat_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(XPLAT_DIR "${_xplat_dir}" CACHE PATH "The project's xplat/ folder (its platform layer)")
unset(_xplat_dir)
include("${CMAKE_CURRENT_LIST_DIR}/XplatWindowsApp.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/XplatNglVk.cmake")

# xplat::xplat for a game that links the library (C API, or xplat.hpp's helpers that call it)
macro(xplat_add_library)
    if(NOT TARGET xplat)
        add_subdirectory("${XPLAT_DIR}" "${CMAKE_BINARY_DIR}/xplat")
    endif()
endmacro()

# [acct2-bro] Call of Duty: Finest Hour port: the build. [waw-decomp] 2026-11-09, phase 2: Swap shows the render target
# in a window (src/fh_present.c, through ../ngl_vk and SDL3).
#
#   cmake -S fh -B fh/out -DXBOXRECOMP_DIR=<xboxrecomp checkout> -DFH_GEN_DIR=<recompiler output> \
#         [-DFH_SDL3_INCLUDE_DIR=<folder holding SDL3/SDL.h> -DFH_SDL3_LIBRARY=<libSDL3.a>]
#         [-DFH_FFMPEG_SOURCE_DIR=<FFmpeg 4.4 source>   (default: the SDK's, work/rexglue-sdk/thirdparty/FFmpeg)]
#         [-DFH_IMGUI_DIR=<Dear ImGui>   (the F2 window; default: the SDK's, work/rexglue-sdk/thirdparty/imgui)]
#   cmake --build fh/out -j
#
# XBOXRECOMP_DIR: a checkout of xboxrecomp (its kernel replacement, platform, USB and input libraries are used; its
#   GPU, audio-chip, Direct3D and movie libraries are not), the same one Big Red One's port builds with.
# FH_GEN_DIR: the folder the recompiler wrote the game's C into (its --gen-dir), default fh/generated. The recompiler
#   is run with fh/config/stand_in_functions.tsv's addresses as --manual-functions (fh/README.md). The generated C is
#   the game's code, so it stays on each user's machine (git-ignored).
# Shared with Big Red One's port (../bro): src/runtime/no_gpu_emulation.c (what xboxrecomp's kernel wants from the GPU
# and audio-chip parts left out) and src/x86_privileged.h (registers the lifted code can name).
cmake_minimum_required(VERSION 3.20)
project(fh C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XBOXRECOMP_DIR "" CACHE PATH "xboxrecomp checkout")
set(FH_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "The recompiler's --gen-dir output")
# How hard the compiler works on the generated C, as compiler flags with spaces between them. The generated code reads
# and writes the same memory as different types, so it is never compiled with strict aliasing.
set(FH_GEN_OPTIMIZE "-O1" CACHE STRING "Compiler flags for the generated C, e.g. -O1 or \"-O2 -fno-strict-aliasing\"")
separate_arguments(FH_GEN_OPTIMIZE_LIST UNIX_COMMAND "${FH_GEN_OPTIMIZE}")
get_filename_component(FH_REPO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(FH_BRO_DIR "${FH_REPO_DIR}/bro")

if(NOT EXISTS "${XBOXRECOMP_DIR}/src/kernel/kernel.h")
    message(FATAL_ERROR "Set XBOXRECOMP_DIR to an xboxrecomp checkout (src/kernel/kernel.h not found in '${XBOXRECOMP_DIR}')")
endif()
if(NOT EXISTS "${FH_GEN_DIR}")
    message(FATAL_ERROR "FH_GEN_DIR '${FH_GEN_DIR}' does not exist: run the recompiler first (fh/README.md)")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
set(XR "${XBOXRECOMP_DIR}/src")

# xboxrecomp's host libraries, as it builds them
add_subdirectory("${XR}/platform" "${CMAKE_BINARY_DIR}/xboxrecomp/platform")
add_subdirectory("${XR}/input" "${CMAKE_BINARY_DIR}/xboxrecomp/input")
add_subdirectory("${XR}/usb" "${CMAKE_BINARY_DIR}/xboxrecomp/usb")

# xboxrecomp's kernel replacement, without its GPU command-stream player (the same list as Big Red One's port):
# nothing here emulates the GPU.
set(FH_KERNEL_SOURCES
    kernel_path.c kernel_pool.c kernel_rtl.c kernel_memory.c kernel_file.c kernel_missing.c kernel_thread.c
    kernel_sync.c kernel_hal.c kernel_guest_cpu.c kernel_xbox.c kernel_ob.c kernel_io.c kernel_crypto.c
    kernel_thunks.c kernel_bridge.c xbox_memory_layout.c guest_vmem.c icall_feedback.c recomp_trace.c
    nv2a_flip_hold.c)
list(TRANSFORM FH_KERNEL_SOURCES PREPEND "${XR}/kernel/")
add_library(xbox_kernel STATIC ${FH_KERNEL_SOURCES} "${FH_BRO_DIR}/src/runtime/no_gpu_emulation.c")
target_include_directories(xbox_kernel PUBLIC "${XR}/kernel" "${XR}" "${XBOXRECOMP_DIR}/templates/runtime")
if(WIN32)
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt)
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)
endif()
target_link_libraries(xbox_kernel PRIVATE xbox_usb)
if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /O2 /GS- /Gy)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -O2 -Wno-unused-parameter)
endif()

# The stand-ins, generated from the committed table
set(FH_STAND_INS_C "${CMAKE_BINARY_DIR}/fh_stand_ins.c")
add_custom_command(
    OUTPUT "${FH_STAND_INS_C}" "${CMAKE_BINARY_DIR}/fh_stand_ins_ids.h"
    COMMAND Python3::Interpreter "${FH_REPO_DIR}/tools/fh_decomp/fh_stubs.py"
            --from-table "${CMAKE_CURRENT_SOURCE_DIR}/config/stand_in_functions.tsv" "${FH_STAND_INS_C}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/config/stand_in_functions.tsv" "${FH_REPO_DIR}/tools/fh_decomp/fh_stubs.py"
            "${FH_REPO_DIR}/tools/bro_decomp/ngl_stubs.py"
    COMMENT "Generating the Direct3D and DirectSound stand-ins")

# The game's recompiled code
file(GLOB FH_GEN_SOURCES CONFIGURE_DEPENDS "${FH_GEN_DIR}/*.c")

# [waw-decomp/fh3] 2026-11-23: functions the port wraps with its own C while keeping the game's body
# (config/wrapped_functions.tsv, tools/fh_decomp/fh_wrap.py): the files of the recompiled C that define them are
# compiled from copies in which that one definition is renamed fh_lifted_<address>; FH_GEN_DIR itself is not changed.
set(FH_WRAPPED_TSV "${CMAKE_CURRENT_SOURCE_DIR}/config/wrapped_functions.tsv")
# [waw-decomp/fh11] 2026-11-24: and loop back edges that call the port's C (config/back_edge_hooks.tsv, fh_jobs.c)
set(FH_BACK_EDGE_TSV "${CMAKE_CURRENT_SOURCE_DIR}/config/back_edge_hooks.tsv")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${FH_REPO_DIR}/tools/fh_decomp/fh_wrap.py" "${FH_GEN_DIR}" "${FH_WRAPPED_TSV}"
            "${CMAKE_BINARY_DIR}/wrapped" "${FH_BACK_EDGE_TSV}"
    RESULT_VARIABLE FH_WRAP_RESULT OUTPUT_VARIABLE FH_WRAP_OUTPUT ERROR_VARIABLE FH_WRAP_ERROR)
if(NOT FH_WRAP_RESULT EQUAL 0)
    message(FATAL_ERROR "fh_wrap.py failed: ${FH_WRAP_ERROR}")
endif()
string(REPLACE "\r" "" FH_WRAP_OUTPUT "${FH_WRAP_OUTPUT}")
string(REPLACE "\n" "|" FH_WRAP_OUTPUT "${FH_WRAP_OUTPUT}")
string(REGEX MATCH "^([^|]*)\\|([^|]*)" FH_WRAP_MATCH "${FH_WRAP_OUTPUT}")
set(FH_WRAP_ORIGINALS "${CMAKE_MATCH_1}")
set(FH_WRAP_COPIES "${CMAKE_MATCH_2}")
if(NOT FH_WRAP_COPIES)
    message(FATAL_ERROR "fh_wrap.py wrote no copies: ${FH_WRAP_OUTPUT}")
endif()
list(REMOVE_ITEM FH_GEN_SOURCES ${FH_WRAP_ORIGINALS})
list(APPEND FH_GEN_SOURCES ${FH_WRAP_COPIES})
# a new table, tool or lift makes the next build configure again
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${FH_WRAPPED_TSV}" "${FH_BACK_EDGE_TSV}" "${FH_REPO_DIR}/tools/fh_decomp/fh_wrap.py" ${FH_WRAP_ORIGINALS})

add_executable(fh
    src/main.c
    src/recomp_manual.c
    src/fh_log.c
    src/fh_d3d.c
    src/fh_d3d_state.c
    src/fh_present.c
    src/fh_frame_pace.c
    src/fh_frame_clock.c    # [acct2-fh] 2026-11-14 the game's frame timer, a stand-in (FRAMERATE-AUDIT.md)
    src/fh_pad.c
    src/fh_movie.c
    src/fh_sound_timing.c   # [waw-decomp-fh2 r17] 2026-11-24 sound buffers keep their play time (subtitles, scripted scenes)
    src/fh_watch.c
    src/fh_draw.c
    src/fh_vsh.c
    src/fh_push.c
    src/fh_debug.c
    src/fh_profile.c
    src/fh_settings.c       # [acct2-fh] 2026-11-13 fh.toml (the player's settings)
    src/fh_resolution.c     # [acct2-fh] 2026-11-13 what size the game is drawn at (RESOLUTION.md)
    src/fh_aspect.c         # [acct2-fh] 2026-11-13 the picture's shape, Hor+ (RESOLUTION.md stage 3)
    src/fh_console.c        # [acct2-fh] 2026-11-14 the console: the log's last lines and commands (CONSOLE.md)
    src/fh_game_vars.c      # [acct2-fh] 2026-11-14 the game's debug menu's values, by name, for the console
    src/fh_relaunch.c       # [acct2-fh] 2026-11-15 a level change starts the game again (CONSOLE.md, `map`)
    src/fh_host.c           # [waw-decomp/fh2] 2026-11-23 UTF-8 paths and a steady clock, Linux and Windows
    src/fh_worlds.c         # [waw-decomp/fh3] 2026-11-23 freed RenderWare worlds' objects taken out (Road to Remagen)
    src/fh_jobs.c           # [waw-decomp/fh11] 2026-11-24 the job thread sleeps on its empty queue (one core no longer spun)
    src/fh_text.c           # [waw-decomp-fh2 r19] 2026-11-24 the game's XFONT text at the picture's own size
    src/fh_fade.c           # [waw-decomp/fh13] 2026-11-24 the screen fade in whole 60ths (the same length at any frame rate)
    "${FH_STAND_INS_C}"
    ${FH_GEN_SOURCES})
target_include_directories(fh PRIVATE src "${CMAKE_BINARY_DIR}" "${FH_GEN_DIR}" "${XBOXRECOMP_DIR}/include")
if(UNIX AND NOT APPLE)
    # the kernel and the USB/input libraries call each other; GNU ld needs them in one group
    target_link_libraries(fh PRIVATE -Wl,--start-group xbox_kernel xbox_usb xbox_input platform -Wl,--end-group)
else()
    target_link_libraries(fh PRIVATE xbox_kernel xbox_usb xbox_input platform)
endif()
if(UNIX)
    target_link_libraries(fh PRIVATE m)
endif()
if(WIN32)
    target_link_libraries(fh PRIVATE shell32)   # [waw-decomp/fh2] 2026-11-23 CommandLineToArgvW (src/fh_host.c)
endif()
find_package(Threads REQUIRED)
target_link_libraries(fh PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

# [input-port] 2026-11-24: inputkit (the project's inputkit/ folder, the input code every game shares: the key names
# and bind rules and the sticks' dead zone of src/fh_pad.c), next to fh/ unless INPUTKIT_DIR names it. It is C++
# inside with a C interface, so the program links as C++.
enable_language(CXX)
if(NOT INPUTKIT_DIR)
    get_filename_component(INPUTKIT_DIR "${FH_REPO_DIR}/inputkit" ABSOLUTE)
endif()
if(NOT EXISTS "${INPUTKIT_DIR}/cmake/Inputkit.cmake")
    message(FATAL_ERROR "fh: the project's inputkit/ folder wasn't found (INPUTKIT_DIR, or next to fh/). "
        "Configure with -DINPUTKIT_DIR=<project>/inputkit.")
endif()
include("${INPUTKIT_DIR}/cmake/Inputkit.cmake")
inputkit_add_library()
inputkit_link_c(fh)
# [input-port] 11-24: input/gamepad_db.h (the SDL_GameControllerDB loader) is shared with BRO; its folder, not a copy
target_include_directories(fh PRIVATE "${FH_BRO_DIR}/src")

# [acct2-fh] 2026-11-13: the films (src/fh_movie.c) through FFmpeg, built here from the FFmpeg 4.4 source Call of
# Duty 3's SDK carries, by cmake/fh_ffmpeg.cmake (which says what is built and why); nothing of the system's is used
# (owner 11-13 19:49: "remember our portable plan you can't depend on the user having something"). Without that source
# the game goes on without its films.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fh_ffmpeg.cmake")
if(FH_FFMPEG_FOUND)
    target_link_libraries(fh PRIVATE fh_ffmpeg)
    target_compile_definitions(fh PRIVATE FH_HAVE_FFMPEG=1)
    # FFmpeg's headers for src/fh_movie.c alone: their folder also holds FFmpeg's own config.h, which no other source
    # should find
    set_property(SOURCE src/fh_movie.c APPEND PROPERTY INCLUDE_DIRECTORIES "${FH_FFMPEG_INCLUDE_DIR}")
endif()

# [waw-decomp] 2026-11-09. The window through SDL3 (src/fh_present.c): the static SDL3 CoD3's SDK builds, named by
# FH_SDL3_INCLUDE_DIR (the folder holding SDL3/SDL.h) and FH_SDL3_LIBRARY (libSDL3.a), else an SDL3 CMake finds, as
# Big Red One's build does. Without SDL3 there is no window: frames are drawn offscreen.
set(FH_SDL3_INCLUDE_DIR "" CACHE PATH "SDL3's include folder (the one holding SDL3/SDL.h)")
set(FH_SDL3_LIBRARY "" CACHE FILEPATH "SDL3's static library (libSDL3.a)")
if(FH_SDL3_INCLUDE_DIR AND FH_SDL3_LIBRARY)
    add_library(fh_sdl3 STATIC IMPORTED)
    set_target_properties(fh_sdl3 PROPERTIES IMPORTED_LOCATION "${FH_SDL3_LIBRARY}"
                          INTERFACE_INCLUDE_DIRECTORIES "${FH_SDL3_INCLUDE_DIR}")
    if(WIN32)
        # the system libraries SDL3's own static target names on Windows
        set_property(TARGET fh_sdl3 PROPERTY INTERFACE_LINK_LIBRARIES kernel32 user32 gdi32 winmm imm32 ole32 oleaut32
                     version uuid advapi32 setupapi shell32 hid dinput8)
    else()
        set_property(TARGET fh_sdl3 PROPERTY INTERFACE_LINK_LIBRARIES m ${CMAKE_DL_LIBS} Threads::Threads)
    endif()
    target_link_libraries(fh PRIVATE fh_sdl3)
    target_compile_definitions(fh PRIVATE FH_HAVE_SDL3=1)
else()
    find_package(SDL3 CONFIG QUIET)
    if(TARGET SDL3::SDL3)
        target_link_libraries(fh PRIVATE SDL3::SDL3)
        target_compile_definitions(fh PRIVATE FH_HAVE_SDL3=1)
    else()
        message(WARNING "SDL3 not found (FH_SDL3_INCLUDE_DIR, FH_SDL3_LIBRARY): no window, frames are drawn offscreen")
    endif()
endif()
if(NOT WIN32)
    # xboxrecomp's input library links SDL2 for a keyboard path this port doesn't use, so only one SDL ends up in the
    # program (as in Big Red One's build)
    get_target_property(_fh_input_links xbox_input INTERFACE_LINK_LIBRARIES)
    if(_fh_input_links)
        list(REMOVE_ITEM _fh_input_links SDL2::SDL2)
        set_target_properties(xbox_input PROPERTIES INTERFACE_LINK_LIBRARIES "${_fh_input_links}")
    endif()
endif()
# The pictures through NGL on Vulkan (../ngl_vk, shared with Big Red One and Call of Duty 3), when the Vulkan headers
# and glslc are there; without it frames are not drawn
find_package(Vulkan)
find_program(FH_GLSLC NAMES glslc HINTS "${Vulkan_GLSLC_EXECUTABLE}")
if(Vulkan_FOUND AND FH_GLSLC)
    add_subdirectory("${FH_REPO_DIR}/ngl_vk" "${CMAKE_BINARY_DIR}/ngl_vk")
    target_link_libraries(fh PRIVATE ngl_vk)
    target_compile_definitions(fh PRIVATE FH_HAVE_VULKAN=1)
    # [waw-decomp] 2026-11-09: the game's draws (src/fh_draw.c) through ngl_vk's lower layer, which calls Vulkan
    # itself (so it links the loader), with shaders of our own turned into SPIR-V here, as ngl_vk does with its own
    # (nothing compiles shaders while the game runs)
    target_sources(fh PRIVATE "${FH_REPO_DIR}/ngl_vk/src/ngl_vk_low.c")
    set_source_files_properties("${FH_REPO_DIR}/ngl_vk/src/ngl_vk_low.c" PROPERTIES
                                COMPILE_DEFINITIONS NGL_VK_HAVE_BACKEND=1)
    target_link_libraries(fh PRIVATE Vulkan::Vulkan)
    set(FH_SHADER_DIR "${CMAKE_BINARY_DIR}/fh_shaders")
    set(FH_SHADERS)
    # [acct2-fh] 2026-11-13: program.vert runs the game's vertex programs on the GPU (src/fh_draw.c)
    foreach(shader fixed.vert fixed.frag program.vert)
        set(spirv "${FH_SHADER_DIR}/${shader}.inc")
        add_custom_command(
            OUTPUT "${spirv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${FH_SHADER_DIR}"
            COMMAND "${FH_GLSLC}" -mfmt=c -o "${spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
            COMMENT "Compiling ${shader} to SPIR-V")
        list(APPEND FH_SHADERS "${spirv}")
    endforeach()
    target_sources(fh PRIVATE ${FH_SHADERS})
    target_include_directories(fh PRIVATE "${FH_SHADER_DIR}")
else()
    message(WARNING "Vulkan or glslc not found: building without ngl_vk, so frames are not drawn")
endif()
# [acct2-fh] 2026-11-14 The F2 settings window (src/f2/, dev/agents/acct2-fh/RESOLUTION.md stage 4): Call of Duty 3's
# window, shared with the other games' ports (FH_F2_SHARED_DIR: COD_3_WAW_Setup-tool/recomp/src), drawn with Dear
# ImGui (FH_IMGUI_DIR: the rexglue SDK's thirdparty/imgui, in the project's work folder as the films' FFmpeg is) over
# ngl_vk's picture, as Big Red One's (bro/src/f2/). Left out without ngl_vk, SDL3, a C++ compiler, Dear ImGui or the
# shared rows: the game runs as before, without F2.
set(FH_IMGUI_DIR "" CACHE PATH "Dear ImGui for the F2 window; empty: Call of Duty 3's SDK's (work/rexglue-sdk/thirdparty/imgui)")
set(FH_F2_SHARED_DIR "${FH_REPO_DIR}/COD_3_WAW_Setup-tool/recomp/src" CACHE PATH "The shared F2 window's f2/ and ui/")
set(_fh_imgui "${FH_IMGUI_DIR}")
if(NOT _fh_imgui)
    set(_fh_imgui "${FH_REPO_DIR}/work/rexglue-sdk/thirdparty/imgui")
endif()
include(CheckLanguage)
check_language(CXX)
if(TARGET ngl_vk AND (TARGET fh_sdl3 OR TARGET SDL3::SDL3) AND CMAKE_CXX_COMPILER
   AND EXISTS "${_fh_imgui}/imgui.h" AND EXISTS "${_fh_imgui}/backends/imgui_impl_vulkan.cpp"
   AND EXISTS "${FH_F2_SHARED_DIR}/f2/f2_rows.cpp")
    enable_language(CXX)
    add_library(fh_f2 STATIC
        src/f2/fh_f2.cpp src/f2/fh_f2_store.cpp
        "${FH_F2_SHARED_DIR}/f2/f2_rows.cpp" "${FH_F2_SHARED_DIR}/f2/f2_theme.cpp" "${FH_F2_SHARED_DIR}/f2/f2_pad.cpp"
        "${_fh_imgui}/imgui.cpp" "${_fh_imgui}/imgui_draw.cpp" "${_fh_imgui}/imgui_tables.cpp"
        "${_fh_imgui}/imgui_widgets.cpp" "${_fh_imgui}/backends/imgui_impl_sdl3.cpp"
        "${_fh_imgui}/backends/imgui_impl_vulkan.cpp")
    set_target_properties(fh_f2 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    # the shim first, so the shared rows' <rex/cvar.h> is Finest Hour's (src/f2/shim/rex/cvar.h)
    target_include_directories(fh_f2 PRIVATE src/f2/shim src/f2 src "${FH_F2_SHARED_DIR}" "${_fh_imgui}"
                               "${_fh_imgui}/backends" "${FH_REPO_DIR}/ngl_vk/include")
    # Finest Hour links the Vulkan loader (ngl_vk_low.c), so ImGui's Vulkan backend calls it directly
    if(TARGET fh_sdl3)
        target_link_libraries(fh_f2 PRIVATE fh_sdl3 ngl_vk Vulkan::Vulkan)
    else()
        target_link_libraries(fh_f2 PRIVATE SDL3::SDL3 ngl_vk Vulkan::Vulkan)
    endif()
    target_link_libraries(fh PRIVATE fh_f2)
    target_compile_definitions(fh PRIVATE FH_HAVE_F2=1)
    target_compile_definitions(fh_f2 PRIVATE FH_HAVE_F2=1)
    message(STATUS "F2 settings window: on (Dear ImGui from ${_fh_imgui})")
else()
    message(STATUS "F2 settings window: left out (needs ngl_vk, SDL3, a C++ compiler, Dear ImGui (FH_IMGUI_DIR) and "
                   "the shared rows (FH_F2_SHARED_DIR))")
endif()
# Registers the lifted code can name that user code never really uses (Big Red One's src/x86_privileged.h)
set(FH_PRIVILEGED_H "${FH_BRO_DIR}/src/x86_privileged.h")
if(MSVC)
    target_compile_options(fh PRIVATE /bigobj)
    target_link_options(fh PRIVATE /LARGEADDRESSAWARE)
    set_source_files_properties(${FH_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/FI${FH_PRIVILEGED_H}")
else()
    # the generated files are large and the lifted code is not written for warnings
    set_source_files_properties(${FH_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;${FH_GEN_OPTIMIZE_LIST};-include;${FH_PRIVILEGED_H}")
endif()

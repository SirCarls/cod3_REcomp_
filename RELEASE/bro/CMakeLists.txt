# Big Red One port: the build.
#
#   cmake -S bro -B bro/out -DXBOXRECOMP_DIR=<xboxrecomp checkout> -DBRO_GEN_DIR=<recompiler output>
#   cmake --build bro/out -j
#
# XBOXRECOMP_DIR: a checkout of xboxrecomp (its kernel replacement, platform, USB and input libraries are used;
#   its GPU, audio-chip, Direct3D and movie libraries are not).
# BRO_GEN_DIR: the folder the recompiler wrote the game's C into (its --gen-dir), default bro/generated.
#   The generated C is the game's code, so it stays on each user's machine (git-ignored).
cmake_minimum_required(VERSION 3.20)
project(bro C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XBOXRECOMP_DIR "" CACHE PATH "xboxrecomp checkout")
set(BRO_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "The recompiler's --gen-dir output")
# [acct2-bro-game] How hard the compiler works on the generated C (the game's own code), as compiler flags with
# spaces between them. -O1 compiles fastest. "-O2 -fno-strict-aliasing" ran the game's frame in about half the time
# in Gela (2026-11-07: 2.8 -> 1.5 ms) at about twice the build time; the generated code reads and writes the same
# memory as different types (an int's bits as a float), so it is never compiled with strict aliasing.
set(BRO_GEN_OPTIMIZE "-O1" CACHE STRING "Compiler flags for the generated C, e.g. -O1 or \"-O2 -fno-strict-aliasing\"")
separate_arguments(BRO_GEN_OPTIMIZE_LIST UNIX_COMMAND "${BRO_GEN_OPTIMIZE}")
get_filename_component(BRO_REPO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(NOT EXISTS "${XBOXRECOMP_DIR}/src/kernel/kernel.h")
    message(FATAL_ERROR "Set XBOXRECOMP_DIR to an xboxrecomp checkout (src/kernel/kernel.h not found in '${XBOXRECOMP_DIR}')")
endif()
if(NOT EXISTS "${BRO_GEN_DIR}")
    message(FATAL_ERROR "BRO_GEN_DIR '${BRO_GEN_DIR}' does not exist: run the recompiler first (bro/README.md)")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
set(XR "${XBOXRECOMP_DIR}/src")

# xboxrecomp's host libraries, as it builds them
add_subdirectory("${XR}/platform" "${CMAKE_BINARY_DIR}/xboxrecomp/platform")
add_subdirectory("${XR}/input" "${CMAKE_BINARY_DIR}/xboxrecomp/input")
add_subdirectory("${XR}/usb" "${CMAKE_BINARY_DIR}/xboxrecomp/usb")

# xboxrecomp's kernel replacement, without its GPU command-stream player (nv2a_pb_scan.c, nv2a_pb_exec.c,
# nv2a_vsh_interp.c, nv2a_combiner.c): nothing here emulates the GPU. src/runtime/no_gpu_emulation.c stands in
# for the few of their functions the kernel calls.
set(BRO_KERNEL_SOURCES
    kernel_path.c kernel_pool.c kernel_rtl.c kernel_memory.c kernel_file.c kernel_missing.c kernel_thread.c
    kernel_sync.c kernel_hal.c kernel_guest_cpu.c kernel_xbox.c kernel_ob.c kernel_io.c kernel_crypto.c
    kernel_thunks.c kernel_bridge.c xbox_memory_layout.c guest_vmem.c icall_feedback.c recomp_trace.c
    nv2a_flip_hold.c)
list(TRANSFORM BRO_KERNEL_SOURCES PREPEND "${XR}/kernel/")
add_library(xbox_kernel STATIC ${BRO_KERNEL_SOURCES} src/runtime/no_gpu_emulation.c)
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
set(BRO_STAND_INS_C "${CMAKE_BINARY_DIR}/stand_ins.c")
add_custom_command(
    OUTPUT "${BRO_STAND_INS_C}" "${CMAKE_BINARY_DIR}/ngl_stand_ins_ids.h"
    COMMAND Python3::Interpreter "${BRO_REPO_DIR}/tools/bro_decomp/ngl_stubs.py"
            --from-table "${CMAKE_CURRENT_SOURCE_DIR}/config/stand_in_functions.tsv" "${BRO_STAND_INS_C}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/config/stand_in_functions.tsv" "${BRO_REPO_DIR}/tools/bro_decomp/ngl_stubs.py"
    COMMENT "Generating the NGL, Direct3D and DirectSound stand-ins")

# The game's recompiled code
file(GLOB BRO_GEN_SOURCES CONFIGURE_DEPENDS "${BRO_GEN_DIR}/*.c")

add_executable(bro
    src/main.c
    src/recomp_manual.c
    src/ngl/ngl_log.c
    src/ngl/ngl_frame_clock.c
    src/ngl/ngl_movie.c
    src/ngl/ngl_fps_counter.c
    src/settings/bro_settings.c
    src/settings/bro_view.c
    src/settings/bro_game_cvars.c       # [f2-menus] the game's own cvars, for the F2 window
    src/ngl/ngl_scene.c
    src/ngl/ngl_font.c
    src/ngl/ngl_texture.c
    src/ngl/ngl_present.c
    src/ngl/ngl_quad.c
    src/ngl/ngl_texture_load.c
    src/ngl/ngl_mesh.c
    src/ngl/ngl_quad_batch.c
    src/input/bro_pad.c
    src/input/bro_binds.c
    src/audio/bro_media.c
    src/audio/bro_sound.c
    src/script/bro_script_threads.c
    src/debug/bro_debug.c
    "${BRO_STAND_INS_C}"
    ${BRO_GEN_SOURCES})
target_include_directories(bro PRIVATE src/ngl src/input src/audio src/script src/debug src/settings "${CMAKE_BINARY_DIR}" "${BRO_GEN_DIR}" "${XBOXRECOMP_DIR}/include")
if(UNIX AND NOT APPLE)
    # the kernel and the USB/input libraries call each other; GNU ld needs them in one group
    target_link_libraries(bro PRIVATE -Wl,--start-group xbox_kernel xbox_usb xbox_input platform -Wl,--end-group)
else()
    target_link_libraries(bro PRIVATE xbox_kernel xbox_usb xbox_input platform)
endif()
# The window, the sound output and the pads through SDL3 (src/ngl/ngl_present.c, src/audio/bro_sound.c,
# src/input/bro_pad.c): the static SDL3 CoD3's SDK builds (rexglue-sdk/thirdparty/sdl3), named by
# BRO_SDL3_INCLUDE_DIR (the folder holding SDL3/SDL.h) and BRO_SDL3_LIBRARY (libSDL3.a), else an SDL3 CMake finds.
# Without SDL3 there is no window (frames are drawn offscreen), no sound out (the clock moves the sounds on) and no pad
# but BRO_PAD_SCRIPT.
set(BRO_SDL3_INCLUDE_DIR "" CACHE PATH "SDL3's include folder (the one holding SDL3/SDL.h)")
set(BRO_SDL3_LIBRARY "" CACHE FILEPATH "SDL3's static library (libSDL3.a)")
if(BRO_SDL3_INCLUDE_DIR AND BRO_SDL3_LIBRARY)
    add_library(bro_sdl3 STATIC IMPORTED)
    set_target_properties(bro_sdl3 PROPERTIES IMPORTED_LOCATION "${BRO_SDL3_LIBRARY}"
                          INTERFACE_INCLUDE_DIRECTORIES "${BRO_SDL3_INCLUDE_DIR}")
    if(WIN32)
        # the system libraries SDL3's own static target names on Windows
        set_property(TARGET bro_sdl3 PROPERTY INTERFACE_LINK_LIBRARIES kernel32 user32 gdi32 winmm imm32 ole32 oleaut32
                     version uuid advapi32 setupapi shell32 hid dinput8)
    else()
        set_property(TARGET bro_sdl3 PROPERTY INTERFACE_LINK_LIBRARIES m ${CMAKE_DL_LIBS} Threads::Threads)
    endif()
    target_link_libraries(bro PRIVATE bro_sdl3)
    target_compile_definitions(bro PRIVATE BRO_HAVE_SDL3=1)
else()
    find_package(SDL3 CONFIG QUIET)
    if(TARGET SDL3::SDL3)
        target_link_libraries(bro PRIVATE SDL3::SDL3)
        target_compile_definitions(bro PRIVATE BRO_HAVE_SDL3=1)
    else()
        message(WARNING "SDL3 not found (BRO_SDL3_INCLUDE_DIR, BRO_SDL3_LIBRARY): no window, sound output or pads")
    endif()
endif()
if(NOT WIN32)
    # xboxrecomp's input library links SDL2 for a keyboard path this port doesn't use (bro_pad.c reads the keyboard
    # through SDL3), so only one SDL ends up in the program
    get_target_property(_bro_input_links xbox_input INTERFACE_LINK_LIBRARIES)
    list(REMOVE_ITEM _bro_input_links SDL2::SDL2)
    set_target_properties(xbox_input PROPERTIES INTERFACE_LINK_LIBRARIES "${_bro_input_links}")
endif()
if(UNIX)
    target_link_libraries(bro PRIVATE m)
endif()
find_package(Threads REQUIRED)
# dladdr and a timer thread, for src/debug/bro_debug.c's crash and where-is-it reports
target_link_libraries(bro PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
# NGL on Vulkan (../ngl_vk), when the Vulkan headers, loader and glslc are there; without it frames are not drawn
find_package(Vulkan)
find_program(BRO_GLSLC NAMES glslc HINTS "${Vulkan_GLSLC_EXECUTABLE}")
if(Vulkan_FOUND AND BRO_GLSLC)
    add_subdirectory("${BRO_REPO_DIR}/ngl_vk" "${CMAKE_BINARY_DIR}/ngl_vk")
    target_link_libraries(bro PRIVATE ngl_vk)
    target_compile_definitions(bro PRIVATE BRO_HAVE_VULKAN=1)
    # [acct2-ocr] 2026-11-12 (written by acct2-bro): on Windows src/ngl/ngl_present.c makes the window's Vulkan
    # surface itself (vkCreateWin32SurfaceKHR), so the program links the loader there; ngl_vk no longer passes it
    # on (11-09)
    if(WIN32 AND TARGET Vulkan::Vulkan)                    # [acct2-ocr]
        target_link_libraries(bro PRIVATE Vulkan::Vulkan)  # [acct2-ocr]
    endif()                                                # [acct2-ocr]
else()
    message(WARNING "Vulkan or glslc not found: building without ngl_vk, so frames are not drawn")
endif()

# [f2-menus] The F2 settings window (src/f2/, dev/agents/f2-menus/bro/BRO-F2.md): Call of Duty 3's window, shared with
# the other games' ports (BRO_F2_SHARED_DIR: COD_3_WAW_Setup-tool/recomp/src), drawn with Dear ImGui (BRO_IMGUI_DIR:
# the rexglue SDK's thirdparty/imgui) over ngl_vk's picture. Left out without ngl_vk, SDL3, a C++ compiler or those
# two folders.
set(BRO_IMGUI_DIR "" CACHE PATH "Dear ImGui (the rexglue SDK's thirdparty/imgui), for the F2 window")
set(BRO_F2_SHARED_DIR "${BRO_REPO_DIR}/COD_3_WAW_Setup-tool/recomp/src" CACHE PATH "The shared F2 window's f2/ and ui/")
# BRO_IMGUI_DIR not given: Call of Duty 3's SDK, when the SDL3 in use is the one it built (the setup tool passes
# SDL3_DIR = <work>/rexglue-sdk/out/install/<preset>/lib/cmake/SDL3; Dear ImGui is <work>/rexglue-sdk/thirdparty/imgui).
# Linux only until a Windows build of the window has been tried.
if(NOT BRO_IMGUI_DIR AND SDL3_DIR AND NOT WIN32)
    get_filename_component(_bro_f2_sdk "${SDL3_DIR}/../../../../../.." ABSOLUTE)
    if(EXISTS "${_bro_f2_sdk}/thirdparty/imgui/imgui.h")
        set(BRO_IMGUI_DIR "${_bro_f2_sdk}/thirdparty/imgui")
    endif()
endif()
include(CheckLanguage)
check_language(CXX)
if(TARGET ngl_vk AND (TARGET bro_sdl3 OR TARGET SDL3::SDL3) AND CMAKE_CXX_COMPILER
   AND EXISTS "${BRO_IMGUI_DIR}/imgui.h" AND EXISTS "${BRO_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
   AND EXISTS "${BRO_F2_SHARED_DIR}/f2/f2_rows.cpp")
    enable_language(CXX)
    add_library(bro_f2 STATIC
        src/f2/bro_f2.cpp src/f2/bro_f2_store.cpp src/f2/bro_dvars.cpp
        "${BRO_F2_SHARED_DIR}/f2/f2_rows.cpp" "${BRO_F2_SHARED_DIR}/f2/f2_theme.cpp"
        "${BRO_F2_SHARED_DIR}/f2/f2_pad.cpp" "${BRO_F2_SHARED_DIR}/f2/f2_console.cpp"
        "${BRO_F2_SHARED_DIR}/f2/f2_accel.cpp"   # [f2-menus] 2026-11-11 in-game mouse acceleration, randomizer, scales
        "${BRO_IMGUI_DIR}/imgui.cpp" "${BRO_IMGUI_DIR}/imgui_draw.cpp" "${BRO_IMGUI_DIR}/imgui_tables.cpp"
        "${BRO_IMGUI_DIR}/imgui_widgets.cpp" "${BRO_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
        "${BRO_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp")
    set_target_properties(bro_f2 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    # ngl_vk opens the Vulkan loader at run time, so nothing links to it: ImGui's backend looks its functions up too
    target_compile_definitions(bro_f2 PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_NO_PROTOTYPES)
    target_include_directories(bro_f2 PRIVATE src/f2/shim src/f2 src/settings "${BRO_F2_SHARED_DIR}" "${BRO_IMGUI_DIR}"
                               "${BRO_IMGUI_DIR}/backends" "${BRO_REPO_DIR}/ngl_vk/include")
    if(TARGET bro_sdl3)
        target_link_libraries(bro_f2 PRIVATE bro_sdl3 ngl_vk)
    else()
        target_link_libraries(bro_f2 PRIVATE SDL3::SDL3 ngl_vk)
    endif()
    target_compile_definitions(bro_f2 PRIVATE BRO_HAVE_SDL3=1)
    target_link_libraries(bro PRIVATE bro_f2)
    target_include_directories(bro PRIVATE src/f2)
    target_compile_definitions(bro PRIVATE BRO_HAVE_F2=1)
    message(STATUS "F2 settings window: on (Dear ImGui from ${BRO_IMGUI_DIR})")
else()
    message(STATUS "F2 settings window: left out (needs ngl_vk, SDL3, a C++ compiler, BRO_IMGUI_DIR and BRO_F2_SHARED_DIR)")
endif()
# Registers the lifted code can name that user code never really uses (src/x86_privileged.h)
set(BRO_PRIVILEGED_H "${CMAKE_CURRENT_SOURCE_DIR}/src/x86_privileged.h")
if(MSVC)
    target_compile_options(bro PRIVATE /bigobj)
    target_link_options(bro PRIVATE /LARGEADDRESSAWARE)
    set_source_files_properties(${BRO_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/FI${BRO_PRIVILEGED_H}")
else()
    # the generated files are large and the lifted code is not written for warnings
    set_source_files_properties(${BRO_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;${BRO_GEN_OPTIMIZE_LIST};-include;${BRO_PRIVILEGED_H}")
endif()

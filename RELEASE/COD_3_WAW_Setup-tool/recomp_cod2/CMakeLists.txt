# cod2 - Call of Duty 2 (Xbox 360) recompiled for PC on the ReXGlue SDK, the
# same way and with the same SDK as Call of Duty 3 (../recomp/). [acct2-cod2]
#
# SDK boilerplate lives in generated/rexglue.cmake (written by `rexglue init`;
# never edited by hand). The translated game goes to generated/default/ when
# the cod2_codegen target runs (it is not part of the template).
#
# Graphics: the SDK's Xenos GPU emulation (librexgpu-xenos), Vulkan on Linux,
# Direct3D 12 and Vulkan on Windows (src/cod2_app.cpp picks one).

cmake_minimum_required(VERSION 3.25)
project(cod2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# A cross build (the Windows .exe made on Linux) can't run the SDK's own
# rexglue.exe for the codegen, so it names the build machine's rexglue here,
# as Call of Duty 3's does. The generated C++ is the same either way.
set(REXGLUE_CODEGEN "" CACHE FILEPATH "rexglue that runs the codegen (default: the SDK's)")
if(REXGLUE_CODEGEN)
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION "${REXGLUE_CODEGEN}"
        IMPORTED_LOCATION_RELEASE "${REXGLUE_CODEGEN}")
endif()

# [f2-menus] The F2 settings window shared by every game's port is built from Call of Duty 3's folder
# (../recomp/src/f2/), not copied, as Call of Duty 4's and World at War's projects do. A work copy of this folder
# alone (no ../recomp next to it) builds without the window unless it names the folder:
# -DCOD3_RECOMP_SRC=<project>/COD_3_WAW_Setup-tool/recomp/src
set(COD3_RECOMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/src" CACHE PATH "Call of Duty 3's recomp/src (f2/)")
set(COD2_F2_SOURCES "")
if(EXISTS "${COD3_RECOMP_SRC}/f2/f2_window.cpp" AND EXISTS "${COD3_RECOMP_SRC}/ui/slider_box.h")
    set(COD2_F2_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cod2_f2.cpp"  # Call of Duty 2's pages and colors
        "${COD3_RECOMP_SRC}/f2/f2_common.cpp"          # the shared window (these six)
        "${COD3_RECOMP_SRC}/f2/f2_accel.cpp"           # [f2-menus] 2026-11-11 mouse acceleration, randomizer, scales
        "${COD3_RECOMP_SRC}/f2/f2_console.cpp"
        "${COD3_RECOMP_SRC}/f2/f2_pad.cpp"
        "${COD3_RECOMP_SRC}/f2/f2_rows.cpp"
        "${COD3_RECOMP_SRC}/f2/f2_theme.cpp"
        "${COD3_RECOMP_SRC}/f2/f2_window.cpp")
    message(STATUS "cod2: F2 settings window: on (${COD3_RECOMP_SRC}/f2)")
else()
    message(STATUS "cod2: F2 settings window: left out (no f2/ under COD3_RECOMP_SRC=${COD3_RECOMP_SRC})")
endif()

set(COD2_SOURCES
    src/main.cpp
    src/cod2_app.cpp
    src/game/audio_fpu.cpp  # the audio callback's floating-point state (SDK host thread)
    src/game/diagnostics.cpp  # the game's console lines, errors, menus and zones in the log
    src/game/debug_shots.cpp  # pictures of the game for test runs (cod2_debug_shot_seconds)
    src/game/test_start.cpp  # test runs: press START, then a console command (cod2_debug_*)
    src/game/native_census.cpp  # tests: what the back end draws a frame (cod2_debug_native_census)
    src/game/native_capture.cpp  # native step N1: each draw's submission recorded (cod2_native_capture)
    src/game/native_side.cpp  # [waw-decomp] native step 3: the 2D drawn natively beside (cod2_debug_native_side)
    src/game/native_compare.cpp  # [waw-decomp] 2026-11-11 the live A/B compare (cod2_native_compare)
    src/game/native_window.cpp  # [waw-decomp] 2026-11-11 4z61 the native picture as the game's (cod2_native_vulkan)
    src/game/frame_time.cpp  # tests: where a frame's time goes (cod2_debug_frame_time)
    src/game/frame_rate.cpp  # the guest vblank at 1000 Hz and the frame limit (cod2_max_fps)
    src/game/front_end.cpp  # launch to the main menu: keyboard pad, cod2_skip_intro_movies
    src/game/movement.cpp  # the player's movement in fixed steps (cod2_move_hz)
    src/game/sprint.cpp  # [f2-menus] sprint in the campaign, by United Offensive's rules (cod2_sprint)
    ${COD2_F2_SOURCES}  # [f2-menus] the F2 settings window (empty without the shared window, see above)
    src/cod2_pc_input.cpp  # [f2-menus] the PC copy's keys and the game's own mouse look (cod2_pc_keys, cod2_mouse_look)
    src/game/dvar_peek.cpp  # [f2-menus] tests: the game's own dvars in the log (cod2_debug_dvars)
    src/game/pc_movement.cpp  # [f2-menus] the PC copy's movement numbers in the campaign (cod2_pc_movement)
    src/game/motion_audit.cpp  # tests: how entities and the view move per frame (cod2_debug_entity_steps)
)

if(WIN32)
    add_executable(cod2 WIN32 ${COD2_SOURCES})
else()
    add_executable(cod2 ${COD2_SOURCES})
endif()

target_include_directories(cod2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(COD2_F2_SOURCES)  # [f2-menus]
    target_include_directories(cod2 PRIVATE ${COD3_RECOMP_SRC})
    target_compile_definitions(cod2 PRIVATE COD2_HAVE_F2=1)
endif()

# GPU_PLUGINS stages librexgpu-xenos next to the program; it is loaded at
# start (Cod2App::OnPreSetup).
rexglue_setup_target(cod2 GPU_PLUGINS xenos)

# MinGW: the SDK's entry point is wWinMain, which MinGW's C runtime calls
# only when the program links with -municode.
if(MINGW)
    target_link_options(cod2 PRIVATE -municode)
endif()

# [waw-decomp] 2026-11-10: ngl_vk (the project's ngl_vk/ folder, the Vulkan library Call of Duty 3's and Big Red
# One's native renderers draw with), for the native renderer's step 3: only the cod2_debug_native_side check uses it,
# off by default. It needs the Vulkan headers and glslc, found as Call of Duty 3's build (../recomp/CMakeLists.txt)
# finds them; without them the check only counts. COD2_NGL_VK_DIR names the folder; by default it is the one two up
# from this one (a build in place; a build from a copy elsewhere names it).
set(COD2_NGL_VK_DIR "" CACHE PATH "The project's ngl_vk/ folder (the native renderer's Vulkan library)")
set(_cod2_ngl_vk "${COD2_NGL_VK_DIR}")
if(NOT _cod2_ngl_vk AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../ngl_vk/CMakeLists.txt")
    get_filename_component(_cod2_ngl_vk "${CMAKE_CURRENT_SOURCE_DIR}/../../ngl_vk" ABSOLUTE)
endif()
if(_cod2_ngl_vk AND EXISTS "${_cod2_ngl_vk}/CMakeLists.txt")
    if(CMAKE_CROSSCOMPILING)
        # the Windows build made on Linux: the headers from its CMAKE_PREFIX_PATH (the SDK's install) only
        find_path(NGL_VK_VULKAN_INCLUDE_DIR vulkan/vulkan.h NO_CMAKE_SYSTEM_PATH NO_SYSTEM_ENVIRONMENT_PATH
            NO_CMAKE_ENVIRONMENT_PATH)
    else()
        find_package(Vulkan QUIET)
        set(NGL_VK_VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIR}")
    endif()
    find_program(COD2_NGL_VK_GLSLC NAMES glslc HINTS "${Vulkan_GLSLC_EXECUTABLE}")
    if(NGL_VK_VULKAN_INCLUDE_DIR AND COD2_NGL_VK_GLSLC)
        add_subdirectory("${_cod2_ngl_vk}" "${CMAKE_CURRENT_BINARY_DIR}/ngl_vk")
        target_link_libraries(cod2 PRIVATE ngl_vk)
        target_compile_definitions(cod2 PRIVATE COD2_NGL_VK=1)
        message(STATUS "cod2: ngl_vk from ${_cod2_ngl_vk} (cod2_debug_native_side)")
        # [waw-decomp] 2026-11-11 4z61: native_window.cpp shows the native picture through the SDK's Vulkan presenter
        # (cod2_native_vulkan). Those headers include renderdoc_app.h by its bare name; the SDK keeps it in
        # include/renderdoc (installed) or thirdparty/renderdoc (source tree), neither on the SDK targets' include path
        # (found as Call of Duty 3's ../recomp/CMakeLists.txt finds it).
        get_target_property(_cod2_rex_includes rex::runtime INTERFACE_INCLUDE_DIRECTORIES)
        find_path(COD2_RENDERDOC_INCLUDE_DIR renderdoc_app.h
            HINTS ${_cod2_rex_includes} "${rexglue_DIR}/../../../include" "${REXSDK_DIR}/thirdparty"
            PATH_SUFFIXES renderdoc NO_DEFAULT_PATH)
        if(COD2_RENDERDOC_INCLUDE_DIR)
            target_include_directories(cod2 PRIVATE "${COD2_RENDERDOC_INCLUDE_DIR}")
        else()
            message(WARNING "cod2: the SDK's renderdoc_app.h wasn't found; native_window.cpp won't build")
        endif()
    else()
        message(STATUS "cod2: no ngl_vk (Vulkan headers or glslc missing); cod2_debug_native_side only counts")
    endif()
else()
    message(STATUS "cod2: no ngl_vk (no ngl_vk/ folder found); cod2_debug_native_side only counts")
endif()

# Linux: the SDK links librexruntime.so with an $ORIGIN rpath but doesn't
# put it next to the program, so copy it there.
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET cod2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:cod2>
        VERBATIM)
endif()

# cod2mp, the multiplayer program (default_mp.xex), built next to cod2 from
# mp/ (its own codegen glue), last, as Call of Duty 3's cod3mp is. Left out
# when the disc's multiplayer program isn't in assets/.
option(COD2_BUILD_MP "Build cod2mp, the multiplayer program (default_mp.xex)" ON)
if(COD2_BUILD_MP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/default_mp.xex")
    add_subdirectory(mp)
endif()

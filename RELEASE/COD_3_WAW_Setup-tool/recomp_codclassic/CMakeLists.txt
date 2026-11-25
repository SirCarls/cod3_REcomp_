# codclassic - Call of Duty Classic (Xbox Live Arcade, 2009; Call of Duty 1's engine) recompiled
# for PC on the ReXGlue SDK, the same SDK Call of Duty 3 uses (../recomp/). It is the base for
# United Offensive on the SDK: UO's PC files load on top of it and what UO added is written in C++
# beside it (dev/agents/acct2-uo/PLAN.md). [acct2-uo]
#
# SDK boilerplate lives in generated/rexglue.cmake (written by `rexglue init`; never edited by
# hand). The translated game goes to generated/default/ when the codclassic_codegen target runs
# (it is not part of the template).
#
# Graphics: the SDK's Xenos GPU emulation (librexgpu-xenos), Vulkan on Linux, Direct3D 12 and
# Vulkan on Windows (src/codclassic_app.cpp picks one).

cmake_minimum_required(VERSION 3.25)
project(codclassic LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# A cross build (the Windows .exe made on Linux) can't run the SDK's own rexglue.exe for the
# codegen, so it names the build machine's rexglue here, as Call of Duty 3's build does. The
# generated C++ is the same either way.
set(REXGLUE_CODEGEN "" CACHE FILEPATH "rexglue that runs the codegen (default: the SDK's)")
if(REXGLUE_CODEGEN)
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION "${REXGLUE_CODEGEN}"
        IMPORTED_LOCATION_RELEASE "${REXGLUE_CODEGEN}")
endif()

set(CODCLASSIC_SOURCES
    src/main.cpp
    src/codclassic_app.cpp
    src/game/audio_fpu.cpp  # the audio callback's floating-point state (acct2-cod2's SIGFPE)
    src/game/diagnostics.cpp  # the game's errors (Com_Error) in the port's log
    src/game/film_log.cpp  # a log line per film: its speed against its own frame rate (step 6)
    src/game/frame_rate.cpp  # the frame limit (codclassic_max_fps) before each Com_Frame
    src/game/port_defaults.cpp  # the keyboard pad and the guest vblank rate (SDK settings' defaults)
    src/game/test_start.cpp  # tests: a console command after the first frames (codclassic_debug_start_*)
    src/game/x360shared_imports.cpp  # the x360shared.xdl imports: to the module once loaded (PLAN 2c)
)

if(WIN32)
    add_executable(codclassic WIN32 ${CODCLASSIC_SOURCES})
else()
    add_executable(codclassic ${CODCLASSIC_SOURCES})
endif()

target_include_directories(codclassic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# GPU_PLUGINS stages librexgpu-xenos next to the program; it is loaded at start
# (CodClassicApp::OnPreSetup).
rexglue_setup_target(codclassic GPU_PLUGINS xenos)

# MinGW: the SDK's entry point is wWinMain, which MinGW's C runtime calls only when the program
# links with -municode.
if(MINGW)
    target_link_options(codclassic PRIVATE -municode)
endif()

# Linux: the SDK links librexruntime.so with an $ORIGIN rpath but doesn't put it next to the
# program, so copy it there.
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET codclassic POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:codclassic>
        VERBATIM)
endif()

# The package's code modules: generated/rexglue.cmake makes a shared library for each [[modules]] entry
# (codclassic_x360shared, codclassic_cgame_sp, ...), and the SDK loads one by that bare name when the game
# loads the module. MinGW names a shared library lib<target>.dll, but LoadLibrary adds only ".dll", so on
# Windows the module DLLs get no prefix (as on Linux, where the SDK's patch 0003 tries lib<name>.so).
if(WIN32)
    foreach(module x360shared cgame_sp game_sp ui_sp)
        if(TARGET codclassic_${module})
            set_target_properties(codclassic_${module} PROPERTIES PREFIX "")
        endif()
    endforeach()
endif()

# cgame_sp and ui_sp import from x360shared.xdl too. Their thunks are undefined symbols in each library
# (dlopen refuses it, run acct2-uo-r10), so the forwarders' file goes into each, with the module's list
# (CODCLASSIC_MODULE_<NAME>), compiled without the module's precompiled header of generated code.
set_source_files_properties(src/game/x360shared_imports.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
foreach(module cgame_sp ui_sp)
    if(TARGET codclassic_${module})
        string(TOUPPER ${module} module_upper)
        target_sources(codclassic_${module} PRIVATE src/game/x360shared_imports.cpp)
        target_compile_definitions(codclassic_${module} PRIVATE CODCLASSIC_MODULE_${module_upper}=1)
    endif()
endforeach()

# codclassicmp, the multiplayer program (cod_mp.xex), built next to codclassic from mp/ (its own
# codegen glue), last, as Call of Duty 3's cod3mp is. Left out when the multiplayer program isn't
# in assets/.
option(CODCLASSIC_BUILD_MP "Build codclassicmp, the multiplayer program (cod_mp.xex)" ON)
if(CODCLASSIC_BUILD_MP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/cod_mp.xex")
    add_subdirectory(mp)
endif()

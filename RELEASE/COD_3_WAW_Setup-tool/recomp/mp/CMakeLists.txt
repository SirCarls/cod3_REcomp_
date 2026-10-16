# cod3mp - Call of Duty 3's multiplayer program (codmp_xenonf.xex), the
# second ReXGlue target next to cod3 (docs/MULTIPLAYER.md).
#
# Added by the top-level CMakeLists.txt (add_subdirectory). Its own folder
# because the codegen writes generated/rexglue.cmake next to the manifest:
# this folder's glue builds cod3mp, the top folder's builds cod3. The game
# code is the top folder's src/.

project(cod3mp LANGUAGES C CXX)

include(generated/rexglue.cmake)

set(COD3_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Only what is the same in both programs, or this program's own: the
# campaign's other game code hooks default.xex's addresses. A shared file may
# hook a function by name only when tools/name_mp_functions.py gives this
# program's function of the same role that name (never a sub_ address name:
# here that is another function).
set(COD3MP_SOURCES
    ${COD3_SRC}/main_mp.cpp
    ${COD3_SRC}/cod3mp_app.cpp
    ${COD3_SRC}/game/color_output.cpp
    ${COD3_SRC}/game/level_hooks.cpp
    ${COD3_SRC}/game/mp_hooks.cpp
    ${COD3_SRC}/game/relaunch.cpp
    ${COD3_SRC}/game/script_fiber.cpp
    ${COD3_SRC}/game/script_threads.cpp
    ${COD3_SRC}/test_input.cpp
    ${COD3_SRC}/ui/theme.cpp
)

if(WIN32)
    add_executable(cod3mp WIN32 ${COD3MP_SOURCES})
else()
    add_executable(cod3mp ${COD3MP_SOURCES})
endif()

# Both programs in one folder: each starts the other (game/relaunch.h), and
# the setup tool packs them together.
set_target_properties(cod3mp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# COD3_MP: the few shared files that include a program's generated headers
# pick this program's (game/script_threads.cpp).
target_compile_definitions(cod3mp PRIVATE COD3_MP=1)
target_include_directories(cod3mp PRIVATE ${COD3_SRC})

rexglue_setup_target(cod3mp GPU_PLUGINS xenos)

if(MINGW)
    target_link_options(cod3mp PRIVATE -municode)
endif()

if(WIN32)
    # winmm: timeBeginPeriod; shell32: CommandLineToArgvW (game/relaunch.cpp).
    target_link_libraries(cod3mp PRIVATE winmm shell32)
endif()

# Linux: librexruntime.so next to the executable, as for cod3.
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET cod3mp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:cod3mp>
        VERBATIM)
    if(TARGET rex::amd_fidelityfx_vk)
        add_custom_command(TARGET cod3mp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rex::amd_fidelityfx_vk> $<TARGET_FILE_DIR:cod3mp>
            VERBATIM)
    endif()
endif()

# codclassicmp - Call of Duty Classic's multiplayer program (cod_mp.xex), the second ReXGlue target
# next to codclassic. [acct2-uo]
#
# Added by the top-level CMakeLists.txt (add_subdirectory), last: this folder's generated glue
# defines the SDK macros again for its own target. The program around the game is the campaign's
# (../src/codclassic_app.cpp), built with CODCLASSIC_MP for this program's names.

project(codclassicmp LANGUAGES C CXX)

# The campaign's generated source list and include folder come down from the parent folder's
# scope. Before this program's first codegen its own list doesn't exist yet, and the glue below
# would then claim the campaign's sources as its outputs too (ninja: "multiple rules generate";
# acct2-cod2 met this with cod2mp).
unset(REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
unset(REXGLUE_ENTRYPOINT_INCLUDE_DIR)
unset(GENERATED_SOURCES)

# The multiplayer modules (codclassicmp_x360shared, codclassicmp_cgame_mp, ...) go next to their host
# program, the one the generated glue names in REXGLUE_HOST_TARGET. This folder would inherit the
# parent's value (codclassic), which put them beside the campaign program, where codclassicmp's own
# copy of the runtime ($ORIGIN) doesn't look for them.
set(REXGLUE_HOST_TARGET codclassicmp)

include(generated/rexglue.cmake)

set(CODCLASSIC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

set(CODCLASSICMP_SOURCES
    ${CODCLASSIC_SRC}/main_mp.cpp
    ${CODCLASSIC_SRC}/codclassic_app.cpp
    ${CODCLASSIC_SRC}/game/audio_fpu.cpp
    ${CODCLASSIC_SRC}/game/diagnostics.cpp
    ${CODCLASSIC_SRC}/game/film_log.cpp
    ${CODCLASSIC_SRC}/game/frame_rate.cpp
    ${CODCLASSIC_SRC}/game/port_defaults.cpp
    ${CODCLASSIC_SRC}/game/test_start.cpp
    ${CODCLASSIC_SRC}/game/x360shared_imports.cpp  # the x360shared.xdl imports
)

if(WIN32)
    add_executable(codclassicmp WIN32 ${CODCLASSICMP_SOURCES})
else()
    add_executable(codclassicmp ${CODCLASSICMP_SOURCES})
endif()

target_include_directories(codclassicmp PRIVATE ${CODCLASSIC_SRC})
target_compile_definitions(codclassicmp PRIVATE CODCLASSIC_MP=1)

rexglue_setup_target(codclassicmp GPU_PLUGINS xenos)

if(MINGW)
    target_link_options(codclassicmp PRIVATE -municode)
endif()

# cgame_mp and ui_mp: the x360shared forwarders for their own thunks (see the parent CMakeLists.txt).
set_source_files_properties(${CODCLASSIC_SRC}/game/x360shared_imports.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
foreach(module cgame_mp ui_mp)
    if(TARGET codclassicmp_${module})
        string(TOUPPER ${module} module_upper)
        target_sources(codclassicmp_${module} PRIVATE ${CODCLASSIC_SRC}/game/x360shared_imports.cpp)
        target_compile_definitions(codclassicmp_${module} PRIVATE CODCLASSIC_MP=1 CODCLASSIC_MODULE_${module_upper}=1)
    endif()
endforeach()

# Windows: the module DLLs without MinGW's "lib" prefix (see the parent CMakeLists.txt).
if(WIN32)
    foreach(module x360shared cgame_mp game_mp ui_mp)
        if(TARGET codclassicmp_${module})
            set_target_properties(codclassicmp_${module} PROPERTIES PREFIX "")
        endif()
    endforeach()
endif()

if(UNIX AND NOT APPLE)
    add_custom_command(TARGET codclassicmp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:codclassicmp>
        VERBATIM)
endif()

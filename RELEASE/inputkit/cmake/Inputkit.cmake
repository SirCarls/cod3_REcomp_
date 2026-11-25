# inputkit for a game's CMake: INPUTKIT_DIR and inputkit_add_library(). [input-port] 2026-11-24
# A game finds this file through INPUTKIT_DIR, else next to the project's xplat/ folder (every game already finds
# XPLAT_DIR, and the setup tool passes that), else next to its own folder:
#     if(NOT INPUTKIT_DIR)
#         get_filename_component(INPUTKIT_DIR "${XPLAT_DIR}/../inputkit" ABSOLUTE)
#     endif()
#     include("${INPUTKIT_DIR}/cmake/Inputkit.cmake")
#     inputkit_add_library()
#     target_link_libraries(<game> PRIVATE inputkit::inputkit)
# A game written in C links it the same way; inputkit_link_c(<game>) also makes the link step C++'s (the library
# is C++ inside, with a C interface: include/inputkit/inputkit.h).
include_guard(GLOBAL)
get_filename_component(_inputkit_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(INPUTKIT_DIR "${_inputkit_dir}" CACHE PATH "The project's inputkit/ folder (the input code every game shares)")
unset(_inputkit_dir)

macro(inputkit_add_library)
    if(NOT TARGET inputkit)
        add_subdirectory("${INPUTKIT_DIR}" "${CMAKE_BINARY_DIR}/inputkit")
    endif()
endmacro()

function(inputkit_link_c target)
    target_link_libraries(${target} PRIVATE inputkit::inputkit)
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
    # A Windows build of a C game ships no C++ runtime DLL beside it (Big Red One's has none), so the library's runtime
    # is linked into the program instead (llvm-mingw's clang++ and MinGW's g++ both take these).
    if(MINGW)
        target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endfunction()

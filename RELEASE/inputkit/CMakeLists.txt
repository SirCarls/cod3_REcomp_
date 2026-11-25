# inputkit: the keyboard, mouse and controller code every game in the project shares (include/inputkit/*.h).
# [input-port] 2026-11-24, taken out of Call of Duty 3's game/keybinds and game/mouse_look. C++20, no game and
# no SDK; a C interface for the ports written in C. A game adds it with cmake/Inputkit.cmake (see there).
cmake_minimum_required(VERSION 3.20)
project(inputkit CXX)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/Inputkit.cmake")

if(NOT TARGET inputkit)
    add_library(inputkit STATIC
        src/keys.cpp
        src/binds.cpp
        src/pad_binds.cpp
        src/mouse.cpp
        src/weapon_sens.cpp
        src/stick.cpp
        src/sens_scale.cpp  # [input-port] 2026-11-24 other games' sensitivity scales
        src/activity.cpp  # [input-port] 2026-11-24 keyboard and mouse or controller: which is in use
        src/inputkit_c.cpp)
    add_library(inputkit::inputkit ALIAS inputkit)
    target_include_directories(inputkit PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    # C++20 inside (defaulted operator==); its headers need C++20 in a C++ game too (CoD3 and the rexglue games
    # build with C++20 or newer), and nothing of C++ in a C game (inputkit.h).
    set_target_properties(inputkit PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
    target_compile_features(inputkit INTERFACE cxx_std_20)
endif()

option(INPUTKIT_BUILD_TESTS "Build inputkit's own tests" OFF)
if(INPUTKIT_BUILD_TESTS)
    enable_testing()
    add_executable(inputkit_test tests/test_inputkit.cpp)
    target_link_libraries(inputkit_test PRIVATE inputkit::inputkit)
    add_test(NAME inputkit_test COMMAND inputkit_test)
    # [input-port] 2026-11-24: the mouse's turn at 20 to 1000 fps (the project rule: any frame rate)
    add_executable(inputkit_test_frame_rate tests/test_frame_rate.cpp)
    target_link_libraries(inputkit_test_frame_rate PRIVATE inputkit::inputkit)
    add_test(NAME inputkit_test_frame_rate COMMAND inputkit_test_frame_rate)
    # [input-port] 2026-11-24: other games' sensitivity scales against the calculators' published numbers
    add_executable(inputkit_test_sens_scale tests/test_sens_scale.cpp)
    target_link_libraries(inputkit_test_sens_scale PRIVATE inputkit::inputkit)
    add_test(NAME inputkit_test_sens_scale COMMAND inputkit_test_sens_scale)
    # [input-port] 2026-11-24: which device is in use, with the same hands at 20 to 1000 reads a second
    add_executable(inputkit_test_activity tests/test_activity.cpp)
    target_link_libraries(inputkit_test_activity PRIVATE inputkit::inputkit)
    add_test(NAME inputkit_test_activity COMMAND inputkit_test_activity)
    enable_language(C)
    add_executable(inputkit_test_c tests/test_inputkit_c.c)
    inputkit_link_c(inputkit_test_c)
    set_target_properties(inputkit_test_c PROPERTIES C_STANDARD 11)
    add_test(NAME inputkit_test_c COMMAND inputkit_test_c)
endif()

# xplat: the project's platform layer (include/xplat/xplat.h, xplat.hpp) and its CMake modules (cmake/).
# [rexglue-native-platform-fix] 2026-11-23. A game adds it with
#     add_subdirectory("${XPLAT_DIR}" "${CMAKE_BINARY_DIR}/xplat")   # or include(".../xplat/cmake/Xplat.cmake")
#     target_link_libraries(<game> PRIVATE xplat::xplat)
# The setup tool passes XPLAT_DIR (c3w/native_args.py) through the project folder's link, so a game built from a
# copy of its folder in the work folder finds it wherever that is.
cmake_minimum_required(VERSION 3.20)
project(xplat C CXX)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/Xplat.cmake")

if(NOT TARGET xplat)
    add_library(xplat STATIC
        src/xplat_common.c
        src/xplat_posix.c
        src/xplat_win32.c)
    add_library(xplat::xplat ALIAS xplat)
    target_include_directories(xplat PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    set_target_properties(xplat PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
    if(WIN32)
        # shell32: CommandLineToArgvW, SHGetKnownFolderPath; ole32: CoTaskMemFree; winmm: timeBeginPeriod
        target_link_libraries(xplat PUBLIC shell32 ole32 winmm)
    endif()
    # xplat.hpp needs C++17 (std::filesystem); the library itself is C
    target_compile_features(xplat INTERFACE cxx_std_17)
endif()

option(XPLAT_BUILD_TESTS "Build xplat's own tests" OFF)
if(XPLAT_BUILD_TESTS)
    enable_testing()
    add_executable(xplat_test tests/test_xplat.c)
    target_link_libraries(xplat_test PRIVATE xplat::xplat)
    target_include_directories(xplat_test PRIVATE src)  # the internal helpers it tests
    set_target_properties(xplat_test PROPERTIES C_STANDARD 11)
    add_executable(xplat_test_cpp tests/test_xplat_cpp.cpp)
    target_link_libraries(xplat_test_cpp PRIVATE xplat::xplat)
    set_target_properties(xplat_test_cpp PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    add_executable(xplat_test_cpp17 tests/test_xplat_cpp.cpp)  # xplat.hpp in C++17 too
    target_link_libraries(xplat_test_cpp17 PRIVATE xplat::xplat)
    set_target_properties(xplat_test_cpp17 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    # console programs; each gets the shared manifest, as a game does
    foreach(t xplat_test xplat_test_cpp xplat_test_cpp17)
        xplat_windows_app(${t})
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
    # an argument outside ASCII must reach the program as UTF-8 (on Windows through xplat_utf8_args)
    add_test(NAME xplat_test_utf8_arg COMMAND xplat_test --expect-arg "Łódź-日本-ü-🎮")
    if(WIN32)
        # the glslc launcher (tools/glslc_launcher.c, cmake/XplatNglVk.cmake's glslc on a Windows build PC): it runs
        # the command in its .args file, here xplat_test --expect-arg, and the name outside ASCII must arrive whole
        add_executable(xplat_test_launcher tools/glslc_launcher.c)
        target_link_libraries(xplat_test_launcher PRIVATE shell32)  # no manifest: as try_compile builds it
        file(GENERATE OUTPUT "$<TARGET_FILE_DIR:xplat_test_launcher>/xplat_test_launcher.args"
             CONTENT "$<TARGET_FILE:xplat_test>\n--expect-arg\n")
        add_test(NAME xplat_test_launcher COMMAND xplat_test_launcher "Łódź-日本-ü-🎮")
    endif()
endif()

# The C++ version of the Call of Duty 3 decomp (engine/cod3_cpp/README.md): the cod3
# library, cod3_check_cpp (the checker's answerer) and the tests. Windows and Linux.
#
#   cmake -S engine/cod3_cpp -B <build folder> -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build <build folder>
#   ctest --test-dir <build folder>
#
# [acct2-decomp] 2026-10-24
cmake_minimum_required(VERSION 3.20)
project(cod3_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The console rounds every float operation on its own; a compiler that fuses a
# multiply and an add into one instruction gives other last bits. So no
# contraction anywhere (GCC and Clang would otherwise fuse when the target CPU
# has FMA; MSVC's /fp:precise never does). The console's integers wrap on
# overflow (the hashes count on it, as the original source did), so signed
# overflow wraps here too (-fwrapv).
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /fp:precise)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion -ffp-contract=off -fwrapv)
endif()

add_library(cod3 STATIC
  cod3/common.cpp
  cod3/cvar.cpp
  cod3/files.cpp
  cod3/name_hash.cpp
  cod3/ngl_quad.cpp
  cod3/q_math.cpp
  cod3/q_math_mp.cpp
  cod3/q_shared.cpp
  cod3/scr_math.cpp
  cod3/tr_cmds.cpp
  cod3/xbox_crt.cpp
)
target_include_directories(cod3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The answerer's protocol and one dispatch file per area, as a library so the
# golden test can ask it without a second process.
add_library(cod3_check STATIC
  cod3_check/protocol.cpp
  cod3_check/gfx.cpp
  cod3_check/math.cpp
  cod3_check/script.cpp
  cod3_check/shared.cpp
)
target_link_libraries(cod3_check PUBLIC cod3)

add_executable(cod3_check_cpp cod3_check/main.cpp)
target_link_libraries(cod3_check_cpp PRIVATE cod3_check)

enable_testing()
add_executable(cod3_unit_tests tests/unit.cpp)
target_link_libraries(cod3_unit_tests PRIVATE cod3)
add_test(NAME unit COMMAND cod3_unit_tests)

# The original game's recorded answers (engine/tools/decomp_check/golden), the same
# files `cargo test -p decomp_check` replays through the Rust ports.
add_executable(cod3_golden tests/golden.cpp)
target_link_libraries(cod3_golden PRIVATE cod3_check)
add_test(NAME golden COMMAND cod3_golden ${CMAKE_CURRENT_SOURCE_DIR}/../tools/decomp_check/golden)

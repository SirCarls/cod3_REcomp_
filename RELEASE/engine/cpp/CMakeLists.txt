# The C++ version of the PC games' decomp (engine/cpp/README.md): the iw_pc
# library and pc_check_cpp, the checker's answerer. Windows and Linux.
#
#   cmake -S engine/cpp -B engine/cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build engine/cpp/build
cmake_minimum_required(VERSION 3.20)
project(iw_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)
endif()

add_library(iw_pc STATIC
  iw_pc/sha256.cpp
  iw_pc/net/delta.cpp
  iw_pc/net/huffman.cpp
  iw_pc/net/msg.cpp
  iw_pc/net/netchan.cpp
  iw_pc/net/oob.cpp
  iw_pc/net/protocol_data.cpp
  iw_pc/shared/filter.cpp
  iw_pc/shared/hash.cpp
  iw_pc/shared/info.cpp
  iw_pc/shared/strings.cpp
)
target_include_directories(iw_pc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(pc_check_cpp
  pc_check/main.cpp
  pc_check/net_delta.cpp
  pc_check/net_msg.cpp
  pc_check/net_netchan.cpp
  pc_check/net_oob.cpp
  pc_check/shared.cpp
)
target_link_libraries(pc_check_cpp PRIVATE iw_pc)

enable_testing()
add_executable(iw_pc_tests tests/tests.cpp)
target_link_libraries(iw_pc_tests PRIVATE iw_pc)
add_test(NAME iw_pc_tests COMMAND iw_pc_tests)

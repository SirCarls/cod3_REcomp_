# cod3 - the decomp trace build (docs/DECOMP_TRACE.md).
#
# CMakeLists.txt includes this with -DCOD3_DECOMP_TRACE=ON. Always in a build
# folder of its own (the setup tool's --trace-build uses out/build/trace):
#
#   cmake -S . -B out/build/trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOD3_DECOMP_TRACE=ON -DCOD3_BUILD_MP=OFF
#
# (only the campaign program is traced, so the multiplayer program is left out).
#
# What it changes, in that folder only:
#  - every generated-code target (the executable's cod3_recomp and each level
#    library) gets trace_prologue.h appended to its precompiled header, which
#    redefines REX_FUNC_PROLOGUE() to count calls and note callers;
#  - each of those gets trace_module.cpp (its slow path) and its function
#    table, made from generated/<module>/cod3_init.cpp;
#  - the executable gets game/decomp_trace.cpp (the writer) and the trace's
#    evidence collectors, and exports cod3_decomp_trace_registry() so the
#    level libraries can reach it.
# The generated code itself and its cod3_pch.h are the same as in a normal build.

set(COD3_TRACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/game/trace")

if(CMAKE_DISABLE_PRECOMPILE_HEADERS)
    message(FATAL_ERROR "COD3_DECOMP_TRACE adds its prologue through the precompiled headers; "
                        "configure without CMAKE_DISABLE_PRECOMPILE_HEADERS")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "COD3_DECOMP_TRACE needs clang or GCC (found ${CMAKE_CXX_COMPILER_ID})")
endif()

# One traced module: `target` compiles generated/<module>/.
function(cod3_trace_module target module)
    set(init_cpp "${CMAKE_CURRENT_SOURCE_DIR}/generated/${module}/cod3_init.cpp")
    set(names_cpp "${CMAKE_CURRENT_BINARY_DIR}/trace/trace_names_${module}.cpp")
    add_custom_command(
        OUTPUT "${names_cpp}"
        COMMAND ${CMAKE_COMMAND} "-DIN=${init_cpp}" "-DOUT=${names_cpp}"
                -P "${COD3_TRACE_DIR}/gen_trace_names.cmake"
        DEPENDS "${init_cpp}" "${COD3_TRACE_DIR}/gen_trace_names.cmake"
        COMMENT "Decomp trace: function table of ${module}"
        VERBATIM)
    target_precompile_headers(${target} PRIVATE "${COD3_TRACE_DIR}/trace_prologue.h")
    target_sources(${target} PRIVATE "${COD3_TRACE_DIR}/trace_module.cpp" "${names_cpp}")
    target_compile_definitions(${target} PRIVATE COD3_DECOMP_TRACE=1 "COD3_TRACE_MODULE_NAME=\"${module}\"")
endfunction()

if(TARGET cod3_recomp)
    cod3_trace_module(cod3_recomp default)
    # The executable's own slow path reaches the registry directly.
    target_compile_definitions(cod3_recomp PRIVATE COD3_TRACE_HOST=1)
endif()

foreach(level IN LISTS COD3_LEVELS)
    if(TARGET cod3_${level})
        cod3_trace_module(cod3_${level} ${level})
        # dlsym(RTLD_DEFAULT, ...) finds the executable's registry.
        target_link_libraries(cod3_${level} PRIVATE ${CMAKE_DL_LIBS})
    endif()
endforeach()

target_sources(cod3 PRIVATE
    src/game/decomp_trace.cpp
    src/game/trace/evidence.cpp
    src/game/trace/trace_format.cpp)
target_compile_definitions(cod3 PRIVATE COD3_DECOMP_TRACE=1)

# The trace files' rules on their own (cmake --build <folder> --target cod3_trace_format_test).
add_executable(cod3_trace_format_test EXCLUDE_FROM_ALL
    src/game/trace/trace_format.cpp
    src/game/trace/trace_format_test.cpp)
if(UNIX AND NOT APPLE)
    # Put the registry in the executable's dynamic symbols (Windows: dllexport).
    target_link_options(cod3 PRIVATE "LINKER:--dynamic-list=${COD3_TRACE_DIR}/exports.list")
endif()

message(STATUS "cod3: decomp trace build (COD3_DECOMP_TRACE=ON): this folder builds the tracing game")

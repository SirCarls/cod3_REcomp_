# [rust-renderer] cod3_render: the Rust renderer's static library (engine/crates/cod3_render;
# the plan is in dev/agents/rust-renderer/). M0: the skeleton. It is linked into cod3 and cod3mp
# so the build and the C ABI are proven on Linux and Windows; the game only checks the library's
# ABI at start and says so in its log (src/render/rust_renderer.cpp). It draws nothing yet.
#
# cargo builds it as a custom command (not Corrosion, which would be a download), into this build
# folder, offline and from the crate's own Cargo.lock (it is its own workspace, so engine/'s
# crates.io packages aren't needed): the library has no outside crates.
#
#   COD3_RUST_RENDERER  AUTO (default): linked when cargo, Rust 1.80+ and the Rust standard
#                       library for this build's target are there; otherwise the game builds
#                       without it and its log says so. ON: the same, but a missing piece stops
#                       the configure. OFF: never.
#   COD3_ENGINE_DIR     the project's engine/ folder. The setup tool passes it: its build folder
#                       is a copy of recomp/, so recomp/../../engine isn't there.
#   COD3_CARGO          the cargo to use. The setup tool passes the one it found (its own in
#                       work/tools/rust, or the PC's); otherwise cargo on PATH.
#
# cod3_render_link(<target>) links it into a program and defines COD3_HAS_RUST_RENDERER there.

set(COD3_RUST_RENDERER "AUTO" CACHE STRING "Link the Rust renderer library: AUTO, ON or OFF")
set_property(CACHE COD3_RUST_RENDERER PROPERTY STRINGS AUTO ON OFF)
set(COD3_ENGINE_DIR "" CACHE PATH "The project's engine/ folder (the Rust workspace)")
set(COD3_CARGO "" CACHE FILEPATH "cargo for the Rust renderer library")

# Where this file is: the default engine/ is three folders up (recomp/cmake -> the project).
set(_COD3_RENDER_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The Rust target triple for this build, or "" when the library isn't made for it.
function(_cod3_render_triple out)
  set(triple "")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" OR CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      set(triple "x86_64-unknown-linux-gnu")
    elseif(WIN32 AND (MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
      set(triple "x86_64-pc-windows-msvc")
    elseif(WIN32 AND MINGW)
      # llvm-mingw (the Windows build made on Linux) is UCRT with libunwind: Rust's gnullvm
      # target matches it. Not -gnu, which would want libgcc_eh.
      set(triple "x86_64-pc-windows-gnullvm")
    endif()
  endif()
  set(${out} "${triple}" PARENT_SCOPE)
endfunction()

# Says why the library is left out: a status line with AUTO, an error with ON.
macro(_cod3_render_without why)
  if(COD3_RUST_RENDERER STREQUAL "ON")
    message(FATAL_ERROR "cod3_render: ${why} (COD3_RUST_RENDERER=ON)")
  endif()
  message(STATUS "cod3_render: not linked: ${why}")
  set(COD3_RUST_RENDERER_LINKED OFF CACHE INTERNAL "")
  return()
endmacro()

function(_cod3_render_setup)
  set(COD3_RUST_RENDERER_LINKED OFF CACHE INTERNAL "")
  if(COD3_RUST_RENDERER STREQUAL "OFF")
    message(STATUS "cod3_render: off (COD3_RUST_RENDERER=OFF)")
    return()
  endif()

  set(engine "${COD3_ENGINE_DIR}")
  if(NOT engine)
    get_filename_component(engine "${_COD3_RENDER_MODULE_DIR}/../../../engine" ABSOLUTE)
  endif()
  set(crate "${engine}/crates/cod3_render")
  if(NOT EXISTS "${crate}/Cargo.toml" OR NOT EXISTS "${crate}/Cargo.lock")
    _cod3_render_without("no engine/crates/cod3_render at ${engine} (set COD3_ENGINE_DIR)")
  endif()

  _cod3_render_triple(triple)
  if(NOT triple)
    _cod3_render_without("no Rust target for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
  endif()

  # cargo and rustc run on the build machine: a cross build's find root (the Windows build made
  # on Linux) doesn't apply to them.
  set(cargo "${COD3_CARGO}")
  if(NOT cargo)
    find_program(_cod3_cargo_on_path cargo NO_CMAKE_FIND_ROOT_PATH)
    set(cargo "${_cod3_cargo_on_path}")
  endif()
  if(NOT cargo OR NOT EXISTS "${cargo}")
    _cod3_render_without("cargo not found")
  endif()
  get_filename_component(cargo_dir "${cargo}" DIRECTORY)
  find_program(_cod3_rustc rustc HINTS "${cargo_dir}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(NOT _cod3_rustc)
    find_program(_cod3_rustc rustc NO_CMAKE_FIND_ROOT_PATH)
  endif()
  if(NOT _cod3_rustc)
    _cod3_render_without("rustc not found next to ${cargo}")
  endif()

  # Rust 1.80 or newer (the workspace's rust-version).
  execute_process(COMMAND "${_cod3_rustc}" --version OUTPUT_VARIABLE rustc_version
                  RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT rc EQUAL 0 OR NOT rustc_version MATCHES "^rustc ([0-9]+)\\.([0-9]+)")
    _cod3_render_without("${_cod3_rustc} doesn't run")
  endif()
  if(CMAKE_MATCH_1 EQUAL 1 AND CMAKE_MATCH_2 LESS 80)
    _cod3_render_without("${rustc_version} is older than Rust 1.80")
  endif()

  # The standard library for the target, and the system libraries it needs when linked into a
  # C++ program: compile an empty static library once and let rustc say them. (On Linux that is
  # -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc; on Windows kernel32, ntdll, userenv, ws2_32 ...)
  set(probe "${CMAKE_BINARY_DIR}/cod3_render_probe")
  file(MAKE_DIRECTORY "${probe}")
  file(WRITE "${probe}/probe.rs" "// The Rust standard library's link needs (cod3_render.cmake).\n")
  execute_process(COMMAND "${_cod3_rustc}" --crate-type staticlib --crate-name cod3_render_probe
                          --edition 2021 --target ${triple} --print native-static-libs
                          -o "${probe}/probe.lib" "${probe}/probe.rs"
                  WORKING_DIRECTORY "${probe}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    string(REGEX REPLACE "\n.*" "" first_line "${err}")
    _cod3_render_without("no Rust standard library for ${triple} (rustup target add ${triple}): ${first_line}")
  endif()
  set(native "")
  if("${out}${err}" MATCHES "native-static-libs: ([^\n]*)")
    separate_arguments(words NATIVE_COMMAND "${CMAKE_MATCH_1}")
    foreach(word IN LISTS words)
      if(word MATCHES "^-l(.+)$")
        list(APPEND native "${CMAKE_MATCH_1}")
      elseif(word MATCHES "^([^/-].*)\\.lib$")
        list(APPEND native "${CMAKE_MATCH_1}")
      endif()   # MSVC's /defaultlib:msvcrt and the like: the program's own C runtime decides
    endforeach()
  endif()
  list(REMOVE_DUPLICATES native)

  if(triple MATCHES "msvc")
    set(lib_name "cod3_render.lib")
  else()
    set(lib_name "libcod3_render.a")
  endif()
  set(target_dir "${CMAKE_BINARY_DIR}/cargo")
  set(lib "${target_dir}/${triple}/release/${lib_name}")
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${crate}/src/*.rs")
  add_custom_command(
    OUTPUT "${lib}"
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${target_dir}"
            "${cargo}" build --release --locked --offline --target ${triple}
            --manifest-path "${crate}/Cargo.toml"
    DEPENDS ${sources} "${crate}/Cargo.toml" "${crate}/Cargo.lock"
    WORKING_DIRECTORY "${crate}"
    COMMENT "Building the Rust renderer library (cod3_render, ${triple})"
    VERBATIM)
  add_custom_target(cod3_render_cargo DEPENDS "${lib}")
  add_library(cod3_render STATIC IMPORTED GLOBAL)
  set_target_properties(cod3_render PROPERTIES
    IMPORTED_LOCATION "${lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${crate}/include"
    INTERFACE_LINK_LIBRARIES "${native}")
  add_dependencies(cod3_render cod3_render_cargo)
  set(COD3_RUST_RENDERER_LINKED ON CACHE INTERNAL "")
  message(STATUS "cod3_render: linked (${rustc_version}, ${triple}; system libraries: ${native})")
endfunction()

function(cod3_render_link target)
  if(COD3_RUST_RENDERER_LINKED AND TARGET cod3_render)
    target_link_libraries(${target} PRIVATE cod3_render)
    target_compile_definitions(${target} PRIVATE COD3_HAS_RUST_RENDERER=1)
    # llvm-mingw: Rust's standard library comes as one object file, so without this the
    # whole of it is linked, imports and all, and the program then needs ProcessPrng from
    # bcryptprimitives.dll (std's random keys, which this library never calls; Windows 7
    # doesn't have it). This drops what nothing calls, as MSVC's /OPT:REF does by default.
    if(MINGW)
      target_link_options(${target} PRIVATE "LINKER:--gc-sections")
    endif()
  endif()
endfunction()

_cod3_render_setup()

# xplat_find_ngl_vk(<PREFIX> TARGET <target> [DEFINE <macro>] [REQUIRE] [BINARY_DIR <dir>] [HINTS <dir>...])
# xplat_find_glslc(<out-var> [PREFIX <PREFIX>])
# xplat_rex_renderdoc_include(<target> <PREFIX>)
# [rexglue-native-platform-fix] 2026-11-23. One way for every game to build ngl_vk (the native renderer's Vulkan
# library) into itself: lifted from Call of Duty 3's recomp/CMakeLists.txt (its 11-22 version, acct2-bro), so the
# other ports get what CoD3 got (finding B1 of the 11-23 platform audit).
#
# What it finds, in order:
#   ngl_vk/      <PREFIX>_NGL_VK_DIR (a -D or the environment), else the first HINTS folder that has one (the game's
#                own places, kept so adopting changes nothing), else the folder next to xplat/ (this module's own
#                project; the setup tool passes XPLAT_DIR through the project folder's link, so a game configured from
#                a copy of its folder in the work folder still finds it, whatever the work folder's path is).
#   the headers  NGL_VK_VULKAN_INCLUDE_DIR when given; a build for another system (the Windows build made on Linux)
#                takes only its CMAKE_PREFIX_PATH (the SDK's install), never the build PC's own; a native build takes
#                FindVulkan's, else CMAKE_PREFIX_PATH's (a Windows PC without the Vulkan SDK: the SDK's install).
#   glslc        xplat_find_glslc below.
# Then ngl_vk is added (once), linked PRIVATE into <target>, and <macro>=1 defined there. <PREFIX>_NGL_VK_BUILT
# (cache, INTERNAL) says ON or OFF for the setup tool. With <PREFIX>_REQUIRE_NGL_VK (default: ON with REQUIRE, else
# OFF) a missing piece stops the configure, saying which and how to get it.
include_guard(GLOBAL)

get_filename_component(_XPLAT_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)  # xplat/cmake -> the project
set(_XPLAT_GLSLANG_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../tools/glslc_glslang.py")
set(_XPLAT_GLSLC_LAUNCHER "${CMAKE_CURRENT_LIST_DIR}/../tools/glslc_launcher.c")

# glslc for the native renderer's shaders. Given ones first (a file, used as it is): NGL_VK_GLSLC,
# <PREFIX>_NGL_VK_GLSLC, Vulkan_GLSLC_EXECUTABLE (the setup tool gives all three). Then glslc on PATH or in the
# Vulkan SDK ($VULKAN_SDK/Bin, which its installer may leave off PATH). Then Khronos' glslang with Python: a
# wrapper in the build folder runs xplat/tools/glslc_glslang.py, which takes glslc's options (a Windows PC has
# glslang from the setup tool, a Linux one from glslang-tools). Sets <out-var> to the program, or "".
function(xplat_find_glslc out)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "PREFIX" "")
    # Given: NGL_VK_GLSLC (ngl_vk/CMakeLists.txt's name), <PREFIX>_NGL_VK_GLSLC (the game's). Not FindVulkan's
    # Vulkan_GLSLC_EXECUTABLE: FindVulkan sets it by itself (to /bin/glslc on a merged-/usr Linux), and the games
    # only ever used it as a search hint, so taking it would change which glslc a build runs.
    set(names NGL_VK_GLSLC)
    if(ARG_PREFIX)
        list(APPEND names ${ARG_PREFIX}_NGL_VK_GLSLC)
    endif()
    foreach(name IN LISTS names)
        if(DEFINED ${name} AND EXISTS "${${name}}" AND NOT IS_DIRECTORY "${${name}}")
            set(${out} "${${name}}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(hints "")
    if(DEFINED ENV{VULKAN_SDK})
        list(APPEND hints "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    endif()
    # the build PC's program, also in a build for another system (its find root doesn't apply to programs).
    # XPLAT_GLSLC_VIA_GLSLANG: glslang through the wrapper even where glslc is (to test the wrapper on a PC that has
    # both, or past a broken glslc).
    option(XPLAT_GLSLC_VIA_GLSLANG "Compile shaders with glslang through xplat's glslc wrapper even when glslc is there" OFF)
    if(NOT XPLAT_GLSLC_VIA_GLSLANG)
        find_program(XPLAT_GLSLC NAMES glslc HINTS ${hints} NO_CMAKE_FIND_ROOT_PATH)
        if(XPLAT_GLSLC AND EXISTS "${XPLAT_GLSLC}")
            set(${out} "${XPLAT_GLSLC}" PARENT_SCOPE)
            return()
        endif()
    endif()
    find_program(XPLAT_GLSLANG NAMES glslang glslangValidator HINTS ${hints} NO_CMAKE_FIND_ROOT_PATH)
    if(Python3_EXECUTABLE AND EXISTS "${Python3_EXECUTABLE}")
        set(python "${Python3_EXECUTABLE}")
    else()
        find_package(Python3 COMPONENTS Interpreter QUIET)
        set(python "${Python3_EXECUTABLE}")
    endif()
    if(NOT XPLAT_GLSLANG OR NOT python OR NOT EXISTS "${_XPLAT_GLSLANG_SCRIPT}")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    xplat__glslang_wrapper(wrapper "${python}" "${XPLAT_GLSLANG}")
    message(STATUS "xplat: no glslc; the shaders go through glslang (${XPLAT_GLSLANG}) with ${wrapper}")
    set(${out} "${wrapper}" PARENT_SCOPE)
endfunction()

# The wrapper glslc. On a Windows build PC: glslc.exe, tools/glslc_launcher.c built for this PC (try_compile), which
# runs glslc.args (Python, -I, glslc_glslang.py, --glslang, glslang: one a line, UTF-8) with glslc's arguments after
# them, through wide functions only, so the folders may have any name. Elsewhere: an executable shell script. The
# script itself is copied into the build folder; Python is Python3_EXECUTABLE when given (the setup tool passes the
# console Python it found), else CMake's FindPython3.
# [rexglue-native-platform-fix] 2026-11-23 (the Wine run): the Windows wrapper was a glslc.cmd. cmd reads a batch file
# in the console's code page, so a Python or glslang outside ASCII needed "chcp 65001" first; under Wine cmd still read
# the next line in the old code page ("Can't recognize" Python, its folder's name garbled), and chcp changes the code
# page of the build's whole console. The .cmd stays only for a build PC where the launcher can't be built.
function(xplat__glslang_wrapper out python glslang)
    set(dir "${CMAKE_BINARY_DIR}/xplat-glslc")
    file(MAKE_DIRECTORY "${dir}")
    file(COPY "${_XPLAT_GLSLANG_SCRIPT}" DESTINATION "${dir}")
    # cmake_path, not file(TO_NATIVE_PATH): off Windows that one splits a path at ':' and writes a space as "\\ ", which
    # broke the sh wrapper for a Python or glslang in a folder with a space (found by the 11-23 Wine run's tests)
    cmake_path(NATIVE_PATH python py)
    cmake_path(NATIVE_PATH glslang tool)
    if(CMAKE_HOST_WIN32)
        set(file "")
        if(WIN32 AND NOT CMAKE_CROSSCOMPILING)  # a program for this PC
            xplat__glslc_launcher(file "${dir}" "${py}" "${tool}")
        endif()
        if(NOT file)
            # glslc.cmd: names it by %~dp0 (its own folder), so the build folder's path isn't in the file; every "%"
            # in a path doubled (cmd reads "%name%" as a variable even inside quotes); a path outside ASCII gets
            # "chcp 65001" first (see above: not enough everywhere)
            set(lines "")
            if(NOT "${py}${tool}" MATCHES "^[ -~]*$")
                string(APPEND lines "@chcp 65001 >nul\r\n")
            endif()
            foreach(v py tool)
                string(REPLACE "%" "%%" ${v} "${${v}}")
            endforeach()
            string(APPEND lines "@\"${py}\" -I \"%~dp0glslc_glslang.py\" --glslang \"${tool}\" %*\r\n")
            set(file "${dir}/glslc.cmd")
            file(WRITE "${file}" "${lines}")
        endif()
    else()
        foreach(v py tool)
            string(REPLACE "'" "'\\''" ${v} "${${v}}")  # inside '...' only ' itself needs care
        endforeach()
        set(file "${dir}/glslc")
        file(WRITE "${file}" "#!/bin/sh\nexec '${py}' -I \"$(dirname \"$0\")/glslc_glslang.py\" --glslang '${tool}' \"$@\"\n")
        file(CHMOD "${file}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ
             WORLD_EXECUTE)
    endif()
    set(${out} "${file}" PARENT_SCOPE)
endfunction()

# glslc.exe and glslc.args in <dir>; <out> is glslc.exe, or "" (and a warning) when it can't be built.
function(xplat__glslc_launcher out dir py tool)
    get_property(languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    set(source "${_XPLAT_GLSLC_LAUNCHER}")
    if(NOT "C" IN_LIST languages)  # the file is C that also builds as C++
        configure_file("${source}" "${dir}/glslc_launcher.cpp" COPYONLY)
        set(source "${dir}/glslc_launcher.cpp")
    endif()
    try_compile(_xplat_launcher_built "${dir}/launcher-build" "${source}" LINK_LIBRARIES shell32
                OUTPUT_VARIABLE log COPY_FILE "${dir}/glslc.exe" COPY_FILE_ERROR copy_error)
    if(NOT _xplat_launcher_built OR copy_error)
        string(SUBSTRING "${log}${copy_error}" 0 2000 log)
        message(WARNING "xplat: the glslc launcher didn't build, so glslc.cmd is used (a Python or glslang folder "
                "outside ASCII may not work through it):\n${log}")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    file(WRITE "${dir}/glslc.args" "${py}\n-I\n${dir}/glslc_glslang.py\n--glslang\n${tool}\n")
    set(${out} "${dir}/glslc.exe" PARENT_SCOPE)
endfunction()

function(xplat_find_ngl_vk prefix)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "REQUIRE" "TARGET;DEFINE;BINARY_DIR" "HINTS")
    if(NOT ARG_TARGET)
        message(FATAL_ERROR "xplat_find_ngl_vk(${prefix}): TARGET is needed")
    endif()
    string(TOLOWER "${prefix}" name)
    set(${prefix}_NGL_VK_DIR "" CACHE PATH "The project's ngl_vk/ folder (the native renderer's Vulkan library)")
    option(${prefix}_REQUIRE_NGL_VK "Stop when the native renderer (ngl_vk) can't be built into ${ARG_TARGET}"
           ${ARG_REQUIRE})
    if(NOT ARG_BINARY_DIR)
        set(ARG_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/ngl_vk")
    endif()

    # 1. the folder
    set(dir "${${prefix}_NGL_VK_DIR}")
    if(NOT dir AND DEFINED ENV{${prefix}_NGL_VK_DIR})
        set(dir "$ENV{${prefix}_NGL_VK_DIR}")
    endif()
    foreach(hint IN LISTS ARG_HINTS)  # the game's own places, in its order (CoD3: next to engine/, two up)
        if(NOT dir AND EXISTS "${hint}/CMakeLists.txt")
            get_filename_component(dir "${hint}" ABSOLUTE)
        endif()
    endforeach()
    if(NOT dir)
        set(dir "${_XPLAT_PROJECT_DIR}/ngl_vk")
    endif()
    set(why "")
    if(NOT TARGET ngl_vk AND NOT EXISTS "${dir}/CMakeLists.txt")
        set(why "the project's ngl_vk/ folder (looked in ${dir}; ${prefix}_NGL_VK_DIR names it)")
    endif()

    # 2. the headers
    if(NOT why AND NOT NGL_VK_VULKAN_INCLUDE_DIR)
        if(CMAKE_CROSSCOMPILING)
            find_path(NGL_VK_VULKAN_INCLUDE_DIR vulkan/vulkan.h NO_CMAKE_SYSTEM_PATH NO_SYSTEM_ENVIRONMENT_PATH
                      NO_CMAKE_ENVIRONMENT_PATH)
        else()
            find_package(Vulkan QUIET)
            if(Vulkan_INCLUDE_DIR)
                set(NGL_VK_VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIR}")
            else()
                find_path(XPLAT_VULKAN_PREFIX_HEADERS vulkan/vulkan.h NO_SYSTEM_ENVIRONMENT_PATH)
                set(NGL_VK_VULKAN_INCLUDE_DIR "${XPLAT_VULKAN_PREFIX_HEADERS}")
            endif()
        endif()
    endif()
    if(NOT why AND NOT NGL_VK_VULKAN_INCLUDE_DIR)
        set(why "the Vulkan headers (vulkan/vulkan.h: the SDK's install in CMAKE_PREFIX_PATH, or the Vulkan SDK)")
    endif()

    # 3. glslc
    if(NOT why)
        xplat_find_glslc(glslc PREFIX ${prefix})
        if(glslc)
            set(NGL_VK_GLSLC "${glslc}" CACHE FILEPATH "glslc for ngl_vk's shaders" FORCE)
        else()
            set(why "glslc (Linux: the glslc or shaderc package; Windows: the setup tool's glslang, or the Vulkan SDK)")
        endif()
    endif()

    # 4. in, or why not
    if(NOT why)
        if(NOT TARGET ngl_vk)
            add_subdirectory("${dir}" "${ARG_BINARY_DIR}")
        endif()
        target_link_libraries(${ARG_TARGET} PRIVATE ngl_vk)
        if(ARG_DEFINE)
            target_compile_definitions(${ARG_TARGET} PRIVATE ${ARG_DEFINE}=1)
        endif()
        message(STATUS "${name}: ngl_vk from ${dir}")
        set(${prefix}_NGL_VK_BUILT ON CACHE INTERNAL "The native renderer (ngl_vk) is in this build")
    else()
        message(STATUS "${name}: no ngl_vk (${why})")
        set(${prefix}_NGL_VK_BUILT OFF CACHE INTERNAL "The native renderer (ngl_vk) is in this build")
        if(${prefix}_REQUIRE_NGL_VK)
            message(FATAL_ERROR "${name}: the native renderer (ngl_vk) can't be built: ${why} wasn't found. The setup "
                    "tool brings everything it needs; building by hand, install what is named, or configure with "
                    "-D${prefix}_REQUIRE_NGL_VK=OFF to build without it.")
        endif()
    endif()
    set(NGL_VK_VULKAN_INCLUDE_DIR "${NGL_VK_VULKAN_INCLUDE_DIR}" PARENT_SCOPE)
    set(${prefix}_NGL_VK_WHY "${why}" PARENT_SCOPE)
endfunction()

# The ReXGlue SDK's renderdoc_app.h, which the SDK's Vulkan presenter headers include by its bare name but which is on
# none of the SDK targets' include paths (installed: include/renderdoc; source tree: thirdparty/renderdoc). For the
# games whose native side shows its picture through that presenter.
function(xplat_rex_renderdoc_include target prefix)
    get_target_property(rex_includes rex::runtime INTERFACE_INCLUDE_DIRECTORIES)
    find_path(${prefix}_RENDERDOC_INCLUDE_DIR renderdoc_app.h
              HINTS ${rex_includes} "${rexglue_DIR}/../../../include" "${REXSDK_DIR}/thirdparty"
              PATH_SUFFIXES renderdoc NO_DEFAULT_PATH)
    if(${prefix}_RENDERDOC_INCLUDE_DIR)
        target_include_directories(${target} PRIVATE "${${prefix}_RENDERDOC_INCLUDE_DIR}")
    else()
        string(TOLOWER "${prefix}" name)
        message(WARNING "${name}: the SDK's renderdoc_app.h wasn't found; the native side's window code won't build")
    endif()
endfunction()

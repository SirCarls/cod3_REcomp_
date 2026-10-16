"""Building the Windows game (cod3.exe) on Linux, with llvm-mingw.

The owner chose the open-source route (2026-10-07): llvm-mingw brings
clang, lld, the mingw-w64 Windows headers and C runtime and LLVM's libc++, so
no Microsoft license is involved. See cross/mingw/README.md.

What's needed beyond the normal Linux build:

- the llvm-mingw toolchain (a pinned, checksummed download, deps.PORTABLE);
- a CMake toolchain file and a header folder (`prepare()`), written into
  the work folder: the project's own headers from cross/mingw/include, plus
  links for Windows headers the SDK includes with other capitalization;
- the SDK built a second time, for Windows (`sdk_configure_args()`);
- the game configured for Windows with the Linux `rexglue` running the
  codegen (`game_configure_args()`);
- packaging with llvm-mingw's C++ runtime DLLs (`runtime_dll_folder()`), and
  Windows' own DLLs recognized by name (`is_windows_system_dll()`).
"""
from __future__ import annotations

import os
import re
import shutil
from pathlib import Path
from typing import Iterable, List

# Windows DLLs that come with Windows 10 and 11 (the game's minimum). A DLL
# not in this list, not built with the game and not in llvm-mingw makes
# packaging stop, so nothing silently goes missing. The graphics DLLs are
# loaded at run time by name, not imported (d3d12, dxgi, vulkan-1: the
# Vulkan loader comes with the graphics driver, and Wine/Proton has its own);
# the GPU plugin itself is looked up explicitly (binaries.find_gpu_plugin).
WINDOWS_SYSTEM_DLLS = {
    "advapi32.dll", "bcrypt.dll", "cfgmgr32.dll", "comctl32.dll", "comdlg32.dll", "crypt32.dll", "d3d11.dll",
    "d3d12.dll", "d3dcompiler_47.dll", "dbghelp.dll", "dinput8.dll", "dwmapi.dll", "dxgi.dll", "gdi32.dll",
    "hid.dll", "imm32.dll", "iphlpapi.dll", "kernel32.dll", "kernelbase.dll", "mfplat.dll", "msvcrt.dll",
    "ntdll.dll", "ole32.dll", "oleaut32.dll", "powrprof.dll", "psapi.dll", "secur32.dll", "setupapi.dll",
    "shell32.dll", "shlwapi.dll", "ucrtbase.dll", "user32.dll", "userenv.dll", "uxtheme.dll", "version.dll",
    "vulkan-1.dll", "winhttp.dll", "winmm.dll", "ws2_32.dll", "xinput1_4.dll", "xinput9_1_0.dll",
}

TOOLCHAIN_TRIPLE = "x86_64-w64-mingw32"
SOURCE_SUFFIXES = (".h", ".hpp", ".c", ".cc", ".cpp", ".inl")
INCLUDE = re.compile(rb'#\s*include\s*[<"]([A-Za-z0-9_./]+\.h)[>"]')


def is_windows_system_dll(name: str) -> bool:
    low = name.lower()
    return low in WINDOWS_SYSTEM_DLLS or low.startswith(("api-ms-win-", "ext-ms-"))


def sysroot(toolchain_bin: Path) -> Path:
    """llvm-mingw's Windows sysroot (headers, import libraries, runtime DLLs)."""
    return Path(toolchain_bin).parent / TOOLCHAIN_TRIPLE


def runtime_dll_folder(toolchain_bin: Path) -> Path:
    """Where llvm-mingw keeps the DLLs its programs need (libc++, libunwind)."""
    return sysroot(toolchain_bin) / "bin"


def case_links(sources: Iterable[Path], headers: Path, dest: Path) -> List[str]:
    """Links in `dest` for every header the sources include under a name that
    differs from the toolchain's file only in capitalization (Windows file
    names ignore case, Linux ones don't). Returns the names linked."""
    have = {}
    for root, _, files in os.walk(headers):
        for f in files:
            rel = Path(root, f).relative_to(headers).as_posix()
            have.setdefault(rel.lower(), rel)
    wanted = set()
    for top in sources:
        for root, dirs, files in os.walk(top):
            dirs[:] = [d for d in dirs if d not in ("out", ".git")]
            for f in files:
                if f.endswith(SOURCE_SUFFIXES):
                    try:
                        data = Path(root, f).read_bytes()
                    except OSError:
                        continue
                    wanted.update(m.decode("ascii") for m in INCLUDE.findall(data))
    made = []
    for name in sorted(wanted):
        real = have.get(name.lower())
        if not real or real == name:
            continue
        link = dest / name
        if os.path.lexists(link):
            continue
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(headers / real)
        made.append(name)
    return made


def prepare(work: Path, toolchain_bin: Path, project_headers: Path, sdk_src: Path) -> Path:
    """Writes the CMake toolchain file and the extra header folder into
    work/mingw/ (`include/` should start empty). Returns the toolchain file."""
    base = work / "mingw"
    include = base / "include"   # the caller clears it first (pipeline: safe_rmtree)
    include.mkdir(parents=True, exist_ok=True)
    for header in Path(project_headers).glob("*.h"):
        shutil.copyfile(header, include / header.name)
    case_links([sdk_src / "src", sdk_src / "include", sdk_src / "thirdparty"],
               sysroot(toolchain_bin) / "include", include)
    tc = Path(toolchain_bin)
    toolchain = base / "toolchain.cmake"
    toolchain.write_text(f"""# Written by the COD_3_WAW setup tool: Windows (x86-64) with llvm-mingw.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "{(tc / (TOOLCHAIN_TRIPLE + '-clang')).as_posix()}")
set(CMAKE_CXX_COMPILER "{(tc / (TOOLCHAIN_TRIPLE + '-clang++')).as_posix()}")
set(CMAKE_RC_COMPILER "{(tc / (TOOLCHAIN_TRIPLE + '-windres')).as_posix()}")
set(CMAKE_FIND_ROOT_PATH "{sysroot(tc).as_posix()}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
# The SDK's own target CPU level, and the headers llvm-mingw lacks or spells
# differently (searched last, so they never hide the toolchain's own).
set(CMAKE_C_FLAGS_INIT "-march=x86-64-v2 -idirafter \\"{include.as_posix()}\\"")
set(CMAKE_CXX_FLAGS_INIT "-march=x86-64-v2 -idirafter \\"{include.as_posix()}\\"")
""", encoding="utf-8")
    return toolchain


def common_args(toolchain: Path) -> List[str]:
    return [f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"]


def sdk_configure_args(toolchain: Path, install: Path) -> List[str]:
    """Extra CMake arguments for the SDK's Windows build."""
    return common_args(toolchain) + [
        "-DCMAKE_CXX_STANDARD=23", "-DREXGLUE_BUILD_TESTS=OFF", "-DBUILD_TESTING=OFF", "-DREXGLUE_ENABLE_FIDELITYFX=OFF",
        # Vulkan next to Direct3D 12, chosen in F2 (cod3_graphics_api).
        "-DREXGLUE_USE_VULKAN=ON",
        f"-DCMAKE_INSTALL_PREFIX={install}"]


def game_configure_args(toolchain: Path, sdk_install: Path, host_rexglue: Path) -> List[str]:
    """Extra CMake arguments for the game's Windows build."""
    return common_args(toolchain) + [f"-DCMAKE_PREFIX_PATH={sdk_install}", f"-DREXGLUE_CODEGEN={host_rexglue}"]


__all__ = ["WINDOWS_SYSTEM_DLLS", "is_windows_system_dll", "sysroot", "runtime_dll_folder", "case_links",
           "prepare", "sdk_configure_args", "game_configure_args"]

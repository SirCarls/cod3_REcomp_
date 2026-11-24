"""[acct2-bro] 2026-11-22 Whether a CMake build folder can be configured again as it is, judged the way CMake judges
it (owner 23:44: "This needs to be fixed permanently and not be a band-aid fix"; 23:48: "The setup process really needs
to be robust and not confusing or easy to mess up").

The user reports of 2026-11-23 (Windows, setup tool 2.47.0): 2.47.0 reaches the work folder through a link with a new
name (C:\\c3w-work-<8 hex digits> instead of C:\\c3w-work). The game's build folder had been configured through the old
name. The setup tool kept it: its check compared the compilers after following both links (the same files), and it
didn't compare the source folder for the game at all. CMake compares the paths as they are written, so it saw another
compiler, threw its cache away in the middle of the configure and ran it again without the -D settings of the command
line: the game's native renderer folder (found from COD3_ENGINE_DIR) was gone and the configure stopped. The same
cache also still named the SDK by the old link (rexglue_DIR), which find_package never looks up again once cached.

stale_reasons() lists every difference CMake would act on (or, for the SDK, silently live with) between a build
folder's CMakeCache.txt and the configure about to run; the setup tool starts the folder over when there is any, so a
configure never runs on a cache made for another place, compiler, generator or SDK.
"""
from __future__ import annotations

import os
import re
import shutil
from pathlib import Path
from typing import Dict, List, Optional, Union

PathLike = Union[str, Path]


def cache_entries(text: str) -> Dict[str, str]:
    """NAME -> value of a CMakeCache.txt (NAME:TYPE=value lines; comments and blank lines skipped)."""
    out: Dict[str, str] = {}
    for line in text.splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        m = re.match(r"^([^:=]+):[A-Za-z_]+=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2).strip()
    return out


def cmake_path(p: PathLike) -> str:
    """A path as CMake compares one (cmSystemTools::ConvertToUnixSlashes): backslashes to slashes, runs of slashes to
    one, no trailing slash. Nothing is resolved: a link's name is not its target's."""
    s = str(p).strip().replace("\\", "/")
    head = "//" if s.startswith("//") else ""   # a UNC path keeps its two leading slashes
    s = head + re.sub(r"/+", "/", s[len(head):])
    if len(s) > 1 and s.endswith("/") and not re.match(r"^[A-Za-z]:/$", s):
        s = s.rstrip("/")
    return s


def same_path(a: PathLike, b: PathLike, windows: bool = os.name == "nt") -> bool:
    """CMake's own comparison of two paths as written (case too, as CMake's is; a drive letter's case aside)."""
    x, y = cmake_path(a), cmake_path(b)
    if windows and len(x) > 1 and len(y) > 1 and x[1] == ":" and y[1] == ":":
        x, y = x[0].lower() + x[1:], y[0].lower() + y[1:]
    return x == y


def inside(path: PathLike, folder: PathLike, windows: bool = os.name == "nt") -> bool:
    """path is folder or under it, both as written (case-insensitive on Windows, where the file system is)."""
    p, f = cmake_path(path), cmake_path(folder)
    if windows:
        p, f = p.lower(), f.lower()
    return p == f or p.startswith(f.rstrip("/") + "/")


def _as_cmake_finds(program: PathLike) -> Optional[str]:
    """A program named without a folder is found on PATH, as CMake finds one (None: not found, not compared)."""
    s = str(program)
    if os.path.isabs(s) or re.match(r"^[A-Za-z]:[\\/]", s):
        return s
    return shutil.which(s)


def stale_reasons(cache_text: str, source: Optional[PathLike] = None,
                  compilers: Optional[Dict[str, PathLike]] = None, make_program: Optional[PathLike] = None,
                  generator: Optional[str] = None, sdk_prefix: Optional[PathLike] = None,
                  toolchain_file: Optional[PathLike] = None, windows: bool = os.name == "nt") -> List[str]:
    """Why a build folder with this CMakeCache.txt can't take the configure about to run ([] when it can).

    source: the folder -S names; compilers: {"CMAKE_C_COMPILER": path, "CMAKE_CXX_COMPILER": path}, as -D gives them;
    make_program: -DCMAKE_MAKE_PROGRAM; generator: -G; sdk_prefix: the SDK install -DCMAKE_PREFIX_PATH names (the
    cached rexglue_DIR must be inside it); toolchain_file: -DCMAKE_TOOLCHAIN_FILE (a cross build's compilers come
    from it, and CMake reads it only on a folder's first configure)."""
    e = cache_entries(cache_text)
    why: List[str] = []
    home = e.get("CMAKE_HOME_DIRECTORY")
    if source is not None and home and not same_path(home, source, windows):
        why.append(f"configured from {home}, now from {cmake_path(source)}")
    programs = dict(compilers or {})
    if make_program is not None:
        programs["CMAKE_MAKE_PROGRAM"] = make_program
    for var, given in programs.items():
        was, now = e.get(var), _as_cmake_finds(given)
        if was and now and not same_path(was, now, windows):
            why.append(f"{var} was {was}, now {cmake_path(now)}")
    was = e.get("CMAKE_TOOLCHAIN_FILE")
    if toolchain_file is not None and was and not same_path(was, toolchain_file, windows):
        why.append(f"CMAKE_TOOLCHAIN_FILE was {was}, now {cmake_path(toolchain_file)}")
    was = e.get("CMAKE_GENERATOR")
    if generator is not None and was and was != generator:
        why.append(f"generator {was}, now {generator}")
    was = e.get("rexglue_DIR")
    if sdk_prefix is not None and was and not was.endswith("-NOTFOUND") and not inside(was, sdk_prefix, windows):
        why.append(f"made with the SDK at {was}, now {cmake_path(sdk_prefix)}")
    return why


def sdk_in_use_ok(cache_text: str, sdk_prefix: PathLike, windows: bool = os.name == "nt") -> Optional[str]:
    """After a configure: None when the game was configured with the SDK in sdk_prefix, else what it used."""
    was = cache_entries(cache_text).get("rexglue_DIR", "")
    if was and not was.endswith("-NOTFOUND") and inside(was, sdk_prefix, windows):
        return None
    return was or "(none)"


__all__ = ["cache_entries", "cmake_path", "same_path", "inside", "stale_reasons", "sdk_in_use_ok"]

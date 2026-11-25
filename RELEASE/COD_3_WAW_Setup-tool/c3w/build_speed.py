"""Faster builds on whatever PC builds, each choice made there with a fallback ([waw-decomp/speed1] 2026-11-23).

The owner, 2026-11-23 20:32: "speed up compilation process, has to be dynamic and work on ALL systems."

Every choice is made on the PC that builds, when it builds, and falls back to what the setup tool did before:

- jobs(): no more compiler jobs than the processor has threads and than the memory that is free *now* can hold
  (MEMORY_PER_JOB_GB each). common.recommended_jobs() looks at the PC's total memory only, so a 16 GB PC with a
  browser open could start more jobs than its free memory holds and swap on the large generated files.
- The linker, Linux only: with C3W_LINKER=auto, mold, else lld, the first that links and runs a test program with
  every compiler of the build; else the compiler's own default (GNU ld). LINKER_DEFAULT says what an unset C3W_LINKER
  means. Windows needs nothing: CMake already links clang's MSVC-style programs with lld-link, and llvm-mingw (the
  Windows build made on Linux) links with lld.
- A compiler cache: ccache, else sccache, when one is installed and compiles a test file. Only for a build folder
  CMake hasn't configured yet, or one that already uses that cache: adding it to a configured folder would change
  every compile command, so everything would compile once more. On Windows only when asked for (not yet tried on a
  Windows PC). The cache gives back the object the compiler made for the same input.
- Links at most link_jobs() at a time (a ninja pool sized by free memory), so the many large links at the end of a
  build don't all run at once on a PC with little free memory.

Environment overrides, for a PC where an automatic choice misbehaves:
  C3W_LINKER=default|auto|lld|mold   C3W_COMPILER_CACHE=off|ccache|sccache
C3W_LINKER=default with C3W_COMPILER_CACHE=off builds exactly as before (the link pool changes only the order).
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Dict, List, Mapping, Optional, Sequence, Tuple

from .common import IS_LINUX, IS_WINDOWS, cpu_count, total_ram_gb

# Memory one compiler job takes, rounded up from the largest generated file of every port (measured 2026-11-23: Big Red
# One's biggest C file with GCC 0.71 GB, Finest Hour's 0.51 GB, Call of Duty 3's and World at War's generated C++ with
# clang 0.24 GB; the one larger compile, Call of Duty 3's hand-written ngl_side.cpp at 1.2 GB, is a single job).
# The numbers: dev/agents/waw-decomp/SPEED1-2026-11-23.md.
MEMORY_PER_JOB_GB = 1.0
# Memory one link takes, rounded up (lld: Big Red One's 0.42 GB, cod3 0.25 GB; GNU ld less).
MEMORY_PER_LINK_GB = 0.5
# Left to the rest of the PC (the desktop, the setup tool itself) when jobs are counted.
MEMORY_RESERVE_GB = 1.0

LINKER_ENV = "C3W_LINKER"
# What an unset C3W_LINKER means: "default" (the compilers' own linker) until the runs of the lld-linked programs pass
# (dev/agents/waw-decomp/SPEED1-2026-11-23.md), then "auto". lld links Call of Duty 3's 17 programs and libraries in
# 1.7 s instead of 3.9 s, Big Red One in 0.2 s instead of 5.2 s, Finest Hour in 0.1 s instead of 2.6 s (2026-11-23).
LINKER_DEFAULT = "default"
CACHE_ENV = "C3W_COMPILER_CACHE"
LINKERS = ("mold", "lld")              # tried in this order
CACHES = ("ccache", "sccache")         # tried in this order
OFF_WORDS = ("off", "no", "none", "0", "default")
LINKER_FLAG_VARS = ("CMAKE_EXE_LINKER_FLAGS", "CMAKE_SHARED_LINKER_FLAGS", "CMAKE_MODULE_LINKER_FLAGS")
LAUNCHER_VARS = ("CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER")
LINK_POOL = "link"
# What cmake_args() adds to a configure command, for comparing two commands without it (bro_windows.configure_folder).
SPEED_ARG = re.compile(r"^-D(CMAKE_(EXE|SHARED|MODULE)_LINKER_FLAGS|CMAKE_(C|CXX)_COMPILER_LAUNCHER|"
                       r"CMAKE_JOB_POOLS|CMAKE_JOB_POOL_LINK)(:[A-Z]+)?=")
OUR_LINKER_FLAG = re.compile(r"^-fuse-ld=(mold|lld)$")   # what cmake_args() sets, and nothing else
PROBE_SOURCE = "int main(void) { return 0; }\n"
PROBE_TIMEOUT = 120   # seconds; a first run of a compiler can be slow on a cold disk

_probed: Dict[Tuple, object] = {}       # probe results, once per compiler set and PATH
_last_said: List[str] = [""]            # the last choice written to the log (said again only when it changes)


def available_ram_gb() -> float:
    """Memory free for new programs right now, in GB: on Linux MemAvailable (it counts the file cache the system can
    drop), on Windows the available physical memory; the PC's total memory when neither can be read."""
    try:
        if IS_WINDOWS:
            import ctypes

            class MemoryStatus(ctypes.Structure):
                _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong),
                            ("total", ctypes.c_ulonglong), ("available", ctypes.c_ulonglong),
                            ("total_page", ctypes.c_ulonglong), ("available_page", ctypes.c_ulonglong),
                            ("total_virtual", ctypes.c_ulonglong), ("available_virtual", ctypes.c_ulonglong),
                            ("available_extended", ctypes.c_ulonglong)]
            status = MemoryStatus()
            status.length = ctypes.sizeof(MemoryStatus)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return status.available / 1024 ** 3
        elif IS_LINUX:
            with open("/proc/meminfo", encoding="ascii") as f:
                for line in f:
                    if line.startswith("MemAvailable:"):
                        return int(line.split()[1]) / 1024 ** 2
    except (OSError, ValueError, AttributeError):
        pass
    return total_ram_gb()   # other systems (macOS, BSD), or an answer that couldn't be read


def jobs(requested: int, log=None, per_job_gb: float = MEMORY_PER_JOB_GB) -> int:
    """The compiler jobs to run: `requested` (the user's choice, or common.recommended_jobs()), lowered to the
    processor's threads and to what the memory free right now holds at per_job_gb each; never below 1."""
    requested = max(1, int(requested or 1))
    free = available_ram_gb()
    by_memory = max(1, int((free - MEMORY_RESERVE_GB) // per_job_gb))
    chosen = max(1, min(requested, cpu_count(), by_memory))
    if log is not None and chosen < requested:
        log.info(f"Building with {chosen} jobs instead of {requested}: {free:.1f} GB of memory is free right now and "
                 f"a job can take about {per_job_gb:g} GB (closing other programs lets the next build use more).")
    return chosen


def link_jobs(per_link_gb: float = MEMORY_PER_LINK_GB) -> int:
    """How many links may run at once: what the memory free right now holds, at most one per processor thread."""
    return max(1, min(cpu_count(), int((available_ram_gb() - MEMORY_RESERVE_GB) // per_link_gb)))


def _environment(env: Optional[Mapping[str, str]]) -> Dict[str, str]:
    return dict(os.environ if env is None else env)


def _choice(env: Mapping[str, str], name: str) -> str:
    return env.get(name, "").strip().lower()


def _run_quietly(argv: Sequence[str], env: Mapping[str, str], cwd: str) -> bool:
    """Runs argv without a window or output; True when it ran and returned 0."""
    kwargs = {"creationflags": 0x08000000} if IS_WINDOWS else {}   # CREATE_NO_WINDOW
    try:
        done = subprocess.run([str(a) for a in argv], cwd=cwd, env=dict(env), stdin=subprocess.DEVNULL,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=PROBE_TIMEOUT, **kwargs)
    except (OSError, subprocess.SubprocessError):
        return False
    return done.returncode == 0


def _probe_source(folder: str, compiler: str) -> str:
    """A test source in folder, as C or C++ to match the compiler driver."""
    name = "probe.cpp" if "++" in Path(compiler).name else "probe.c"
    path = Path(folder) / name
    path.write_text(PROBE_SOURCE, encoding="ascii")
    return str(path)


def _links_with(compiler: str, linker: str, env: Mapping[str, str]) -> bool:
    """compiler -fuse-ld=linker builds a test program, and the program runs."""
    with tempfile.TemporaryDirectory(prefix="c3w-linker-") as folder:
        exe = str(Path(folder) / "probe")
        return (_run_quietly([compiler, f"-fuse-ld={linker}", _probe_source(folder, compiler), "-o", exe], env, folder)
                and _run_quietly([exe], env, folder))


def linker(compilers: Sequence, env: Optional[Mapping[str, str]] = None) -> str:
    """The linker for a Linux build with these compiler drivers: "" for the compilers' own default (C3W_LINKER unset
    while LINKER_DEFAULT is "default", or "default"), else "mold" or "lld" (the one asked for, or with "auto" the first
    that links a test program that runs, with every one of them; "" when none does). "" on other systems."""
    if not IS_LINUX:
        return ""
    env = _environment(env)
    wanted = _choice(env, LINKER_ENV) or LINKER_DEFAULT
    if wanted in OFF_WORDS:
        return ""
    names = [wanted] if wanted in LINKERS else list(LINKERS)   # "auto": each in turn
    drivers = tuple(str(c) for c in compilers if c)
    if not drivers:
        return ""
    key = ("linker", drivers, tuple(names), env.get("PATH", ""))
    if key not in _probed:
        _probed[key] = next((n for n in names if all(_links_with(d, n, env) for d in drivers)), "")
    return str(_probed[key])


def compiler_cache(compilers: Sequence, env: Optional[Mapping[str, str]] = None) -> Optional[Path]:
    """ccache or sccache, when installed and it compiles a test file with the first of these compilers; None when
    neither (or C3W_COMPILER_CACHE=off). On Windows only when C3W_COMPILER_CACHE names one."""
    env = _environment(env)
    wanted = _choice(env, CACHE_ENV)
    if wanted in OFF_WORDS or (IS_WINDOWS and wanted not in CACHES):
        return None
    drivers = [str(c) for c in compilers if c]
    if not drivers:
        return None
    key = ("cache", drivers[0], wanted, env.get("PATH", ""))
    if key not in _probed:
        _probed[key] = None
        for name in ([wanted] if wanted in CACHES else list(CACHES)):
            found = shutil.which(name, path=env.get("PATH"))
            if not found:
                continue
            with tempfile.TemporaryDirectory(prefix="c3w-cache-") as folder:
                src = _probe_source(folder, drivers[0])
                if _run_quietly([found, drivers[0], "-c", src, "-o", str(Path(folder) / "probe.o")], env, folder):
                    _probed[key] = Path(found)
                    break
    found_cache = _probed[key]
    return found_cache if isinstance(found_cache, Path) else None


def cache_value(build: Path, var: str) -> Optional[str]:
    """var's value in the build folder's CMakeCache.txt; None without that line or without the file."""
    try:
        text = (Path(build) / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    m = re.search(rf"^{re.escape(var)}(?::[A-Z]+)?=(.*)$", text, re.MULTILINE)
    return m.group(1).strip() if m else None


def cmake_args(build: Path, compilers: Sequence, env: Optional[Mapping[str, str]] = None, *,
               native_link: bool = True, ninja: bool = True, fresh: Optional[bool] = None, log=None) -> List[str]:
    """The CMake arguments that make the build in `build` quicker on this PC (all -D settings; [] when there is
    nothing to change). compilers: the build's compiler drivers (C, C++). native_link: the build links programs for
    this PC (so the linker may be chosen; False for a cross build). ninja: the Ninja generator (the link pool needs
    it). fresh: the folder will be configured from nothing (default: it has no CMakeCache.txt)."""
    build = Path(build)
    env = _environment(env)
    fresh = not (build / "CMakeCache.txt").is_file() if fresh is None else fresh
    args: List[str] = []
    said = []

    # The linker (Linux builds for Linux only): our -fuse-ld flag, or taken out again when it no longer links; never
    # someone else's flags.
    if native_link and IS_LINUX:
        chosen = linker(compilers, env)
        flag = f"-fuse-ld={chosen}" if chosen else ""
        for var in LINKER_FLAG_VARS:
            old = "" if fresh else (cache_value(build, var) or "")
            if old and not OUR_LINKER_FLAG.match(old):
                continue
            if flag or old:
                args.append(f"-D{var}={flag}")
        said.append(f"linker {chosen or 'the default'}")

    # The compiler cache: on a new folder, kept where it is in use, taken out where it is gone or turned off.
    cache = compiler_cache(compilers, env)
    cache_text = cache.as_posix() if cache else ""
    in_use = ""
    for var in LAUNCHER_VARS:
        old = "" if fresh else (cache_value(build, var) or "")
        if not old:
            if cache and fresh:
                args.append(f"-D{var}={cache_text}")
                in_use = cache_text
        elif old != cache_text and (_choice(env, CACHE_ENV) in OFF_WORDS or not Path(old).exists()):
            args.append(f"-D{var}=")
        else:
            in_use = old
    said.append(f"compiler cache {Path(in_use).stem if in_use else 'none'}")

    if ninja:
        links = link_jobs()
        args += [f"-DCMAKE_JOB_POOLS={LINK_POOL}={links}", f"-DCMAKE_JOB_POOL_LINK={LINK_POOL}"]
        said.append(f"up to {links} links at a time")

    text = "Build speed: " + ", ".join(said) + "."
    if log is not None and text != _last_said[0]:
        _last_said[0] = text
        log.info(text)
    return args


def without_speed_args(command: Sequence) -> List[str]:
    """command without what cmake_args() added: two configure commands compared without it are the same build."""
    return [str(a) for a in command if not SPEED_ARG.match(str(a))]


__all__ = ["available_ram_gb", "jobs", "link_jobs", "linker", "compiler_cache", "cache_value", "cmake_args",
           "without_speed_args", "MEMORY_PER_JOB_GB", "MEMORY_PER_LINK_GB"]

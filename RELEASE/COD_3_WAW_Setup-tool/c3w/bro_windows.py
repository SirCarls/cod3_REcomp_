"""Big Red One for Windows: bro.exe, built with llvm-mingw ([acct2-ocr] 2026-11-12, owner 23:06 "Also the windows
build needs to happen").

The build bro/README.md describes under "For Windows", done by the setup tool (c3w/bro.py calls it):

- On Windows the whole build runs there, with llvm-mingw's Windows toolchain: clang, lld, the mingw-w64 headers and C
  runtime and LLVM's libc++ in one pinned download into the work folder's tools/ (no Visual Studio, nothing installed
  into the system, no administrator rights). Git, CMake and Ninja are the ones on PATH, else the pinned ones Call of
  Duty 3's build downloads (deps.py).
- On Linux the Windows version can be built too (bro.BuildOptions.windows), with the Linux llvm-mingw Call of Duty 3's
  Windows version uses (deps.PORTABLE "llvm-mingw-linux").

Both are llvm-mingw 20260908 compiling for the same target, so bro.exe is the same program whichever system built it.
The program is linked statically: it needs only Windows' own DLLs and the Vulkan loader (vulkan-1.dll, which every
Vulkan driver installs). Its imports are checked after the build.

What it needs besides the port's sources, each at the commit Call of Duty 3's SDK pins (owner 2026-11-07 10:04: "Ok
just use what cod3 has"): taken from that SDK when it is in the same work folder at that commit, else fetched once
into tools/bro-windows-src/ (listed for approval first, like every download):
- SDL3 (the window, sound and pads), built here once as a static library with the same compiler;
- the Vulkan headers, and the Vulkan loader's vulkan-1.def (the import library is made from it with llvm-mingw's
  dlltool);
- Dear ImGui (the F2 window).
The shaders: glslc where there is one (Linux: the distribution's package); on Windows, Khronos' glslang (a pinned
download) through c3w/bro_glslc.py, which takes glslc's arguments and writes glslc's output.
"""
from __future__ import annotations

import re
import shutil
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from . import common, cross, deps
from .common import IS_WINDOWS, APPROVAL, Install, Log, Runner, SetupError

TRIPLE = cross.TOOLCHAIN_TRIPLE      # x86_64-w64-mingw32
LLVM_MINGW_LINUX = "llvm-mingw-linux"   # deps.PORTABLE's key (Call of Duty 3's Windows version uses the same)

# Pinned downloads only this build uses, in deps.PORTABLE's form: (url, folder with the programs, sha256). The
# hashes are the ones GitHub publishes with each release file (llvm-mingw's Linux file there matches deps.PORTABLE's).
DOWNLOADS: Dict[str, Tuple[str, str, str]] = {
    "bro-llvm-mingw-win": ("https://github.com/mstorsjo/llvm-mingw/releases/download/20260908/"
                           "llvm-mingw-20260908-ucrt-x86_64.zip", "llvm-mingw-20260908-ucrt-x86_64/bin",
                           "1bcf74d06b724aeecaa6412ca85f5b26fb1da770e7cdcefa9263c9c5c3ad34b6"),
    "bro-glslang-win": ("https://github.com/KhronosGroup/glslang/releases/download/16.6.0/"
                        "glslang-16.6.0-windows-x86_64-release.zip", "",
                        "82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b"),
}
DOWNLOAD_INFO: Dict[str, Tuple[str, str]] = {
    "bro-llvm-mingw-win": ("llvm-mingw 20260908 for Windows (compiles Big Red One: clang, lld, the mingw-w64 headers "
                           "and libraries)", "about 180 MB, 1 GB unpacked"),
    "bro-glslang-win": ("glslang 16.6.0, Khronos' shader compiler (turns the graphics shaders into SPIR-V)",
                        "about 13 MB"),
}

# The sources, at the commits Call of Duty 3's SDK pins (rexglue-sdk/.gitmodules; a git commit names its content, so
# a fetch checks itself).
SOURCES: Dict[str, Tuple[str, str]] = {
    "sdl3": ("https://github.com/libsdl-org/SDL.git", "8bf3b7215ad9fc3deb583c6a3a37c6c67f2e24e4"),
    "vulkan-headers": ("https://github.com/KhronosGroup/Vulkan-Headers.git", "e3b1eec08173d6b825cd3ac88c885a63b621504a"),
    "vulkan-loader": ("https://github.com/KhronosGroup/Vulkan-Loader.git", "5f157b62e333c63260d05d81bf66faa216ab0fb8"),
    "imgui": ("https://github.com/ocornut/imgui.git", "6d910d5487d11ca567b61c7824b0c78c569d62f0"),
}
SOURCE_INFO: Dict[str, Tuple[str, str]] = {
    "sdl3": ("SDL 3 (zlib licence): the window, sound and pads", "about 20 MB"),
    "vulkan-headers": ("the Vulkan headers (Apache-2.0)", "about 5 MB"),
    "vulkan-loader": ("the Vulkan loader's source (Apache-2.0), for its list of functions (vulkan-1.def)", "about 5 MB"),
    "imgui": ("Dear ImGui (MIT): the F2 window", "about 5 MB"),
}
# SDL3 as a static library, without its tests and examples
SDL3_OPTIONS = ["-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF",
                "-DSDL_EXAMPLES=OFF"]

EXE = "bro.exe"
LAUNCHER_BAT = "play.bat"

# The C flags of both builds ([acct2-ocr] 2026-11-13, found by the setup tool's own build end to end). Passing an
# integer where a pointer goes (-Wint-conversion: memcpy given XBOX_PTR, an integer, in xboxrecomp's
# gen/recomp_types.h and in bro/src/settings/bro_game_cvars.c) is a warning in GCC 13 but an error in clang 16 and
# later and in GCC 14 and later: llvm-mingw's clang 23 made no bro.exe, and Linux with GCC 14 (Fedora 40 and later,
# Arch) would make no bro. On a 64-bit host the integer holds the whole address, so this keeps it a warning, as GCC 13
# has it. The real fix is a cast, in bro/ and in xboxrecomp's generator (acct2-bro's; generated C is never edited by
# hand).
C_FLAGS = "-Wno-error=int-conversion"


def _prog(folder: Path, name: str) -> Path:
    """A program in a toolchain's folder: name.exe on Windows."""
    return Path(folder) / (name + (".exe" if IS_WINDOWS else ""))


def console_python() -> str:
    """The Python to hand CMake: on Windows the tool may run under pythonw.exe, which has no console output.
    [waw-decomp] 2026-11-16: through a link to its folder when that folder's path has characters a build tool chokes
    on (common.alias_path): setup.bat's Python lives in the project folder (work\\tools\\python), and this path goes
    into the build's files and into glslc.bat."""
    exe = Path(sys.executable)
    if IS_WINDOWS and exe.name.lower() == "pythonw.exe" and exe.with_name("python.exe").is_file():
        exe = exe.with_name("python.exe")
    if common.awkward_path(str(exe.parent)):
        exe = common.alias_path(exe.parent, "python-" + common.path_digest(exe.parent)) / exe.name
    return str(exe)


# ---------------------------------------------------------------------------
# Pinned downloads
# ---------------------------------------------------------------------------
def download_install(key: str, tools: Path) -> Install:
    url = DOWNLOADS[key][0]
    what, size = DOWNLOAD_INFO[key]
    return Install(key, what, str(Path(tools) / key), size, common.url_host(url))


def _verified(tools: Path, key: str) -> bool:
    try:
        return (Path(tools) / key / ".verified").read_text(encoding="ascii").strip().lower() == DOWNLOADS[key][2]
    except OSError:
        return False


def have_download(tools: Path, key: str) -> bool:
    sub = DOWNLOADS[key][1]
    folder = Path(tools) / key / sub if sub else Path(tools) / key
    return _verified(tools, key) and folder.is_dir()


def fetch(key: str, tools: Path, log: Log, cancel: threading.Event) -> Path:
    """One of DOWNLOADS: downloaded (after approval), checked against its SHA-256 and unpacked into tools/<key>;
    returns its program folder. Nothing in it is run to install."""
    url, sub, sha = DOWNLOADS[key]
    tools = Path(tools)
    dest = tools / key
    folder = dest / sub if sub else dest
    if have_download(tools, key):
        return folder
    archive = common.download(url, tools / "downloads" / url.rsplit("/", 1)[1], log, cancel, sha256=sha,
                              approval=download_install(key, tools))
    if dest.exists():
        common.safe_rmtree(dest, tools, [], log)
    common.extract_archive(archive, dest, log)
    if not folder.is_dir():
        raise SetupError(f"{archive.name} didn't contain {sub}/ as expected.")
    (dest / ".verified").write_text(sha, encoding="ascii")
    return folder


def find_glslang(folder: Path) -> Optional[Path]:
    """glslang's program in its unpacked release (glslang.exe, or glslangValidator.exe in older ones)."""
    for name in ("glslang.exe", "glslangValidator.exe", "glslang", "glslangValidator"):
        for p in sorted(Path(folder).rglob(name)):
            if p.is_file():
                return p
    return None


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
def sources_dir(tools: Path) -> Path:
    return Path(tools) / "bro-windows-src"


def sdk_thirdparty(tools: Path) -> Path:
    """Call of Duty 3's SDK in the same work folder (pipeline.py's sdk_src), whose submodules are these sources."""
    return Path(tools).parent / "rexglue-sdk" / "thirdparty"


def _at_commit(git: Path, folder: Path, commit: str, runner: Runner) -> bool:
    """`folder` is a clean checkout of `commit`."""
    if not (folder / ".git").exists():
        return False
    rc, head = runner.capture([git, "-C", folder, "rev-parse", "HEAD"])
    if rc != 0 or head.strip() != commit:
        return False
    # (--no-optional-locks: only read; Call of Duty 3's SDK checkout is left exactly as it is)
    rc, changed = runner.capture([git, "--no-optional-locks", "-C", folder, "status", "--porcelain",
                                  "--untracked-files=no"])
    return rc == 0 and not changed.strip()


def source_dir(name: str, tools: Path, git: Optional[Path], runner: Runner) -> Optional[Path]:
    """Where `name`'s source is at its pinned commit already: the SDK's submodule, else tools/bro-windows-src/;
    None when it has to be fetched."""
    if not git:
        return None
    commit = SOURCES[name][1]
    for folder in (sdk_thirdparty(tools) / name, sources_dir(tools) / name):
        if _at_commit(Path(git), folder, commit, runner):
            return folder
    return None


def source_install(name: str, tools: Path) -> Install:
    url, commit = SOURCES[name]
    what, size = SOURCE_INFO[name]
    return Install(f"bro-src-{name}", f"{what}, its source at commit {commit[:12]}", str(sources_dir(tools) / name),
                   size, common.url_host(url))


def ensure_source(name: str, tools: Path, git: Path, runner: Runner, log: Log) -> Path:
    found = source_dir(name, tools, git, runner)
    if found:
        return found
    url, commit = SOURCES[name]
    APPROVAL.require([source_install(name, tools)], log)
    dest = sources_dir(tools) / name
    if dest.exists():
        common.safe_rmtree(dest, tools, [], log)
    dest.mkdir(parents=True)
    no_crlf = ["-c", "core.autocrlf=false"]
    runner.run([git, "init", "-q", dest])
    runner.run([git, *no_crlf, "-C", dest, "fetch", "--depth", "1", url, commit])
    runner.run([git, *no_crlf, "-C", dest, "checkout", "-q", "--force", "--detach", commit])
    if not _at_commit(Path(git), dest, commit, runner):
        raise SetupError(f"{name} in {dest} isn't at commit {commit[:12]} after the download.")
    return dest


# ---------------------------------------------------------------------------
# The toolchain
# ---------------------------------------------------------------------------
def toolchain_key() -> str:
    """What the compiler is, for the stamps of what was built with it."""
    return DOWNLOADS["bro-llvm-mingw-win"][2] if IS_WINDOWS else deps.PORTABLE[LLVM_MINGW_LINUX][2]


def toolchain_install(tools: Path) -> Optional[Install]:
    """The approval entry for the compiler, or None when it is there already."""
    if IS_WINDOWS:
        return None if have_download(tools, "bro-llvm-mingw-win") else download_install("bro-llvm-mingw-win", tools)
    if deps._portable_ok(Path(tools), LLVM_MINGW_LINUX) and deps._portable_bin(Path(tools), LLVM_MINGW_LINUX).is_dir():
        return None
    return deps.tool_install(LLVM_MINGW_LINUX, Path(tools))


def fetch_toolchain(tools: Path, runner: Runner, log: Log, cancel: threading.Event) -> Path:
    """llvm-mingw's bin folder for this system."""
    if IS_WINDOWS:
        return fetch("bro-llvm-mingw-win", tools, log, cancel)
    return deps.fetch_portable(LLVM_MINGW_LINUX, Path(tools), runner, log, cancel)


def toolchain_file_text(toolchain_bin: Path, extra_roots: Sequence[Path] = ()) -> str:
    """A CMake toolchain file for Windows x86-64 with llvm-mingw (the same on Linux and on Windows)."""
    tc = Path(toolchain_bin)
    # [waw-decomp] 2026-11-16: quoted for CMake (common.cmake_quote), so a "$" in a folder's name stays itself
    q = common.cmake_quote
    roots = " ".join(q(p.as_posix()) for p in [cross.sysroot(tc), *map(Path, extra_roots)])
    return (f"# Written by the COD_3_WAW setup tool for Big Red One's Windows version (llvm-mingw).\n"
            f"set(CMAKE_SYSTEM_NAME Windows)\n"
            f"set(CMAKE_SYSTEM_PROCESSOR x86_64)\n"
            f'set(CMAKE_C_COMPILER {q(_prog(tc, TRIPLE + "-clang").as_posix())})\n'
            f'set(CMAKE_CXX_COMPILER {q(_prog(tc, TRIPLE + "-clang++").as_posix())})\n'
            f'set(CMAKE_RC_COMPILER {q(_prog(tc, TRIPLE + "-windres").as_posix())})\n'
            f"set(CMAKE_FIND_ROOT_PATH {roots})\n"
            "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n"
            "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n"
            "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n"
            "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n")


def generator_args(ninja: Optional[Path]) -> List[str]:
    if ninja:
        return ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}"]
    if IS_WINDOWS:
        raise SetupError("Big Red One's Windows build needs Ninja.")
    return []   # CMake's own default on Linux (Unix Makefiles)


# ---------------------------------------------------------------------------
# glslc
# ---------------------------------------------------------------------------
def glslc_wrapper_text(python: str, glslang: Path) -> str:
    """glslc.bat: bro_glslc.py with glslang, where the build calls glslc.
    [waw-decomp] 2026-11-16 (rc2-paths): cmd expands %NAME% even inside quotes, so every % in a path is doubled; the
    script is the copy next to glslc.bat (%~dp0: the folder cmd started it from), so the setup tool's own folder,
    whatever its name, isn't in the file; Python and glslang come through plain links (console_python, the work
    folder's link)."""
    def bat(text: object) -> str:
        return str(text).replace("%", "%%")
    return f'@"{bat(python)}" -I "%~dp0{GLSLC_SCRIPT}" --glslang "{bat(glslang)}" %*\r\n'


GLSLC_SCRIPT = "bro_glslc.py"   # [waw-decomp] 2026-11-16 copied next to glslc.bat (glslc_wrapper_text)


def bat_bytes(text: str) -> bytes:
    """[waw-decomp] 2026-11-16: a .bat file's bytes: cmd reads it in the console's (OEM) code page, as pipeline.py
    writes Launcher.bat; UTF-8 where that code page can't spell it (or off Windows, which has no "oem" codec)."""
    try:
        return text.encode("oem")
    except (UnicodeEncodeError, LookupError):
        return text.encode("utf-8")


def ensure_glslc(tools: Path, found: Optional[Path], log: Log, cancel: threading.Event) -> Path:
    """The glslc the build uses: the one found, else (Windows) glslang through bro_glslc.py."""
    if found:
        return Path(found)
    if not IS_WINDOWS:
        raise SetupError("glslc wasn't found (it turns the graphics shaders into SPIR-V): install your "
                         "distribution's glslc or shaderc package and build again.")
    folder = fetch("bro-glslang-win", tools, log, cancel)
    glslang = find_glslang(folder)
    if glslang is None:
        raise SetupError(f"glslang.exe wasn't found in {folder}.")
    wrapper = Path(tools) / "bro-windows" / "glslc.bat"
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    # [rexglue-native-platform] 2026-11-16: the bytes as they are (write_text's newline= needs Python 3.10, and the
    # tool supports 3.9: a TypeError there)
    # [waw-decomp] 2026-11-16: the script beside the .bat, the .bat in cmd's code page (glslc_wrapper_text), still
    # written as bytes
    shutil.copyfile(Path(__file__).resolve().with_name(GLSLC_SCRIPT), wrapper.with_name(GLSLC_SCRIPT))
    wrapper.write_bytes(bat_bytes(glslc_wrapper_text(console_python(), glslang)))
    return wrapper


# ---------------------------------------------------------------------------
# SDL3 and the Vulkan import library
# ---------------------------------------------------------------------------
@dataclass
class Libraries:
    sdl3_include: Path
    sdl3_library: Path
    vulkan_include: Path
    vulkan_library: Path
    imgui: Path


def sdl3_command(cmake: Path, src: Path, build: Path, install: Path, toolchain: Path,
                 ninja: Optional[Path]) -> List[str]:
    return [str(cmake), "-S", str(src), "-B", str(build), *generator_args(ninja),
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_INSTALL_PREFIX={install}", *SDL3_OPTIONS]


def ensure_sdl3(src: Path, tools: Path, cmake: Path, ninja: Optional[Path], toolchain: Path, jobs: int,
                runner: Runner, log: Log) -> Tuple[Path, Path]:
    """SDL3 built once for Windows as a static library into tools/bro-windows/sdl3 (built again when its commit or
    the compiler changes); returns (its include folder, libSDL3.a)."""
    base = Path(tools) / "bro-windows"
    build, install = base / "sdl3-build", base / "sdl3"
    lib = install / "lib" / "libSDL3.a"
    key = {"commit": SOURCES["sdl3"][1], "toolchain": toolchain_key(), "options": SDL3_OPTIONS}
    stamp = base / "sdl3.stamp.json"
    if common.read_json(stamp) == key and lib.is_file() and (install / "include" / "SDL3" / "SDL.h").is_file():
        return install / "include", lib
    for d in (build, install):
        if d.exists():
            common.safe_rmtree(d, tools, [], log)
    stamp.unlink(missing_ok=True)
    log.info("Building SDL3 for Windows (once; it is kept in the work folder's tools/)...")
    runner.run(sdl3_command(cmake, src, build, install, toolchain, ninja))
    runner.run([cmake, "--build", build, "--target", "install", "-j", str(max(1, jobs))])
    if not lib.is_file():
        raise SetupError(f"SDL3's build finished without {lib}.")
    common.write_json(stamp, key)
    return install / "include", lib


def vulkan_import_library(loader_src: Path, toolchain_bin: Path, out: Path, runner: Runner) -> Path:
    """libvulkan-1.a, the import library for vulkan-1.dll, made with llvm-mingw's dlltool from the loader's own list
    of functions. [waw-decomp] 2026-11-16: one that is the same already stays as it is, dated as it was, so a kept
    build folder doesn't link bro.exe again for nothing."""
    deffile = Path(loader_src) / "loader" / "vulkan-1.def"
    if not deffile.is_file():
        raise SetupError(f"The Vulkan loader's vulkan-1.def isn't in {loader_src}.")
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    lib = out / "libvulkan-1.a"
    new = out / "libvulkan-1.a.new"
    runner.run([_prog(toolchain_bin, TRIPLE + "-dlltool"), "-d", deffile, "-D", "vulkan-1.dll", "-l", new])
    if lib.is_file() and lib.read_bytes() == new.read_bytes():
        new.unlink()
    else:
        new.replace(lib)
    return lib


def _write_if_changed(path: Path, text: str) -> None:
    """[waw-decomp] 2026-11-16 A file written only when its text changes (CMake reconfigures when a toolchain file's
    date moves)."""
    path = Path(path)
    if not (path.is_file() and path.read_text(encoding="utf-8") == text):
        path.write_text(text, encoding="utf-8")


# [waw-decomp] 2026-11-16 Keeping a build folder between builds (cleanup off): the configure command it was made with.
CONFIGURE_STAMP = "setup-tool-configure.json"


def configure_folder(cmd: Sequence, folder: Path, keep: bool, lay, runner: Runner, log: Log) -> None:
    """[waw-decomp] 2026-11-16 CMake's configure of a build folder, for this Windows build and bro.py's Linux one.
    keep (cleanup off): the folder of the last build is used again when it was configured with the same command, so
    the build compiles only what changed (CMake notices new or removed C files itself: bro/CMakeLists.txt's
    CONFIGURE_DEPENDS); else it starts new, as each build did before. lay: bro.py's Layout (the work folder)."""
    folder = Path(folder)
    cmd = [str(c) for c in cmd]
    stamp = folder / CONFIGURE_STAMP
    last = common.read_json(stamp)
    if folder.exists() and not (keep and isinstance(last, dict) and last.get("command") == cmd):
        common.safe_rmtree(folder, Path(lay.work), [lay.out], log)
    runner.run(cmd)
    common.write_json(stamp, {"command": cmd})


# ---------------------------------------------------------------------------
# The game
# ---------------------------------------------------------------------------
def configure_command(cmake: Path, ninja: Optional[Path], bro_src: Path, build: Path, toolchain: Path,
                      xboxrecomp: Path, gen: Path, libs: Libraries, glslc: Path, python: str) -> List[str]:
    vk = Path(libs.vulkan_include).as_posix()
    glslc_s = Path(glslc).as_posix()
    return [str(cmake), "-S", str(bro_src), "-B", str(build), *generator_args(ninja),
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
            f"-DXBOXRECOMP_DIR={xboxrecomp}", f"-DBRO_GEN_DIR={gen}", f"-DPython3_EXECUTABLE={python}",
            f"-DBRO_SDL3_INCLUDE_DIR={Path(libs.sdl3_include).as_posix()}",
            f"-DBRO_SDL3_LIBRARY={Path(libs.sdl3_library).as_posix()}",
            f"-DVulkan_INCLUDE_DIR={vk}", f"-DNGL_VK_VULKAN_INCLUDE_DIR={vk}",
            f"-DVulkan_LIBRARY={Path(libs.vulkan_library).as_posix()}",
            # glslc for ngl_vk's shaders (bro/ and ngl_vk/ look it up by name; given here, they take this one)
            f"-DVulkan_GLSLC_EXECUTABLE={glslc_s}", f"-DBRO_GLSLC={glslc_s}", f"-DNGL_VK_GLSLC={glslc_s}",
            f"-DBRO_IMGUI_DIR={Path(libs.imgui).as_posix()}", f"-DCMAKE_C_FLAGS={C_FLAGS}",
            # one program: libc++, libunwind and the C runtime's own parts inside bro.exe
            "-DCMAKE_EXE_LINKER_FLAGS=-static"]


def imports(exe: Path, toolchain_bin: Path, runner: Runner) -> List[str]:
    """The DLLs a Windows program imports (llvm-objdump -p)."""
    rc, out = runner.capture([_prog(toolchain_bin, "llvm-objdump"), "-p", exe])
    if rc != 0:
        raise SetupError(f"Couldn't read {Path(exe).name}'s imports: {out.strip()[:300]}")
    return re.findall(r"DLL Name:\s*(\S+)", out)


def check_imports(exe: Path, toolchain_bin: Path, runner: Runner) -> List[str]:
    """Every DLL bro.exe imports is Windows' own or the Vulkan loader; returns them."""
    names = imports(exe, toolchain_bin, runner)
    others = sorted({n for n in names if not cross.is_windows_system_dll(n)})
    if others:
        raise SetupError(f"{Path(exe).name} needs DLLs that are neither part of Windows nor the Vulkan loader: "
                         + ", ".join(others))
    return names


def cache_problems(cache_text: str) -> List[str]:
    out = []
    if "BRO_GLSLC:FILEPATH=BRO_GLSLC-NOTFOUND" in cache_text or "NGL_VK_GLSLC-NOTFOUND" in cache_text:
        out.append("glslc")
    if "Vulkan_INCLUDE_DIR:PATH=Vulkan_INCLUDE_DIR-NOTFOUND" in cache_text:
        out.append("the Vulkan headers")
    return out


def build(lay, host_git: Path, cmake: Path, ninja: Optional[Path], glslc_found: Optional[Path], jobs: int,
          runner: Runner, log: Log, cancel: threading.Event,
          line_filter: Optional[Callable[[str], None]] = None, keep: bool = False,
          target: Optional[str] = None) -> Optional[Path]:
    """bro.exe from the recompiled C in lay.gen (bro.py's Layout); returns it. [waw-decomp] 2026-11-16: keep: the
    build folder of the last build is used again (configure_folder); target: only that CMake target (the libraries,
    while the recompiler still writes the C): returns None."""
    tools = Path(lay.tools)
    toolchain_bin = fetch_toolchain(tools, runner, log, cancel)
    glslc = ensure_glslc(tools, glslc_found, log, cancel)
    src = {name: ensure_source(name, tools, host_git, runner, log) for name in SOURCES}
    base = tools / "bro-windows"
    base.mkdir(parents=True, exist_ok=True)
    sdl3_install = base / "sdl3"
    toolchain = base / "toolchain.cmake"
    _write_if_changed(toolchain, toolchain_file_text(toolchain_bin, [sdl3_install]))   # [waw-decomp] 2026-11-16
    inc, lib = ensure_sdl3(src["sdl3"], tools, cmake, ninja, toolchain, jobs, runner, log)
    out = Path(lay.work) / "build-windows"
    vulkan_lib = vulkan_import_library(src["vulkan-loader"], toolchain_bin, Path(lay.work) / "windows-libs", runner)
    libs = Libraries(inc, lib, src["vulkan-headers"] / "include", vulkan_lib, src["imgui"])
    log.info("Building the Windows version (bro.exe) with llvm-mingw" + (f" ({target})..." if target else "..."))
    configure_folder(configure_command(cmake, ninja, lay.bro_src, out, toolchain, lay.xboxrecomp, lay.gen, libs,
                                       glslc, console_python()), out, keep, lay, runner, log)
    cache = out / "CMakeCache.txt"
    missing = cache_problems(cache.read_text(encoding="utf-8", errors="replace") if cache.is_file() else "")
    if missing:
        raise SetupError("The Windows build found no " + " and no ".join(missing) + ", so the game couldn't draw "
                         "anything.")
    runner.run([cmake, "--build", out, "-j", str(max(1, jobs))] + (["--target", target] if target else []),
               line_filter=line_filter)
    if target:
        return None
    exe = out / EXE
    if not exe.is_file():
        raise SetupError(f"The Windows build finished without {EXE} in {out}: see the log above.")
    names = check_imports(exe, toolchain_bin, runner)
    log.info(f"{EXE} imports: " + ", ".join(names))
    return exe


# ---------------------------------------------------------------------------
# What the build will download
# ---------------------------------------------------------------------------
def downloads(tools: Path, git: Optional[Path], glslc_found: Optional[Path], runner: Runner) -> List[Install]:
    """What the Windows version needs to download, for bro.py's one approval list."""
    items = []
    tc = toolchain_install(tools)
    if tc:
        items.append(tc)
    if IS_WINDOWS and not glslc_found and not have_download(tools, "bro-glslang-win"):
        items.append(download_install("bro-glslang-win", tools))
    items += [source_install(name, tools) for name in SOURCES if source_dir(name, tools, git, runner) is None]
    return items


# ---------------------------------------------------------------------------
# Installing
# ---------------------------------------------------------------------------
LAUNCHER_BAT_TEXT = "\r\n".join([
    "@echo off",
    "rem Call of Duty 2: Big Red One, built by the COD3 setup tool from your own disc. Starts the game with its files",
    "rem in game\\ and its saves in saves\\, drawn with Vulkan. The setup tool's Play page has the same start with",
    "rem options.",
    "setlocal",
    'set "here=%~dp0"',
    'if not defined BRO_VK set "BRO_VK=1"',
    'if not exist "%here%saves" mkdir "%here%saves"',
    'if not exist "%here%logs" mkdir "%here%logs"',
    '"%here%bin\\bro.exe" "%here%game" "%here%saves" %*',
    ""])


@dataclass
class HostTools:
    """Windows: what the build runs (bro.py's Tools has the Linux side)."""
    git: Optional[Path] = None
    cmake: Optional[Path] = None
    ninja: Optional[Path] = None
    glslc: Optional[Path] = None


MIN_CMAKE = (3, 20)


def _cmake_ok(runner: Runner, cmake: Optional[str]) -> bool:
    if not cmake:
        return False
    m = re.search(r"(\d+)\.(\d+)", runner.capture([cmake, "--version"])[1])
    return bool(m) and (int(m.group(1)), int(m.group(2))) >= MIN_CMAKE


def find_host_tools(tools: Path, runner: Runner) -> HostTools:
    """Windows: Git, CMake, Ninja and glslc on PATH, else the pinned ones in tools/ when they are there."""
    which = shutil.which
    t = HostTools()
    t.git = Path(which("git")) if which("git") else None
    if t.git is None and deps._portable_ok(Path(tools), "git-win"):
        t.git = deps._portable_bin(Path(tools), "git-win") / "git.exe"
    cmake = which("cmake")
    if _cmake_ok(runner, cmake):
        t.cmake = Path(cmake)
    elif deps._portable_ok(Path(tools), "cmake-win"):
        t.cmake = deps._portable_bin(Path(tools), "cmake-win") / "cmake.exe"
    t.ninja = Path(which("ninja")) if which("ninja") else None
    if t.ninja is None and deps._portable_ok(Path(tools), "ninja-win"):
        t.ninja = deps._portable_bin(Path(tools), "ninja-win") / "ninja.exe"
    t.glslc = Path(which("glslc")) if which("glslc") else None
    return t


def host_downloads(t: HostTools, tools: Path) -> List[Install]:
    """Windows: the pinned Git, CMake and Ninja the build would fetch."""
    out = []
    for have, key in ((t.git, "git-win"), (t.cmake, "cmake-win"), (t.ninja, "ninja-win")):
        if not have:
            out.append(deps.tool_install(key, Path(tools)))
    return out


def ensure_host_tools(t: HostTools, tools: Path, allow_install: bool, runner: Runner, log: Log,
                      cancel: threading.Event) -> HostTools:
    """Windows: fetches what find_host_tools didn't find (after approval), or says what is missing."""
    missing = [name for name, have in (("Git", t.git), ("CMake", t.cmake), ("Ninja", t.ninja)) if not have]
    if missing and not allow_install:
        raise SetupError("Big Red One's build needs: " + ", ".join(missing) + ". Turn on installing what's missing "
                         "(Set up): they are downloaded into the work folder's tools/, nothing is installed.")
    if not t.git:
        t.git = deps.fetch_windows_fallback("git-win", Path(tools), runner, log, cancel) / "git.exe"
    if not t.cmake:
        t.cmake = deps.fetch_portable("cmake-win", Path(tools), runner, log, cancel) / "cmake.exe"
    if not t.ninja:
        t.ninja = deps.fetch_portable("ninja-win", Path(tools), runner, log, cancel) / "ninja.exe"
    return t


__all__ = ["DOWNLOADS", "SOURCES", "EXE", "LAUNCHER_BAT", "LAUNCHER_BAT_TEXT", "HostTools", "Libraries", "build",
           "configure_command", "downloads", "ensure_glslc", "fetch_toolchain", "find_host_tools",
           "ensure_host_tools", "host_downloads", "toolchain_file_text", "check_imports"]

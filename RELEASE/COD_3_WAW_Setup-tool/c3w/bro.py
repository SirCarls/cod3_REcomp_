"""Call of Duty 2: Big Red One (the original Xbox game) built for PC from your own disc: the setup tool's side of
the port in bro/ ([acct2-bro], owner 2026-11-06 07:31: "add a tab within the setup tool for that. Follow similar
steps to that of cod3").

The Big Red One tab (games.py) builds it in the same steps as Call of Duty 3's Set up page:

1. Check the disc: an Xbox disc image (.iso: redump or trimmed XISO) or a folder copied from the disc, whose
   default.xbe is one the port builds from (KNOWN_XBE: the Collector's Edition the port was made from, the World,
   German and French/Spanish/Italian discs; the port's tables name addresses in that program).
2. Copy the disc's files into <output>/bro/game: the game reads them from there. Only default.xbe is needed before
   the next steps, so the rest is copied beside them, then checked against the disc's list of MD5s (DISC_LISTS)
   ([acct2-ocr] 2026-11-12).
3. Fetch xboxrecomp (MIT; the recompiler for original Xbox programs) at the commit the port was made with into
   the work folder's tools/, and a Python with Capstone (the disassembler xboxrecomp uses). Both are listed for
   approval first, like every download.
4. Recompile default.xbe to C with xboxrecomp's own steps: its parser, the disassembly (seeded with
   bro/config/seed_functions.json), function IDs, ABI analysis, then the lift, with bro/config/
   stand_in_functions.tsv as the manual functions. About 7 minutes on a fast PC (a Ryzen 9 5900X), 3.6 of them the
   disassembly and 3.3 the lift, and up to 3.6 GB of memory; it was 19 minutes before
   bro/patches/xboxrecomp-speed.patch ([acct2-ocr] 2026-11-12, owner 18:07 "work on making the decompilation
   process for BRO quicker": decode caches and fewer garbage collections, the same C byte for byte on all four discs)
   and 105 before bro/patches/xboxrecomp-lazy-decode.patch ([acct2-bro] 2026-11-08, owner 09:43 "Is there a way to
   speed up the setup tool for the big red one?": the same C, byte for byte). The C is the game's code, so it stays
   in the work folder and is deleted with it.
   [waw-decomp] 2026-11-16 (owner 13:58: "Work on optimizing the recompilation process ... Make sure the
   optimizations aren't disc specific"; 14:3x on keeping the output: "Keep it unless cleanup is on"): each of the
   five steps is done again only when something it reads changed (a key per step, LiftInputs), so a stand-in's new
   name redoes nothing and a new stand-in redoes only the last step. With cleanup off the tables and the C stay in
   the work folder between builds (<work>/bro/lift-cache: one entry per disc, by its default.xbe's MD5), so a second
   build of a disc skips the recompile, and the C keeps its file dates where it didn't change, so only changed files
   are compiled again.
   [waw-decomp/speed1] 2026-11-23 (owner 21:00: "keep the BRO and FH lift cache even with cleanup
   on"): the cache is kept with cleanup on too, compressed, in <work>/tools/lift-cache/bro (LiftCache);
   --clear-lift-cache empties it on purpose.
5. Build bro/ with CMake against that C: graphics on ../ngl_vk; the window, sound and pads through SDL3, the static
   SDL3 Call of Duty 3's SDK builds when that SDK is in the same work folder (both games then use one SDL), else the
   system's own SDL3 (owner 2026-11-07 10:04: "Ok just use what cod3 has").
   [waw-decomp] 2026-11-16: while the recompiler runs (one processor core), what doesn't need its C is built beside
   it (the Windows version's SDL3, both builds' libraries); then the Linux and Windows versions are built side by
   side, sharing the jobs.
6. Install bin/bro, saves/, a launcher (play.sh) and bro.json in <output>/bro.

Linux and Windows ([acct2-ocr] 2026-11-12, owner 23:06 "Also the windows build needs to happen"): on Windows step 5
builds bin/bro.exe with llvm-mingw, and the launcher is play.bat (c3w/bro_windows.py); on Linux BuildOptions.windows
builds that too, beside the Linux program. Plain logic, no Tk, so the tests run without Tkinter. The disc and
default.xbe are only read; what runs is the port's own C.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tarfile   # [waw-decomp/speed1] the lift cache's archives
import tempfile
import threading
import time
import zlib   # [waw-decomp/speed1] (zlib.error: a damaged archive)
from dataclasses import asdict, dataclass, field
from pathlib import Path, PurePosixPath
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from . import bro_windows, common, controller_db, deps
from .common import IS_LINUX, IS_WINDOWS, APPROVAL, Cancelled, Install, Log, Runner, SetupError, human, is_within
from .xiso import XisoImage
from . import build_speed   # [waw-decomp/speed1] 2026-11-23 jobs, linker, compiler cache and link pool for this PC

GAME_NAME = "Call of Duty 2: Big Red One"
OUTPUT_SUBDIR = "bro"          # <output>/bro: the game, its saves and its launcher
WORK_SUBDIR = "bro"            # <work>/bro: the recompiled C and the build (deleted at the end, like work/)
MANIFEST = "bro.json"

# xboxrecomp (https://github.com/sp00nznet/xboxrecomp, MIT) at the release the port was made and tested with. A
# git commit names its content, so the fetch checks itself.
XBOXRECOMP_URL = "https://github.com/sp00nznet/xboxrecomp.git"
XBOXRECOMP_VERSION = "0.16.0"
XBOXRECOMP_COMMIT = "f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b"
# Capstone, the disassembler xboxrecomp's steps import, from PyPI: a pinned version, wheels only (nothing is
# built or run to install it).
CAPSTONE = "capstone==5.0.9"

# The discs the port builds from, by the MD5 of their default.xbe ([acct2-ocr] 2026-11-12, the owner 18:07: "we need
# to make sure the setup tool has the MD5s of the discs that can be used"). Each disc's own default.xbe is
# recompiled, so the C is that disc's program; the port's tables (bro/config: stand-ins, seeds) name addresses, and
# none of them is in code that differs between the four. The World and German discs' code is the Collector's
# Edition's byte for byte (only 37 bytes of .rdata differ); the French, Spanish and Italian disc has 12 KB of other
# code in one level's section (LIBEATOR), none of it at an address the port names. All four recompile, build and
# play: on 2026-11-12 each disc's own build ran the first level for 75 s at 60 fps, its log's warnings the same as
# the Collector's Edition's, and the French, Spanish and Italian disc's also the level with the other code (liberator).
KNOWN_XBE: Dict[str, str] = {
    "3b50372b121dc4e51b2b3dbda7cba19d": "Call of Duty 2: Big Red One, Collector's Edition (World), the disc the port "
                                        "was made from",
    "96b46e06c8181e31800c97aa0c73a62f": "Call of Duty 2: Big Red One (World)",
    "1eb034b61c1462328f95b2b65205df21": "Call of Duty 2: Big Red One (Europe: French, Spanish, Italian)",
    "43293b5fb91f7f3595a87af2a652474f": "Call of Duty 2: Big Red One (Germany)",
}
# Each of those discs' files: c3w/disc_md5/bro_<name>.tsv (md5, size, path; disc_files.load_list's format), made from
# the discs read in place with c3w/xiso.py: names, sizes and checksums only. The copy is checked against it.
DISC_LISTS: Dict[str, str] = {
    "3b50372b121dc4e51b2b3dbda7cba19d": "bro_world_ce",
    "96b46e06c8181e31800c97aa0c73a62f": "bro_world",
    "1eb034b61c1462328f95b2b65205df21": "bro_europe_fr_es_it",
    "43293b5fb91f7f3595a87af2a652474f": "bro_germany",
}
DISC_MD5_DIR = Path(__file__).resolve().parent / "disc_md5"

# The levels a start can go straight into (the game's own map names, as its chapter list uses them; the
# BRO_DEVMAP setting of bro/src/input/bro_pad.c). The first entry is the game's own start: the menus.
LEVELS: Tuple[Tuple[str, str], ...] = (
    ("", "Main menu"), ("prologue", "Prologue"), ("oran", "Oran"), ("Tunisia", "Tunisia"),
    ("kasserine_pass", "Kasserine Pass"), ("kasserine", "Kasserine"), ("liberator", "Liberator"),
    ("gela", "Gela"), ("piano_lupo", "Piano Lupo"), ("troina", "Troina"), ("omaha_beach", "Omaha Beach"),
    ("mons1", "Mons"), ("eilendorf", "Eilendorf"), ("buchholz", "Buchholz"), ("dragons_teeth", "Dragon's Teeth"),
)
WINDOW_SIZES = ("", "1280x960", "1600x1200", "1920x1080", "2560x1440", "3840x2160")
MOUSE_SPEED_RANGE = (100, 32767)
DEFAULT_MOUSE_SPEED = 1600

# What the build needs on this PC, and the packages that bring it, per package manager (deps.py's way: one
# script run once as root; a name a distribution doesn't have is skipped).
MIN_CMAKE = (3, 20)
PKGCONFIG = {"sdl2": "SDL2", "vulkan": "the Vulkan loader", "libcrypto": "OpenSSL"}
LINUX_PACKAGES: Dict[str, List[str]] = {
    "apt": ["git", "cmake", "build-essential", "pkg-config", "libsdl2-dev", "libsdl3-dev", "libvulkan-dev", "glslc",
            "libssl-dev", "python3-venv"],
    "dnf": ["git", "cmake", "gcc", "make", "pkgconf-pkg-config", "SDL2-devel", "sdl2-compat-devel", "SDL3-devel",
            "vulkan-loader-devel", "vulkan-headers", "glslc", "openssl-devel"],
    "pacman": ["git", "cmake", "base-devel", "pkgconf", "sdl2", "sdl3", "vulkan-icd-loader", "vulkan-headers",
               "shaderc", "openssl"],
    "zypper": ["git", "cmake", "gcc", "make", "pkg-config", "libSDL2-devel", "SDL3-devel", "vulkan-devel", "shaderc",
               "libopenssl-devel", "python3"],
    "xbps": ["git", "cmake", "base-devel", "pkg-config", "SDL2-devel", "SDL3-devel", "vulkan-loader-devel",
             "Vulkan-Headers", "shaderc", "openssl-devel", "python3"],
}
# (SDL2: xboxrecomp's input library still looks for it. SDL3: the window, sound and pads, when Call of Duty 3's SDK
# isn't in the work folder; distributions without an SDL3 package skip the name.)
# Room the build needs besides the game's own files: the recompiler's tables and the C (about 0.6 GB), the build
# (about 0.4 GB).
WORK_ROOM = 1_500_000_000
# [acct2-ocr] the Windows version besides: its build (about 0.4 GB), SDL3's, and llvm-mingw (about 1 GB unpacked)
WINDOWS_ROOM = 1_600_000_000
# [waw-decomp/speed1] 2026-11-23 a disc's entry in the lift cache besides (LiftCache: the tables and
# the C, about 0.3 GB, compressed; 400 MB uncompressed before)
LIFT_CACHE_ROOM = 100_000_000

# The recompiler's steps: (key, what the log says, share of the build's time).
STEPS: Tuple[Tuple[str, str, float], ...] = (
    ("xbe_parser", "Reading default.xbe", 0.01),
    ("disasm", "Disassembling (the long step: 3-8 minutes)", 0.50),
    ("func_id", "Naming functions", 0.02),
    ("abi_analysis", "Working out how each function is called", 0.01),
    ("recomp", "Turning the program into C", 0.46),
)


def supported() -> Tuple[bool, str]:
    """(True, "") where the build works, else (False, why): Linux, and 64-bit Windows ([acct2-ocr] 2026-11-12)."""
    if IS_LINUX:
        return True, ""
    if IS_WINDOWS:
        if sys.maxsize > 2 ** 32:
            return True, ""
        return False, "Big Red One's Windows build needs 64-bit Windows and a 64-bit Python."
    return False, "Big Red One builds on Windows and on Linux for now, not on this system."


# ---------------------------------------------------------------------------
# The disc
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class XbeInfo:
    title_id: int
    title: str
    version: int


def xbe_info(data: bytes) -> XbeInfo:
    """The title ID, name and version in an XBE's certificate (`data`: the file's start; the header and the
    certificate are in its first pages)."""
    if len(data) < 0x178 or data[:4] != b"XBEH":
        raise SetupError("default.xbe isn't an Xbox program (no XBEH header).")
    base, cert_va = struct.unpack_from("<I", data, 0x104)[0], struct.unpack_from("<I", data, 0x118)[0]
    off = cert_va - base
    if off < 0 or off + 0xB0 > len(data):
        raise SetupError("default.xbe's certificate isn't where its header says.")
    title_id = struct.unpack_from("<I", data, off + 0x08)[0]
    title = data[off + 0x0C:off + 0x0C + 80].decode("utf-16-le", errors="replace").split("\0", 1)[0].strip()
    version = struct.unpack_from("<I", data, off + 0xAC)[0]
    return XbeInfo(title_id, title, version)


@dataclass(frozen=True)
class Disc:
    kind: str           # "iso" or "folder"
    path: Path          # the image, or the folder holding default.xbe
    xbe_md5: str
    info: XbeInfo
    files: int
    size: int           # bytes of game files

    @property
    def known(self) -> str:
        return KNOWN_XBE.get(self.xbe_md5, "")


def _folder_xbe(folder: Path) -> Optional[Path]:
    """default.xbe in `folder`, whatever its case."""
    exact = folder / "default.xbe"
    if exact.is_file():
        return exact
    try:
        return next((p for p in folder.iterdir() if p.name.lower() == "default.xbe" and p.is_file()), None)
    except OSError:
        return None


def _folder_files(folder: Path) -> List[Path]:
    return [p for p in folder.rglob("*") if p.is_file()]


def open_disc(text: str) -> Disc:
    """The disc a path names: a disc image, a folder copied from the disc, or that folder's default.xbe.
    SetupError (a plain message) when it isn't one."""
    s = (text or "").strip().strip('"').strip("'")
    if not s:
        raise SetupError(f"Choose your {GAME_NAME} disc: its .iso, or a folder copied from it.")
    path = Path(os.path.expanduser(s))
    if not path.exists():
        raise SetupError(f"{path.name} isn't there.")
    if path.is_file() and path.name.lower() == "default.xbe":
        path = path.parent
    if path.is_dir():
        xbe = _folder_xbe(path)
        if xbe is None:
            raise SetupError(f"{path.name} has no default.xbe: pick the folder that holds the disc's files.")
        data = xbe.read_bytes()
        files = _folder_files(path)
        disc = Disc("folder", path, hashlib.md5(data).hexdigest(), xbe_info(data[:65536]), len(files),
                    sum(f.stat().st_size for f in files))
    else:
        try:
            with XisoImage(path) as img:
                if img.find("default.xbe") is None:
                    raise SetupError(f"{path.name} is a disc image without default.xbe: not an original Xbox game.")
                data = img.read("default.xbe")
                disc = Disc("iso", path, hashlib.md5(data).hexdigest(), xbe_info(data[:65536]), len(img.files),
                            img.total_size)
        except OSError as e:
            raise SetupError(f"Couldn't read {path.name}: {e.strerror or e}") from None
    return disc


def check_disc(text: str) -> Tuple[Optional[Disc], str]:
    """(the disc, a line for the Set up page) when it can be built; (None, why) when not."""
    try:
        disc = open_disc(text)
    except SetupError as e:
        return None, str(e)
    if not disc.known:
        if "big red one" not in disc.info.title.lower():
            return None, f"This is {disc.info.title or 'another game'}, not {GAME_NAME}."
        return None, (f"This {GAME_NAME} isn't the version the port was made from, nor another disc it builds from "
                      f"(its default.xbe's MD5 is {disc.xbe_md5}), so the port's tables may not fit it. Please "
                      "report it, with that MD5.")
    what = "disc image" if disc.kind == "iso" else "folder"
    return disc, f"Found: {disc.known} ({what}, {disc.files} files, {human(disc.size)})."


# ---------------------------------------------------------------------------
# Where things go
# ---------------------------------------------------------------------------
def project_root() -> Path:
    """The project folder: the one holding bro/, ngl_vk/ and tools/bro_decomp/ next to the setup tool."""
    return Path(__file__).resolve().parent.parent.parent


@dataclass(frozen=True)
class Layout:
    repo: Path          # the project folder (bro/, ngl_vk/, tools/bro_decomp/)
    out: Path           # <output>/bro
    work: Path          # <work>/bro
    tools: Path         # <work>/tools: kept between builds

    # In the output folder
    @property
    def game(self) -> Path:
        return self.out / "game"

    @property
    def saves(self) -> Path:
        return self.out / "saves"

    @property
    def logs(self) -> Path:
        return self.out / "logs"

    @property
    def exe(self) -> Path:
        return self.out / "bin" / common.exe_name("bro")

    @property
    def launcher(self) -> Path:
        return self.out / ("play.bat" if IS_WINDOWS else "play.sh")

    # [acct2-ocr] the Windows program and its launcher, whichever system built them
    @property
    def exe_windows(self) -> Path:
        return self.out / "bin" / bro_windows.EXE

    @property
    def launcher_windows(self) -> Path:
        return self.out / bro_windows.LAUNCHER_BAT

    @property
    def manifest(self) -> Path:
        return self.out / MANIFEST

    # In the work folder's tools/ (kept)
    @property
    def xboxrecomp_cache(self) -> Path:
        return self.tools / f"xboxrecomp-{XBOXRECOMP_VERSION}"

    @property
    def python_env(self) -> Path:
        return self.tools / "bro-python"

    @property
    def python(self) -> Path:
        return self.python_env / ("Scripts/python.exe" if IS_WINDOWS else "bin/python")

    # Call of Duty 3's SDK in the same work folder (pipeline.py's sdk_install), when it has been built there
    @property
    def sdk_sdl3(self) -> Path:
        preset = "win-amd64" if IS_WINDOWS else "linux-amd64"
        return self.tools.parent / "rexglue-sdk" / "out" / "install" / preset / "lib" / "cmake" / "SDL3"

    # In the work folder (deleted at the end)
    @property
    def xboxrecomp(self) -> Path:
        return self.work / "xboxrecomp"

    @property
    def xbe(self) -> Path:
        return self.work / "game_files" / "default.xbe"

    @property
    def gen(self) -> Path:
        return self.work / "gen"

    @property
    def recomp_out(self) -> Path:
        return self.work / "recomp_out"

    @property
    def build(self) -> Path:
        return self.work / "build"

    @property
    def stamp(self) -> Path:
        return self.work / "recompile.stamp.json"

    # [waw-decomp] 2026-11-16
    @property
    def analysis_json(self) -> Path:
        """What xboxrecomp's parser writes beside default.xbe; its disassembly finds it there."""
        return self.xbe.with_name("default_analysis.json")

    @property
    def source_stamp(self) -> Path:
        """Which xboxrecomp and patches the work copy (xboxrecomp/) was made from."""
        return self.work / "xboxrecomp.stamp.json"

    @property
    def lift_cache(self) -> Path:
        """The recompiler's output kept between builds (LiftCache; [waw-decomp/speed1] 2026-11-23: in tools/, which a
        cleanup keeps, one folder per game: bro, fh)."""
        return self.tools / "lift-cache" / self.work.name

    # In the project folder
    @property
    def bro_src(self) -> Path:
        return self.repo / "bro"

    @property
    def patches(self) -> List[Path]:
        return sorted((self.bro_src / "patches").glob("*.patch"), key=lambda p: p.name)

    @property
    def seeds(self) -> Path:
        return self.bro_src / "config" / "seed_functions.json"

    @property
    def stand_ins(self) -> Path:
        return self.bro_src / "config" / "stand_in_functions.tsv"


def layout(output: Path, work: Path, repo: Optional[Path] = None) -> Layout:
    return Layout(Path(repo or project_root()), Path(output) / OUTPUT_SUBDIR, Path(work) / WORK_SUBDIR,
                  Path(work) / "tools")


def check_project(lay: Layout) -> None:
    """The port's sources are next to the setup tool (a copy with only the tool has none)."""
    need = [lay.bro_src / "CMakeLists.txt", lay.stand_ins, lay.seeds, lay.repo / "ngl_vk" / "CMakeLists.txt",
            lay.repo / "tools" / "bro_decomp" / "ngl_stubs.py"]
    missing = [str(p.relative_to(lay.repo)) for p in need if not p.is_file()]
    if missing:
        raise SetupError("The Big Red One port's sources aren't in this copy of the project (missing: "
                         + ", ".join(missing) + "). Use the whole project folder.")


def installed(output: Path) -> Optional[Path]:
    """The built game in an output folder, or None."""
    if not output:
        return None
    out = Path(output) / OUTPUT_SUBDIR
    m = common.read_json(out / MANIFEST)
    rel = m.get("exe_windows" if IS_WINDOWS else "exe_linux") if isinstance(m, dict) else None
    exe = out / rel if isinstance(rel, str) and rel and ".." not in Path(rel).parts else None
    return exe if exe is not None and exe.is_file() else None


# ---------------------------------------------------------------------------
# What this PC needs
# ---------------------------------------------------------------------------
@dataclass
class Tools:
    git: Optional[Path] = None
    cmake: Optional[Path] = None
    ninja: Optional[Path] = None
    cc: Optional[Path] = None
    glslc: Optional[Path] = None
    pkgconfig: Optional[Path] = None
    missing_modules: List[str] = field(default_factory=list)   # pkg-config names
    venv: bool = True
    cmake_version: Tuple[int, ...] = ()
    sdl3: Optional[Path] = None     # Call of Duty 3's SDK's SDL3 CMake package, when it is built in the work folder
    system_sdl3: bool = False       # else the system's SDL3 (pkg-config sdl3)

    def problems(self) -> List[str]:
        out = []
        if not self.git:
            out.append("git")
        if not self.cmake or self.cmake_version < MIN_CMAKE:
            out.append(f"cmake >= {'.'.join(map(str, MIN_CMAKE))}")
        if not self.cc:
            out.append("a C compiler (gcc or clang)")
        if not self.glslc:
            out.append("glslc (turns the graphics shaders into SPIR-V)")
        if not self.pkgconfig:
            out.append("pkg-config")
        out += [f"{PKGCONFIG.get(m, m)} (development files)" for m in self.missing_modules]
        if not (self.sdl3 or self.system_sdl3):
            out.append("SDL3 (development files; or build Call of Duty 3 first with the same work folder, and its "
                       "SDK's SDL3 is used)")
        if not self.venv:
            out.append("Python's venv module (python3-venv)")
        return out


def _version(text: str) -> Tuple[int, ...]:
    m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    return tuple(int(x) for x in m.groups() if x is not None) if m else ()


def find_tools(runner: Runner, sdk_sdl3: Optional[Path] = None) -> Tools:
    """What this PC has for the build. sdk_sdl3: where Call of Duty 3's SDK keeps its SDL3 CMake package
    (Layout.sdk_sdl3), used when it is there."""
    which = shutil.which
    t = Tools(git=_path(which("git")), cmake=_path(which("cmake")), ninja=_path(which("ninja")),
              cc=_path(which("cc") or which("gcc") or which("clang")), glslc=_path(which("glslc")),
              pkgconfig=_path(which("pkg-config") or which("pkgconf")))
    if t.cmake:
        t.cmake_version = _version(runner.capture([t.cmake, "--version"])[1])
    if t.pkgconfig:
        t.missing_modules = [m for m in PKGCONFIG if runner.capture([t.pkgconfig, "--exists", m])[0] != 0]
    else:
        t.missing_modules = list(PKGCONFIG)
    if sdk_sdl3 is not None and (Path(sdk_sdl3) / "SDL3Config.cmake").is_file():
        t.sdl3 = Path(sdk_sdl3)
    t.system_sdl3 = bool(t.pkgconfig) and runner.capture([t.pkgconfig, "--exists", "sdl3"])[0] == 0
    rc, _ = runner.capture([sys.executable, "-c", "import venv, ensurepip"])
    t.venv = rc == 0
    return t


def _path(p: Optional[str]) -> Optional[Path]:
    return Path(p) if p else None


def packages_install(pm: str, wanted: Sequence[str]) -> Install:
    return Install("bro-system-packages", f"Packages Big Red One's build needs ({len(wanted)}, any already "
                   "installed are skipped): " + ", ".join(wanted),
                   f"the system's own folders, by {pm} (packages can only go there; it asks for your password)",
                   "often 50-300 MB", f"your distribution's {pm} repositories")


def install_packages(runner: Runner, log: Log) -> None:
    """Installs LINUX_PACKAGES with the distribution's package manager, after the user approves (deps.py's way:
    one script with only those names, written to a private folder, run once as root)."""
    immutable = deps.immutable_system()
    if immutable:
        raise SetupError(f"{immutable} is an image-based system: packages can't be added to it the usual way. "
                         + deps.CONTAINER_HINT)
    pm = deps._linux_package_manager()
    if not pm or pm not in LINUX_PACKAGES:
        raise SetupError("No supported package manager (apt, dnf, pacman, zypper, xbps) found. Install what the "
                         "log says is missing yourself, then build again.")
    wanted = LINUX_PACKAGES[pm]
    APPROVAL.require([packages_install(pm, wanted)], log)
    private = Path(tempfile.mkdtemp(prefix="bro-setup-"))
    script = private / "install-packages.sh"
    script.write_text(install_script(pm, wanted), encoding="utf-8")
    interactive = sys.stdin is not None and sys.stdin.isatty()
    prefix = deps._elevate_prefix(interactive)
    log.info(f"Installing Big Red One's build packages with {pm}" +
             (" - enter your password when asked." if prefix else "."))
    rc = runner.run(prefix + ["sh", str(script)], check=False)
    if rc != 0:
        raise SetupError(f"Package installation failed (exit code {rc}). You can run it yourself:\n"
                         f"  sudo sh \"{script}\"\nthen build again.")
    script.unlink()
    private.rmdir()


def install_script(pm: str, wanted: Sequence[str]) -> str:
    return (f"#!/bin/sh\n# Generated by the COD3 setup tool for Big Red One's build ({pm}).\nset -u\n"
            f"have() {{ {deps._PROBE[pm]}; }}\n{deps._REFRESH.get(pm, '')}\npkgs=\"\"\n"
            f"for p in {' '.join(wanted)}; do\n"
            "  if have \"$p\"; then pkgs=\"$pkgs $p\"; else echo \"skipping (not in your repositories): $p\"; fi\n"
            f"done\necho \"installing:$pkgs\"\n{deps._INSTALL[pm]} $pkgs\n")


def ensure_tools(runner: Runner, log: Log, allow_install: bool, found: Optional[Tools] = None,
                 sdk_sdl3: Optional[Path] = None) -> Tools:
    t = found or find_tools(runner, sdk_sdl3)
    if t.problems() and allow_install:
        log.info("Big Red One's build needs: " + "; ".join(t.problems()))
        install_packages(runner, log)
        t = find_tools(runner, sdk_sdl3)
    if t.problems():
        raise SetupError("Big Red One's build needs: " + "; ".join(t.problems()) + ". Turn on installing what's "
                         "missing (Set up), or install them yourself.")
    return t


# ---------------------------------------------------------------------------
# xboxrecomp and Capstone (in tools/: kept for the next build)
# ---------------------------------------------------------------------------
def xboxrecomp_install(lay: Layout) -> Install:
    return Install("xboxrecomp", f"xboxrecomp {XBOXRECOMP_VERSION} (MIT; the recompiler for original Xbox "
                   f"programs), its source at commit {XBOXRECOMP_COMMIT[:12]}", str(lay.xboxrecomp_cache),
                   "about 10 MB", "github.com")


def capstone_install(lay: Layout) -> Install:
    return Install("bro-capstone", f"Capstone {CAPSTONE.split('==')[1]} (the disassembler xboxrecomp uses), in a "
                   "Python environment of its own", str(lay.python_env), "about 10 MB", "pypi.org")


def downloads(lay: Layout, t: Tools, runner: Runner, allow_install: bool, windows: bool = False) -> List[Install]:
    """Everything the build would download or install on this PC, for one approval list before anything starts:
    the packages (when some are missing and installing is on), xboxrecomp and Capstone (when not there yet), and
    for the Windows version (`windows`, always on Windows) its compiler and sources (bro_windows.downloads)."""
    items = []
    if t.problems() and allow_install and IS_LINUX:
        pm = deps._linux_package_manager()
        if pm in LINUX_PACKAGES:
            items.append(packages_install(pm, LINUX_PACKAGES[pm]))
    src = lay.xboxrecomp_cache
    if not (t.git and (src / ".git").is_dir() and _git_head(t.git, src, runner) == XBOXRECOMP_COMMIT):
        items.append(xboxrecomp_install(lay))
    if not (lay.python.is_file() and runner.capture([lay.python, "-c", "import capstone"])[0] == 0):
        items.append(capstone_install(lay))
    if windows or IS_WINDOWS:
        items += bro_windows.downloads(lay.tools, t.git, t.glslc, runner)
    return items


# [acct2-ocr] 2026-11-12: files as they are in the repository (LF), whatever the system's Git does with line ends (Git
# for Windows turns LF into CRLF by default): bro/patches are applied to them.
NO_CRLF = ["-c", "core.autocrlf=false", "-c", "core.eol=lf"]


def _git_head(git: Path, repo: Path, runner: Runner) -> str:
    rc, out = runner.capture([git, "-C", repo, "rev-parse", "HEAD"])
    return out.strip() if rc == 0 else ""


def ensure_xboxrecomp(lay: Layout, git: Path, runner: Runner, log: Log) -> Path:
    """xboxrecomp's source at XBOXRECOMP_COMMIT in tools/ (fetched once, after approval)."""
    src = lay.xboxrecomp_cache
    if (src / ".git").is_dir() and _git_head(git, src, runner) == XBOXRECOMP_COMMIT:
        return src
    APPROVAL.require([xboxrecomp_install(lay)], log)
    src.mkdir(parents=True, exist_ok=True)
    if not (src / ".git").is_dir():
        runner.run([git, "init", "-q", src])
    runner.run([git, *NO_CRLF, "-C", src, "fetch", "--depth", "1", XBOXRECOMP_URL, XBOXRECOMP_COMMIT])
    runner.run([git, *NO_CRLF, "-C", src, "checkout", "-q", "--force", "--detach", XBOXRECOMP_COMMIT])
    if _git_head(git, src, runner) != XBOXRECOMP_COMMIT:
        raise SetupError(f"xboxrecomp in {src} isn't at commit {XBOXRECOMP_COMMIT[:12]} after the download.")
    return src


def source_key(lay: Layout) -> dict:
    """[waw-decomp] 2026-11-16 What the work copy of xboxrecomp is made from: the commit and the patches. Only when
    this changes is the copy made again (and the tables its steps wrote into it go with it)."""
    return {"xboxrecomp": XBOXRECOMP_COMMIT, "patches": {p.name: _file_md5(p) for p in lay.patches}}


def prepare_xboxrecomp(lay: Layout, git: Path, runner: Runner, log: Log) -> None:
    """A copy of the fetched source in the work folder (its steps write their tables into it), with the port's
    patches (bro/patches) applied."""
    lay.source_stamp.unlink(missing_ok=True)   # [waw-decomp] 2026-11-16 written again only once the copy is whole
    if lay.xboxrecomp.exists():
        common.safe_rmtree(lay.xboxrecomp, lay.work, [lay.out], log)
    lay.stamp.unlink(missing_ok=True)   # the steps' tables were in the copy just removed
    lay.work.mkdir(parents=True, exist_ok=True)
    runner.run([git, *NO_CRLF, "clone", "-q", "--no-checkout", lay.xboxrecomp_cache, lay.xboxrecomp])
    runner.run([git, *NO_CRLF, "-C", lay.xboxrecomp, "checkout", "-q", "--detach", XBOXRECOMP_COMMIT])
    for patch in lay.patches:
        runner.run([git, *NO_CRLF, "-C", lay.xboxrecomp, "apply", "--whitespace=nowarn", patch])
        log.info(f"Applied {patch.name}")
    common.write_json(lay.source_stamp, source_key(lay))   # [waw-decomp] 2026-11-16


def ensure_python(lay: Layout, runner: Runner, log: Log) -> Path:
    """A Python that can import capstone (tools/bro-python, made once, after approval)."""
    py = lay.python
    if py.is_file() and runner.capture([py, "-c", "import capstone"])[0] == 0:
        return py
    APPROVAL.require([capstone_install(lay)], log)
    if not py.is_file():
        runner.run([bro_windows.console_python(), "-m", "venv", lay.python_env])
    runner.run([py, "-m", "pip", "install", "--disable-pip-version-check", "--no-input", "--only-binary", ":all:",
                CAPSTONE])
    if runner.capture([py, "-c", "import capstone"])[0] != 0:
        raise SetupError(f"Capstone was installed into {lay.python_env}, but Python can't load it.")
    return py


# ---------------------------------------------------------------------------
# The game's files
# ---------------------------------------------------------------------------
def copy_game(disc: Disc, lay: Layout, log: Log, cancel: threading.Event) -> None:
    """The disc's files into <output>/bro/game (files already there at the same size are kept, so a build that
    stopped goes on), and default.xbe into the work folder for the recompiler."""
    copy_files(disc, lay, log, cancel)
    xbe = _folder_xbe(lay.game)
    if xbe is None:
        raise SetupError(f"default.xbe didn't arrive in {lay.game}.")
    lay.xbe.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(xbe, lay.xbe)
    if hashlib.md5(lay.xbe.read_bytes()).hexdigest() != disc.xbe_md5:
        raise SetupError("default.xbe changed while it was copied: build again.")


def copy_files(disc: Disc, lay: Layout, log: Log, cancel) -> None:
    """The disc's files into <output>/bro/game (files already there at the same size are kept). `cancel`: anything
    with is_set()."""
    if disc.kind == "iso":
        with XisoImage(disc.path) as img:
            img.extract(lay.game, log, cancel)
    elif common.resolved(disc.path) != common.resolved(lay.game):
        if common.overlaps(disc.path, lay.game):
            raise SetupError(f"The disc folder {disc.path} and the game folder {lay.game} are inside each other: "
                             "pick the disc's folder, or another output folder.")
        _copy_folder(disc.path, lay.game, log, cancel)


def place_xbe(disc: Disc, lay: Layout) -> None:
    """default.xbe into the work folder for the recompiler, read straight from the disc ([acct2-ocr] 2026-11-12):
    the recompiler needs only this file, so the rest of the disc is copied beside it (GameCopy)."""
    if disc.kind == "iso":
        with XisoImage(disc.path) as img:
            data = img.read("default.xbe")
    else:
        xbe = _folder_xbe(disc.path)
        if xbe is None:
            raise SetupError(f"{disc.path.name} has no default.xbe any more.")
        data = xbe.read_bytes()
    if hashlib.md5(data).hexdigest() != disc.xbe_md5:
        raise SetupError("default.xbe changed while it was read: build again.")
    lay.xbe.parent.mkdir(parents=True, exist_ok=True)
    tmp = lay.xbe.with_name(lay.xbe.name + ".part")
    tmp.write_bytes(data)
    tmp.replace(lay.xbe)


class _Either:
    """Set when any of the events is: a cancel the user can press and a stop the build can give."""

    def __init__(self, *events: threading.Event):
        self._events = events

    def is_set(self) -> bool:
        return any(e.is_set() for e in self._events)


class _NoBar:
    """A Log whose lines go on as they are and whose progress is dropped: the step in front owns the bar."""

    def __init__(self, log: Log):
        self._log = log

    def progress(self, frac: Optional[float], text: str = "") -> None:
        pass

    def __getattr__(self, name: str):
        return getattr(self._log, name)


class GameCopy:
    """The disc's files copied into <output>/bro/game on a thread of their own while default.xbe is recompiled and
    the game is built ([acct2-ocr], the owner 2026-11-12 18:07: "work on making the decompilation process for BRO
    quicker"): neither step reads them, only the game does, so the copy (a few GB; 51 s on the owner's PC, minutes
    from a DVD drive or a slow disk) no longer waits in front of them. The copy is then checked against the disc's
    list (check_game_files), on the same thread. finish() waits for both, raises what they met and logs the check;
    stop() ends them early (the build stopped)."""

    def __init__(self, disc: Disc, lay: Layout, log: Log, cancel: threading.Event):
        self._stop = threading.Event()
        self._error: Optional[BaseException] = None
        self._checked: Optional[FilesCheck] = None
        self._game = lay.game
        self._log = log
        self._seconds = 0.0
        self._thread = threading.Thread(target=self._run, name="bro-game-copy", daemon=True,
                                        args=(disc, lay, _NoBar(log), _Either(cancel, self._stop)))

    def start(self) -> "GameCopy":
        self._thread.start()
        return self

    def _run(self, disc: Disc, lay: Layout, log, cancel) -> None:
        t0 = time.monotonic()
        try:
            copy_files(disc, lay, log, cancel)
            self._checked = check_game_files(disc, lay.game, cancel)
        except BaseException as e:      # handed to finish(), on the build's own thread
            self._error = e
        self._seconds = time.monotonic() - t0

    def finish(self) -> None:
        if self._thread.is_alive():
            self._log.progress(None, "Copying the game's files: finishing")
        self._thread.join()
        if self._error is not None:
            raise self._error
        what = "Copied and checked" if self._checked is not None else "Copied"
        self._log.info(f"{what} the game's files beside the recompile ({format_minutes(self._seconds)}).")
        if self._checked is not None:
            report_game_files(self._checked, self._log, self._game)

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=60)


@dataclass
class FilesCheck:
    """The game's files against their disc's list (check_game_files)."""
    disc: str                                       # the disc's name (KNOWN_XBE)
    listed: int                                     # files on the disc's list
    same: int                                       # of them, the same size and MD5 in the game folder
    problems: List[str] = field(default_factory=list)   # one line per file that isn't


def disc_list(xbe_md5: str) -> Dict[str, "object"]:
    """The files of the disc with this default.xbe (lower-case path: disc_files.Known), or {} for a disc without a
    list."""
    key = DISC_LISTS.get(xbe_md5)
    if not key:
        return {}
    from . import disc_files     # its reader of the md5/size/path lists (Call of Duty 3's use the same format)
    return disc_files.load_list(DISC_MD5_DIR / f"{key}.tsv")


def check_game_files(disc: Disc, game: Path, cancel=None) -> Optional[FilesCheck]:
    """Every file on the disc's list against the one in `game` (the copy in <output>/bro/game): there, the same
    size and the same MD5. Reads every listed file (about 3-4 GB, beside the recompile). None for a disc without a
    list. Only reads."""
    known = disc_list(disc.xbe_md5)
    if not known:
        return None
    have = {p.relative_to(game).as_posix().lower(): p for p in _folder_files(game)} if game.is_dir() else {}
    check = FilesCheck(disc.known or disc.info.title or GAME_NAME, len(known), 0)
    for low, k in sorted(known.items()):
        if cancel is not None and cancel.is_set():
            raise common.Cancelled()
        p = have.get(low)
        if p is None:
            check.problems.append(f"missing: {k.path}")
            continue
        size = p.stat().st_size
        if size != k.size:
            check.problems.append(f"another size ({size} bytes, the disc's has {k.size}): {k.path}")
            continue
        h = hashlib.md5()
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(8 << 20), b""):
                if cancel is not None and cancel.is_set():
                    raise common.Cancelled()
                h.update(chunk)
        if h.hexdigest() != k.md5:
            check.problems.append(f"changed or damaged (another MD5): {k.path}")
            continue
        check.same += 1
    return check


def report_game_files(check: FilesCheck, log: Log, game: Path, shown: int = 10) -> None:
    """check_game_files' result in the log: one line when all is well, else a warning and the files. The build goes
    on either way (a damaged file may be one the game never reads), but the player is told."""
    if not check.problems:
        log.info(f"Checked the game's files: all {check.listed} are the same as on the disc ({check.disc}).")
        return
    log.warn(f"{len(check.problems)} of the {check.listed} game files aren't the same as on the disc ({check.disc}). "
             "The disc or its copy may be damaged, and the game may not run right where it reads them. To fix it, "
             f"delete these files from {game} and build again from a good copy of the disc: the build copies what "
             "is missing.")
    for line in check.problems[:shown]:
        log.warn(f"  {line}")
    if len(check.problems) > shown:
        log.warn(f"  ... and {len(check.problems) - shown} more")


def _copy_folder(src: Path, dest: Path, log: Log, cancel: threading.Event) -> None:
    files = _folder_files(src)
    total = sum(f.stat().st_size for f in files) or 1
    done, last = 0, 0.0
    log.info(f"{src}: {len(files)} files, {human(total)}")
    for f in files:
        if cancel.is_set():
            raise common.Cancelled()
        rel = f.relative_to(src)
        out = dest / rel
        if not is_within(out, dest):
            raise SetupError(f"Unsafe path in the disc folder: {rel}")
        size = f.stat().st_size
        if not (out.is_file() and out.stat().st_size == size):
            out.parent.mkdir(parents=True, exist_ok=True)
            tmp = out.with_name(out.name + ".part")
            shutil.copyfile(f, tmp)
            tmp.replace(out)
        done += size
        now = time.time()
        if now - last > 0.25:
            last = now
            log.progress(done / total, f"Copying the game's files: {rel.as_posix()}")
    log.progress(1.0, f"Copied {human(total)}")


def check_room(disc: Disc, lay: Layout, windows: bool = False, cache: bool = False) -> None:
    """Enough free space for the game's files (output) and the build (work); `windows`: and the Windows version's
    ([acct2-ocr] its build, and llvm-mingw unpacked in tools/ the first time); `cache` ([waw-decomp] 2026-11-16):
    and the disc's entry in the lift cache (since [waw-decomp/speed1] 2026-11-23 always)."""
    have_game = sum(f.stat().st_size for f in _folder_files(lay.game)) if lay.game.is_dir() else 0
    work_need = WORK_ROOM + (WINDOWS_ROOM if windows else 0) + (LIFT_CACHE_ROOM if cache else 0)
    needs = [(lay.out, max(0, disc.size - have_game)), (lay.work, work_need)]
    for folder, need in needs:
        probe = folder
        while not probe.exists() and probe != probe.parent:
            probe = probe.parent
        try:
            free = shutil.disk_usage(probe).free
        except OSError:
            continue
        if free < need:
            raise SetupError(f"Not enough room in {probe}: the build needs {human(need)} there, "
                             f"{human(free)} is free.")


# ---------------------------------------------------------------------------
# The recompile and the build
# ---------------------------------------------------------------------------
def _file_md5(path: Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest() if path.is_file() else ""


def recompile_key(lay: Layout, xbe_md5: str) -> dict:
    """What the recompiled C depends on: when any of it changes, it is made again. [waw-decomp] 2026-11-16: since
    then each step has a key of its own (step_keys); this one is kept, for people, in a lift cache entry's
    manifest (what the entry was made from)."""
    return {"xbe_md5": xbe_md5, "xboxrecomp": XBOXRECOMP_COMMIT,
            "patches": {p.name: _file_md5(p) for p in lay.patches},
            "seeds": _file_md5(lay.seeds), "stand_ins": _file_md5(lay.stand_ins)}


# ---------------------------------------------------------------------------
# [waw-decomp] 2026-11-16 A key per step, and the lift cache
# (owner 2026-11-16 13:58 "Work on optimizing the recompilation process ... Make sure the optimizations aren't disc
# specific"; 14:3x "Keep it unless cleanup is on"). Before, one stamp held all five steps, keyed on the whole
# stand-in table: a stand-in's new name made the 6-minute recompile run again from the disassembly.
# ---------------------------------------------------------------------------
STAMP_FORMAT = 2        # recompile.stamp.json: {"format": 2, "steps": {step: key}}; another shape: nothing done
CACHE_FORMAT = 2        # a lift cache entry's manifest ([waw-decomp/speed1] 2026-11-23: 2, the compressed entries)
CACHE_SUFFIX = ".tar.gz"   # [waw-decomp/speed1] an entry's archive (gzip: Python's own zlib)
CACHE_LEVEL = 6         # [waw-decomp/speed1] gzip's level: its default, quick and about 9 to 1 on the C
CACHE_DISCS = 4         # [waw-decomp/speed1] at most this many discs' entries per game
RUNTIME_HEADER = "recomp_types.h"   # templates/runtime/'s header, which the lift copies beside the C


def _sha256(path: Path) -> str:
    """A file's SHA-256 ("" when it isn't there)."""
    if not path.is_file():
        return ""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def tree_sha256(root: Path, skip: Sequence[str] = ("output", "__pycache__")) -> str:
    """One SHA-256 over every file under `root`: each file's path from root and its own SHA-256, in path order.
    Folders named in `skip` are left out (xboxrecomp's steps write their tables into tools/<step>/output, and Python
    its caches into __pycache__). The same files give the same hash on Linux and Windows."""
    found = []
    for folder, subfolders, names in os.walk(root):
        subfolders[:] = [d for d in subfolders if d not in skip]
        found += [Path(folder) / name for name in names]
    h = hashlib.sha256()
    for path in sorted(found, key=lambda p: p.relative_to(root).as_posix()):
        h.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0" + _sha256(path).encode("ascii") + b"\n")
    return h.hexdigest()


def _address(text: str) -> str:
    """A stand-in's address as 8 hex digits ("0x000634D0" and "000634d0" are the same address)."""
    try:
        return f"{int(text, 16):08X}"
    except ValueError:
        return text.strip()


@dataclass(frozen=True)
class LiftInputs:
    """What xboxrecomp's steps read besides each other's tables, from its source (f0f8a461 with bro/patches applied):
    - every step: default.xbe, its own code (the tools/ package; tools/lazy_decode.py too) and Python; the
      disassembly and the lift decode with capstone;
    - xbe_parser writes default_analysis.json, which the disassembly reads (loader._find_analysis_json);
    - the disassembly: the seeds (--seed-functions: each entry's start and observed);
    - func_id: the disassembly's functions, strings and xrefs; abi_analysis: functions and func_id's identified
      functions;
    - the lift (recomp): the tables above (functions, labels, summary, identified, abi), the stand-ins' addresses
      (--manual-functions: read as a set of addresses, so the table's order and its other columns, the names, don't
      change the C) and templates/runtime/recomp_types.h, which it copies beside the C.
    Each step's key (step_keys) holds what it reads and the key of the step before it, whose output it reads."""
    xbe_md5: str
    tools: str              # tree_sha256 of the patched tools/ tree
    runtime_header: str     # SHA-256 of templates/runtime/recomp_types.h
    python: str             # the steps' Python, e.g. "3.12.3"
    capstone: str           # capstone's version, e.g. "5.0.9"
    seeds: str              # SHA-256 of bro/config/seed_functions.json
    stand_ins: Tuple[str, ...]   # the stand-ins' addresses, sorted, 8 hex digits each


def lift_inputs(lay: Layout, py: Path, xbe_md5: str, runner: Runner) -> LiftInputs:
    rc, out = runner.capture([py, "-c", "import sys, capstone; print(sys.version.split()[0]); "
                                        "print(capstone.__version__)"])
    words = out.split()
    if rc != 0 or len(words) < 2:
        raise SetupError(f"The recompiler's Python ({py}) didn't say its version and Capstone's: {out.strip()[:300]}")
    return LiftInputs(
        xbe_md5=xbe_md5, tools=tree_sha256(lay.xboxrecomp / "tools"),
        runtime_header=_sha256(lay.xboxrecomp / "templates" / "runtime" / RUNTIME_HEADER),
        python=words[0], capstone=words[1], seeds=_sha256(lay.seeds),
        stand_ins=tuple(sorted({_address(a) for a in manual_functions(lay.stand_ins)})))


def step_flags(step: str, lay: Layout) -> List[str]:
    """A step's command line without the Python and with the work folder written as <work>: its options."""
    out = []
    for arg in step_command(step, lay, Path("python"))[1:]:
        try:
            out.append("<work>/" + Path(arg).relative_to(lay.work).as_posix())
        except ValueError:
            out.append(arg)
    return out


def step_keys(inputs: LiftInputs, lay: Layout) -> Dict[str, str]:
    """Each step's key: what it reads (LiftInputs' docstring) and the key of the step before it. A step whose key
    changed is done again, and so is every step after it (their keys hold its key)."""
    keys: Dict[str, str] = {}
    before = ""
    for step, _, _ in STEPS:
        parts = {"step": step, "after": before, "xbe_md5": inputs.xbe_md5, "tools": inputs.tools,
                 "python": inputs.python, "capstone": inputs.capstone, "options": step_flags(step, lay)}
        if step == "disasm":
            parts["seeds"] = inputs.seeds
        if step == "recomp":
            parts["stand_ins"] = list(inputs.stand_ins)
            parts["runtime_header"] = inputs.runtime_header
        keys[step] = before = _sha256_text(json.dumps(parts, sort_keys=True))
    return keys


def step_outputs(lay: Layout) -> Dict[str, List[Tuple[str, Path, Optional[List[str]]]]]:
    """Where each step leaves its output, as (its name in a cache entry, the folder, the file names, or None for
    every file at the folder's top). The disassembly's ASM listings (output/asm/) are left out: nothing reads them."""
    tools = lay.xboxrecomp / "tools"
    return {"xbe_parser": [("xbe_parser", lay.analysis_json.parent, [lay.analysis_json.name])],
            "disasm": [("disasm", tools / "disasm" / "output", None)],
            "func_id": [("func_id", tools / "func_id" / "output", None)],
            "abi_analysis": [("abi_analysis", tools / "abi_analysis" / "output", None)],
            "recomp": [("recomp", lay.recomp_out, None), ("gen", lay.gen, None)]}


def _files(folder: Path, names: Optional[List[str]]) -> List[Path]:
    if names is not None:
        return [folder / n for n in names]
    return sorted(p for p in folder.iterdir() if p.is_file()) if folder.is_dir() else []


def _outputs_there(step: str, lay: Layout) -> bool:
    for _, folder, names in step_outputs(lay)[step]:
        files = _files(folder, names)
        if not files or not all(f.is_file() for f in files):
            return False
    return True


def read_stamp(lay: Layout) -> Dict[str, str]:
    """The steps done in the work folder, with the key each was done with."""
    data = common.read_json(lay.stamp)
    steps = data.get("steps") if isinstance(data, dict) and data.get("format") == STAMP_FORMAT else None
    if not isinstance(steps, dict) or not (lay.xboxrecomp / ".git").exists():
        return {}   # (the tables are in xboxrecomp's folder: gone with it)
    return {k: v for k, v in steps.items() if isinstance(v, str)}


def write_stamp(lay: Layout, done: Dict[str, str]) -> None:
    common.write_json(lay.stamp, {"format": STAMP_FORMAT, "steps": done})


def steps_done(lay: Layout, keys: Dict[str, str]) -> int:
    """How many of the steps, from the first, are done in the work folder with today's keys."""
    done = read_stamp(lay)
    n = 0
    for step, _, _ in STEPS:
        if done.get(step) != keys[step] or not _outputs_there(step, lay):
            break
        n += 1
    return n


def _copy_new(src: Path, dest: Path) -> None:
    """src over dest, dated now (not src's date: a build must see a changed file as newer than what it made)."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    shutil.copyfile(src, tmp)
    tmp.replace(dest)


class LiftCache:
    """[waw-decomp/speed1] 2026-11-23 The recompiler's output kept between builds, with cleanup on or off (owner 21:00:
    "keep the BRO and FH lift cache even with cleanup on"): <work>/tools/lift-cache/<bro or fh>/, in the work
    folder's tools/, which a cleanup keeps. One entry per disc, the newest, and at most CACHE_DISCS discs:
    - <xbe MD5>-<key>.tar.gz: every step's tables and the C, compressed with gzip (Python's own zlib, so nothing to
      install on any system; Big Red One's C, about 180 MB, takes about 20 MB);
    - <xbe MD5>-<key>.json beside it: the five steps' keys and each file's size and SHA-256.
    Each step's key holds the disc's default.xbe MD5, xboxrecomp's patched tools/ tree (its commit with every patch),
    Python's and Capstone's versions and the step's options (LiftInputs, step_keys), so a changed disc, xboxrecomp or
    patch lifts again from the first step that changed; the new entry then replaces the disc's older one. Nothing here
    names a disc but its MD5. An entry is written under a .partial- name and renamed into place only when whole; its
    files are checked against the manifest when they are taken, and one that doesn't match is removed and that part
    lifted again. clear() (the build's --clear-lift-cache) removes every entry of the game.
    [waw-decomp] 2026-11-16: the first version kept a folder per entry in <work>/bro, only with cleanup off."""

    def __init__(self, root: Path, log: Log, game: str = GAME_NAME):
        self.root = Path(root)
        self.log = log
        self.game = game

    @staticmethod
    def _archive(base: Path) -> Path:
        return base.with_name(base.name + CACHE_SUFFIX)

    @staticmethod
    def _manifest_path(base: Path) -> Path:
        return base.with_name(base.name + ".json")

    def _read(self, base: Path) -> dict:
        """An entry's manifest ({} when it is missing or of another format)."""
        m = common.read_json(self._manifest_path(base))
        return m if isinstance(m, dict) and m.get("format") == CACHE_FORMAT else {}

    def _bases(self, pattern: str = "*") -> List[Path]:
        """The whole entries (their names without the endings) whose names match `pattern`."""
        if not self.root.is_dir():
            return []
        bases = [p.with_name(p.name[:-len(".json")]) for p in self.root.glob(f"{pattern}.json")]
        return sorted(b for b in bases if self._archive(b).is_file())

    def name(self, xbe_md5: str, keys: Dict[str, str]) -> Path:
        return self.root / f"{xbe_md5}-{keys['recomp'][:16]}"

    def remove(self, base: Path) -> None:
        """One entry: its manifest first, so a half-removed entry is never read."""
        for p in (self._manifest_path(base), self._archive(base)):
            try:
                p.unlink()
            except FileNotFoundError:
                pass

    def remove_partial(self) -> None:
        """What a build that stopped while writing an entry left."""
        if self.root.is_dir():
            for p in self.root.glob("*.partial-*"):
                p.unlink()

    def clear(self) -> None:
        """Every entry of this game (the build's --clear-lift-cache)."""
        if self.root.is_dir():
            common.safe_rmtree(self.root, self.root.parent, [], self.log)
            self.log.info(f"Removed the kept recompiler output in {self.root}, as asked: every step is done again.")

    def has(self, xbe_md5: str, keys: Dict[str, str]) -> bool:
        base = self.name(xbe_md5, keys)
        return self._read(base).get("keys") == keys and self._archive(base).is_file()

    def best(self, xbe_md5: str, keys: Dict[str, str]) -> Tuple[Optional[Path], dict, int]:
        """This disc's entry that holds the most steps (from the first) done with today's keys: (entry, manifest,
        how many steps), or (None, {}, 0). Its files are checked when they are taken (restore)."""
        found: Tuple[Optional[Path], dict, int] = (None, {}, 0)
        for base in self._bases(f"{xbe_md5}-*"):
            m = self._read(base)
            theirs = m.get("keys")
            if not isinstance(theirs, dict) or not isinstance(m.get("files"), dict) or not m["files"]:
                continue
            n = 0
            for step, _, _ in STEPS:
                if theirs.get(step) != keys[step]:
                    break
                n += 1
            if n > found[2]:
                found = (base, m, n)
        return found

    def unpack(self, base: Path, manifest: dict, into: Path) -> bool:
        """The entry's files into `into` (made new), each checked against the manifest as it is written. False, and
        the entry removed, when the archive is damaged or a file is missing or doesn't match."""
        files = manifest["files"]
        if into.exists():
            common.safe_rmtree(into, into.parent, [], self.log)
        into.mkdir(parents=True)
        seen = set()
        try:
            with tarfile.open(self._archive(base), "r:gz") as tar:
                for member in tar:
                    info = files.get(member.name)
                    rel = PurePosixPath(member.name)
                    if (not isinstance(info, dict) or not member.isfile() or rel.is_absolute() or ".." in rel.parts
                            or ":" in member.name or "\\" in member.name):
                        continue   # only the files the manifest lists, as plain files inside `into`
                    source = tar.extractfile(member)
                    if source is None:
                        continue
                    if _write_hashed(source, into.joinpath(*rel.parts)) != info:
                        raise ValueError(f"{member.name} isn't what was kept")
                    seen.add(member.name)
        except (OSError, EOFError, ValueError, tarfile.TarError, zlib.error) as e:
            self.log.warn(f"The kept recompiler output {self._archive(base).name} can't be used ({e}): it is removed, "
                          "and that part is made again.")
            self.remove(base)
            return False
        if seen != set(files):
            self.log.warn(f"The kept recompiler output {self._archive(base).name} is missing files: it is removed, "
                          "and that part is made again.")
            self.remove(base)
            return False
        return True

    def restore(self, entry: Path, manifest: dict, lay: Layout, steps: Sequence[str]) -> bool:
        """The outputs of `steps` from an entry into the work folder (unpacked and checked first; False when the
        entry doesn't check out, see unpack). A file that is the same already is left as it is, dated as it was, so
        the build doesn't compile it again; one the entry doesn't have goes (in folders that hold only that step's
        output)."""
        unpacked = lay.work / "lift-cache-unpacked"
        if not self.unpack(entry, manifest, unpacked):
            return False
        files = manifest["files"]
        try:
            for step in steps:
                for name, folder, names in step_outputs(lay)[step]:
                    ours = {rel.split("/", 1)[1]: info for rel, info in files.items() if rel.split("/", 1)[0] == name}
                    folder.mkdir(parents=True, exist_ok=True)
                    if names is None:
                        for p in folder.iterdir():
                            if p.name not in ours:
                                if p.is_dir():
                                    common.safe_rmtree(p, lay.work, [lay.out], self.log)
                                else:
                                    p.unlink()
                    for fname, info in ours.items():
                        dest = folder / fname
                        if dest.is_file() and dest.stat().st_size == info["size"] and _sha256(dest) == info["sha256"]:
                            continue
                        _copy_new(unpacked / name / fname, dest)
        finally:
            common.safe_rmtree(unpacked, lay.work, [lay.out], self.log)
        return True

    def store(self, lay: Layout, xbe_md5: str, keys: Dict[str, str], made_from: dict,
              cancel: Optional[threading.Event] = None) -> Path:
        """The work folder's tables and C as this disc's entry, compressed (replacing its older ones and, past
        CACHE_DISCS discs, the oldest disc's); returns the archive."""
        self.root.mkdir(parents=True, exist_ok=True)
        self.remove_partial()
        base = self.name(xbe_md5, keys)
        tmp = self.root / f"{base.name}.partial-{os.getpid()}"
        files = {}
        try:
            with tarfile.open(tmp, "w:gz", compresslevel=CACHE_LEVEL) as tar:
                for step, _, _ in STEPS:
                    for name, folder, names in step_outputs(lay)[step]:
                        for src in _files(folder, names):
                            if cancel is not None and cancel.is_set():
                                raise Cancelled()
                            rel = f"{name}/{src.name}"
                            files[rel] = _add_hashed(tar, src, rel)
            self.remove(base)
            tmp.replace(self._archive(base))
        except BaseException:
            tmp.unlink(missing_ok=True)
            raise
        common.write_json(self._manifest_path(base), {
            "format": CACHE_FORMAT, "game": self.game, "xbe_md5": xbe_md5, "keys": keys, "made_from": made_from,
            "written": time.strftime("%Y-%m-%d %H:%M"), "files": files})
        for older in self._bases(f"{xbe_md5}-*"):
            if older != base:
                self.remove(older)
        newest_first = sorted(self._bases(), key=lambda b: self._archive(b).stat().st_mtime, reverse=True)
        for oldest in newest_first[CACHE_DISCS:]:
            self.remove(oldest)
        return self._archive(base)


def _add_hashed(tar: tarfile.TarFile, src: Path, rel: str) -> dict:
    """[waw-decomp/speed1] src into the archive as rel, with no owner, date or permissions of this PC: its size and
    SHA-256."""
    info = tarfile.TarInfo(rel)
    info.size = src.stat().st_size
    info.mode = 0o644
    h = hashlib.sha256()
    with open(src, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
        f.seek(0)
        tar.addfile(info, f)
    return {"size": info.size, "sha256": h.hexdigest()}


def _write_hashed(source, dest: Path) -> dict:
    """[waw-decomp/speed1] A readable file object into dest: what was written, as {"size", "sha256"}."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    h, size = hashlib.sha256(), 0
    with open(dest, "wb") as out:
        for chunk in iter(lambda: source.read(1 << 20), b""):
            h.update(chunk)
            out.write(chunk)
            size += len(chunk)
    return {"size": size, "sha256": h.hexdigest()}


def drop_old_lift_cache(lay: Layout, log: Log) -> None:
    """[waw-decomp/speed1] The first version's lift cache (<work>/bro/lift-cache, folders) once the build runs with
    the compressed one: removed, so its 0.3 GB per disc doesn't stay behind."""
    old = lay.work / "lift-cache"
    if old.is_dir():
        common.safe_rmtree(old, lay.work, [lay.out], log)


def keep_lift(cache: LiftCache, lay: Layout, xbe_md5: str, keys: Dict[str, str], ran: Sequence[str],
              made_from: dict, cancel: Optional[threading.Event], log: Log) -> None:
    """[waw-decomp/speed1] After the recompile: its output as this disc's entry, when a step ran or the entry isn't
    there yet. A failure (a full disk, say) is only noted: the next build of this disc recompiles, that's all."""
    if not ran and cache.has(xbe_md5, keys):
        return
    t0 = time.monotonic()
    try:
        archive = cache.store(lay, xbe_md5, keys, made_from, cancel)
        log.info(f"Kept the recompiler's output for the next build of this disc in {archive} "
                 f"({human(archive.stat().st_size)}, {format_minutes(time.monotonic() - t0)}).")
    except OSError as e:
        log.warn(f"The recompiler's output couldn't be kept for the next build: {e}")
        cache.remove_partial()


def restore_lift(lay: Layout, keys: Dict[str, str], xbe_md5: str, cache: Optional[LiftCache], log: Log) -> int:
    """Before the recompile: the steps done already (in the work folder, else from the lift cache, whichever has
    more of them); returns how many, from the first, need not run."""
    have = steps_done(lay, keys)
    if have == len(STEPS) or cache is None:
        return have
    cache.remove_partial()
    entry, manifest, n = cache.best(xbe_md5, keys)
    if entry is None or n <= have:
        return have
    names = [s for s, _, _ in STEPS][:n]
    log.info("Taking " + ("the whole recompile" if n == len(STEPS) else f"the recompiler's first {n} steps") +
             f" from an earlier build of this disc ({entry.name}).")
    t0 = time.monotonic()
    if not cache.restore(entry, manifest, lay, names):   # [waw-decomp/speed1] 2026-11-23 checked as it is unpacked
        return have
    write_stamp(lay, {s: keys[s] for s in names})   # the steps after these follow from them: none are kept
    log.info(f"Taken in {format_minutes(time.monotonic() - t0)}.")
    return n


def remove_stale_c(lay: Layout, log: Log) -> None:
    """After a lift into a kept gen/: the files it didn't write (another disc's, an older lifter's) go, so the build
    doesn't compile them. The lift's summary (recomp_out/summary.json, "files") names what it wrote; it also writes
    recomp_types.h. Its own numbered chunks past the new count it removes itself."""
    summary = common.read_json(lay.recomp_out / "summary.json")
    written = summary.get("files") if isinstance(summary, dict) else None
    if not isinstance(written, list) or not written:
        log.warn(f"The recompiler's summary doesn't say which files it wrote: everything in {lay.gen} is kept.")
        return
    keep = {Path(str(f)).name for f in written} | {RUNTIME_HEADER}
    stale = sorted(p for p in lay.gen.iterdir() if p.is_file() and p.name not in keep)
    for p in stale:
        p.unlink()
    if stale:
        log.info(f"Removed {len(stale)} file(s) from {lay.gen} that this recompile didn't write: "
                 + ", ".join(p.name for p in stale[:5]) + (" ..." if len(stale) > 5 else ""))


def manual_functions(tsv: Path) -> List[str]:
    """The stand-ins' addresses (the table's first column): the recompiler routes every call to them through the
    port's own versions (bro/README.md)."""
    out = []
    for i, line in enumerate(tsv.read_text(encoding="utf-8").splitlines()):
        cell = line.split("\t", 1)[0].strip()
        if i == 0 or not cell or cell.startswith("#"):
            continue
        out.append(cell)
    return out


def step_command(key: str, lay: Layout, py: Path) -> List[str]:
    """One of xboxrecomp's steps, run from its folder (it imports its tools package from there)."""
    xbe = str(lay.xbe)
    if key == "xbe_parser":
        return [str(py), "-m", "tools.xbe_parser", xbe, "--json", str(lay.xbe.with_name("default_analysis.json"))]
    if key == "disasm":
        return [str(py), "-m", "tools.disasm", xbe, "-v", "--seed-functions", str(lay.work / "seed_functions.json")]
    if key in ("func_id", "abi_analysis"):
        return [str(py), "-m", f"tools.{key}", xbe, "-v"]
    if key == "recomp":
        return [str(py), "-m", "tools.recomp", xbe, "--game-name", "Big Red One", "--all", "--split", "250",
                "--gen-dir", str(lay.gen), "-o", str(lay.recomp_out),
                "--manual-functions", str(lay.work / "manual_functions.json"), "-v"]
    raise ValueError(key)


def recompile(lay: Layout, py: Path, xbe_md5: str, runner: Runner, log: Log,
              on_step: Optional[Callable[[str, float], None]] = None, keys: Optional[Dict[str, str]] = None,
              keep_gen: bool = False) -> List[str]:
    """default.xbe to C (lay.gen) with xboxrecomp's steps. Each finished step is noted in the stamp, so a build that
    stopped goes on from the step it stopped in. [waw-decomp] 2026-11-16: with its own key (step_keys), so a step
    is done again only when something it reads changed; keep_gen (cleanup off): gen/ stays, so the lift's
    write-if-changed leaves the files it would write the same as they are, dates and all, and the build compiles
    only the changed ones. Returns the steps it ran."""
    keys = keys or step_keys(lift_inputs(lay, py, xbe_md5, runner), lay)
    done = read_stamp(lay)
    shutil.copy2(lay.seeds, lay.work / "seed_functions.json")
    (lay.work / "manual_functions.json").write_text(json.dumps(manual_functions(lay.stand_ins)), encoding="utf-8")
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    env.pop("PYTHONPATH", None)
    start = 0.0
    names = [s for s, _, _ in STEPS]
    ran: List[str] = []
    for i, (step, what, share) in enumerate(STEPS):
        if on_step:
            on_step(what, start)
        if done.get(step) == keys[step] and _outputs_there(step, lay):
            log.info(f"{what}: done before, kept.")
            start += share
            continue
        # This step's output is about to change, so it and every step after it count as not done until they are.
        for later in names[i:]:
            done.pop(later, None)
        write_stamp(lay, done)
        if step == "recomp":
            if lay.recomp_out.exists():   # its summary names the files the lift writes (remove_stale_c)
                common.safe_rmtree(lay.recomp_out, lay.work, [lay.out], log)
            if lay.gen.exists() and not keep_gen:
                common.safe_rmtree(lay.gen, lay.work, [lay.out], log)
            for d in (lay.gen, lay.recomp_out):
                d.mkdir(parents=True, exist_ok=True)
        log.step(what)
        t0 = time.monotonic()
        with _StepClock(log, what, start, share) as clock:
            runner.run(step_command(step, lay, py), cwd=lay.xboxrecomp, env=env,
                       line_filter=_progress_lines(log, what, clock.where))
        log.info(f"{what}: {format_minutes(time.monotonic() - t0)}.")
        if step == "recomp":
            remove_stale_c(lay, log)
        done[step] = keys[step]
        write_stamp(lay, done)
        ran.append(step)
        start += share
    if not any(lay.gen.glob("*.c")):
        raise SetupError(f"The recompiler wrote no C into {lay.gen}: see the log above.")
    return ran


def _progress_lines(log: Log, what: str, frac) -> Callable[[str], None]:
    """Each line of a step's output as the status line (twice a second at most), the bar at `frac`: a number, or a
    one-item list that a _StepClock moves on while the step runs."""
    last = [0.0]
    where = frac if isinstance(frac, list) else [frac]

    def handle(line: str) -> None:
        now = time.monotonic()
        if line.strip() and now - last[0] > 0.5:
            last[0] = now
            log.progress(where[0], f"{what}: {line.strip()[:90]}")
    return handle


# About how long the recompile's steps take in all; STEPS' shares are of this time. [acct2-ocr] 2026-11-12 measured
# with bro/patches/xboxrecomp-speed.patch (v3ce, the owner's PC): xbe_parser 0 s, disasm 215 s, func_id 7 s,
# abi_analysis 5 s, recomp 196 s. Before it ([acct2-bro] 2026-11-08, lazy233): 0, 706, 7, 5 and 300 s (1020).
RECOMPILE_SECONDS = 425


class _StepClock:
    """While one of xboxrecomp's steps runs, every `every` seconds: the bar moves on by the time spent (up to 95% of
    the step's share, until the step ends) and the status line says how long it has run. The disassembly prints
    nothing for many minutes at a time, and a bar that stood still looked like a frozen tool: the owner's build of
    2026-11-06 was closed 14 minutes into it, so there was no game to play ("it wouldn't launch", 2026-11-07)."""

    def __init__(self, log: Log, what: str, start: float, share: float, every: float = 30.0):
        self.log, self.what, self.start, self.share, self.every = log, what, start, share, every
        self.where = [start]     # the bar's place, shared with _progress_lines
        self._stop = threading.Event()
        self._t0 = time.monotonic()
        self._thread = threading.Thread(target=self._tick, daemon=True)

    def __enter__(self) -> "_StepClock":
        self._thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self._stop.set()
        self._thread.join(timeout=5)

    def _tick(self) -> None:
        while not self._stop.wait(self.every):
            spent = time.monotonic() - self._t0
            expected = max(1.0, self.share * RECOMPILE_SECONDS)
            self.where[0] = self.start + self.share * min(0.95, spent / expected)
            self.log.progress(self.where[0], f"{self.what}: still working, {format_minutes(spent)} so far "
                                             "(it shows little until it is done; leave the tool open)")


def format_minutes(seconds: float) -> str:
    s = int(round(seconds))
    return f"{s} s" if s < 60 else f"{s // 60} min {s % 60} s"


def configure_command(lay: Layout, t: Tools) -> List[str]:
    cmd = [str(t.cmake), "-S", str(lay.bro_src), "-B", str(lay.build), f"-DXBOXRECOMP_DIR={lay.xboxrecomp}",
           f"-DBRO_GEN_DIR={lay.gen}", "-DCMAKE_BUILD_TYPE=Release",
           f"-DPython3_EXECUTABLE={bro_windows.console_python()}",
           f"-DCMAKE_C_FLAGS={bro_windows.C_FLAGS}"]   # [acct2-ocr] 2026-11-13: GCC 14 (bro_windows.C_FLAGS)
    if t.sdl3:
        cmd.append(f"-DSDL3_DIR={t.sdl3}")    # Call of Duty 3's SDK's static SDL3 (bro/CMakeLists.txt's find_package)
    if t.ninja:
        cmd[1:1] = ["-G", "Ninja"]
    return cmd


def build_command(lay: Layout, t: Tools, jobs: int) -> List[str]:
    return [str(t.cmake), "--build", str(lay.build), "-j", str(max(1, jobs))]


def host_compilers() -> List[str]:
    """[waw-decomp/speed1] 2026-11-23: the C and C++ compilers CMake takes for a build that names none
    (CC and CXX, else cc and c++): the ones build_speed tries the linker with."""
    return [os.environ.get("CC") or "cc", os.environ.get("CXX") or "c++"]


def build(lay: Layout, t: Tools, jobs: int, runner: Runner, log: Log, keep: bool = False,
          target: Optional[str] = None) -> Optional[Path]:
    """bro/ with CMake against the recompiled C; returns the program. [waw-decomp] 2026-11-16: keep: the build
    folder of the last build is used again when it was configured the same way, so only what changed is compiled
    (cleanup off; else a new one, as before: bro_windows.configure_folder). target: only that CMake target (the
    libraries, while the recompiler still writes the C): returns None."""
    # [waw-decomp/speed1] 2026-11-23: this PC's speed arguments (c3w/build_speed.py); a folder not kept starts new
    speed = build_speed.cmake_args(lay.build, host_compilers(), ninja=bool(t.ninja), fresh=True if not keep
                                   else None, log=log)
    bro_windows.configure_folder(configure_command(lay, t) + speed, lay.build, keep, lay, runner, log)
    runner.run(build_command(lay, t, jobs) + (["--target", target] if target else []),
               line_filter=_progress_lines(log, "Compiling" + (f" ({target})" if target else ""), 0.0))
    if target:
        return None
    exe = lay.build / common.exe_name("bro")
    if not exe.is_file():
        raise SetupError(f"The build finished without {exe.name} in {lay.build}: see the log above.")
    cache = lay.build / "CMakeCache.txt"
    text = cache.read_text(encoding="utf-8", errors="replace") if cache.is_file() else ""
    if "BRO_GLSLC:FILEPATH=BRO_GLSLC-NOTFOUND" in text or "Vulkan_INCLUDE_DIR:PATH=Vulkan_INCLUDE_DIR-NOTFOUND" in text:
        raise SetupError("The build found no Vulkan or no glslc, so the game couldn't draw anything: install them "
                         "(Vulkan development files and glslc) and build again.")
    if "SDL3_DIR:PATH=SDL3_DIR-NOTFOUND" in text:
        raise SetupError("The build found no SDL3, so the game would have no window, sound or pads: install SDL3's "
                         "development files, or build Call of Duty 3 first with the same work folder, and build again.")
    return exe


# ---------------------------------------------------------------------------
# Installing and starting
# ---------------------------------------------------------------------------
LAUNCHER_SH = """#!/bin/sh
# Call of Duty 2: Big Red One, built by the COD3 setup tool from your own disc. Starts the game with its files in
# game/ and its saves in saves/, drawn with Vulkan. The setup tool's Play page has the same start with options.
here=$(cd "$(dirname "$0")" && pwd)
BRO_VK=${BRO_VK:-1}
export BRO_VK
mkdir -p "$here/saves" "$here/logs"
exec "$here/bin/bro" "$here/game" "$here/saves" "$@"
"""


def _place(exe: Path, dest: Path, executable: bool) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".new")
    shutil.copy2(exe, tmp)
    if executable and not IS_WINDOWS:
        tmp.chmod(tmp.stat().st_mode | 0o755)
    tmp.replace(dest)


def install(lay: Layout, exe: Optional[Path], disc: Disc, tool_version: str,
            exe_windows: Optional[Path] = None, log: Optional[Log] = None) -> None:
    """The programs into <output>/bro/bin (Linux: bro, Windows: bro.exe; [acct2-ocr] both when both were built),
    their launchers (play.sh, play.bat), saves/ and bro.json; [input-port] the controller database beside them."""
    if exe is not None:
        _place(exe, lay.exe, True)
    if exe_windows is not None:
        _place(exe_windows, lay.exe_windows, False)
    if exe is not None or exe_windows is not None:
        # [input-port] 2026-11-24: bro_pad.c reads gamecontrollerdb.txt next to the program (SDL_GameControllerDB)
        controller_db.copy_next_to(lay.exe.parent, log.warn if log is not None else None)
    lay.saves.mkdir(parents=True, exist_ok=True)
    lay.logs.mkdir(parents=True, exist_ok=True)
    if exe is not None and not IS_WINDOWS:
        lay.launcher.write_text(LAUNCHER_SH, encoding="utf-8")
        lay.launcher.chmod(0o755)
    if exe_windows is not None:
        # [rexglue-native-platform] 2026-11-16: the bytes as they are (write_text's newline= needs Python 3.10, and
        # the tool supports 3.9: a TypeError there)
        lay.launcher_windows.write_bytes(bro_windows.LAUNCHER_BAT_TEXT.encode("utf-8"))
    m = {"game": GAME_NAME, "tool_version": tool_version, "built": time.strftime("%Y-%m-%d %H:%M"),
         "xbe_md5": disc.xbe_md5, "xboxrecomp": XBOXRECOMP_COMMIT}
    if exe is not None and not IS_WINDOWS:
        m["exe_linux"] = lay.exe.relative_to(lay.out).as_posix()
    if exe_windows is not None:
        m["exe_windows"] = lay.exe_windows.relative_to(lay.out).as_posix()
    common.write_json(lay.manifest, {**m, "game_dir": "game", "saves_dir": "saves"})


@dataclass
class PlayOptions:
    """The Play page's choices: settings of the port itself (bro/src), passed as environment variables."""
    level: str = ""                  # a map from LEVELS; "" = the menus
    fullscreen: bool = False
    vsync: bool = True
    size: str = ""                   # "1280x960"; "" = the port's own default
    mouse_look: bool = True
    mouse_speed: int = DEFAULT_MOUSE_SPEED
    mute: bool = False

    def problems(self) -> List[str]:
        out = []
        if self.level not in {key for key, _ in LEVELS}:
            out.append(f"Unknown level: {self.level}")
        if self.size and not re.fullmatch(r"\d{2,5}x\d{2,5}", self.size):
            out.append(f"Window size: {self.size} isn't WIDTHxHEIGHT")
        if not MOUSE_SPEED_RANGE[0] <= self.mouse_speed <= MOUSE_SPEED_RANGE[1]:
            out.append(f"Mouse speed: {MOUSE_SPEED_RANGE[0]} to {MOUSE_SPEED_RANGE[1]}")
        return out

    def to_settings(self) -> dict:
        return {f"bro_{k}": v for k, v in asdict(self).items()}

    @classmethod
    def from_settings(cls, s: dict) -> "PlayOptions":
        """The choices saved in settings `s`; a value of the wrong type or out of range is left at its default."""
        o = cls()
        for k, default in asdict(cls()).items():
            v = s.get(f"bro_{k}")
            if type(v) is type(default):   # (a bool isn't taken for a number, nor a number for a bool)
                setattr(o, k, v)
        if o.level not in {key for key, _ in LEVELS}:
            o.level = ""
        if o.size and not re.fullmatch(r"\d{2,5}x\d{2,5}", o.size):
            o.size = ""
        if not MOUSE_SPEED_RANGE[0] <= o.mouse_speed <= MOUSE_SPEED_RANGE[1]:
            o.mouse_speed = DEFAULT_MOUSE_SPEED
        return o


def play_env(opts: PlayOptions, base: Optional[Dict[str, str]] = None) -> Dict[str, str]:
    env = dict(os.environ if base is None else base)
    env["BRO_VK"] = "1"
    for name in ("BRO_DEVMAP", "BRO_FULLSCREEN", "BRO_VSYNC", "BRO_VK_SIZE", "BRO_MOUSE_LOOK", "BRO_MOUSE_SPEED",
                 "BRO_SOUND_MUTE"):
        env.pop(name, None)
    if opts.level:
        env["BRO_DEVMAP"] = opts.level
    if opts.fullscreen:
        env["BRO_FULLSCREEN"] = "1"
    if not opts.vsync:
        env["BRO_VSYNC"] = "0"
    if opts.size:
        env["BRO_VK_SIZE"] = opts.size
    if not opts.mouse_look:
        env["BRO_MOUSE_LOOK"] = "0"
    if opts.mouse_speed != DEFAULT_MOUSE_SPEED:
        env["BRO_MOUSE_SPEED"] = str(opts.mouse_speed)
    if opts.mute:
        env["BRO_SOUND_MUTE"] = "1"
    return env


def last_build(output: Path) -> str:
    """How the last build into an output folder ended, from its log (<output>/bro/logs/build.log), for Play when
    there is no game yet; "" when no build was started there."""
    path = Path(output) / OUTPUT_SUBDIR / "logs" / "build.log"
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        when = time.strftime("%a %m-%d %H:%M", time.localtime(path.stat().st_mtime))
    except OSError:
        return ""
    steps = [line.split("[step] ", 1)[1] for line in lines if "[step] " in line]
    errors = [line.split("[error] ", 1)[1] for line in lines if "[error] " in line]
    where = steps[-1] if steps else "its start"
    if errors:
        return f"The last build stopped at \"{where}\" ({when}): {errors[-1][:300]}"
    return (f"The last build stopped at \"{where}\" ({when}) without an error: it was stopped, or the setup tool "
            "was closed while it ran.")


def play_command(output: Path) -> Tuple[Optional[List[str]], str]:
    """(the command that starts the game, "") or (None, why it can't start)."""
    exe = installed(Path(output)) if output else None
    if exe is None:
        ended = last_build(Path(output)) if output else ""
        if ended:
            return None, (f"Not built yet. {ended} Press Build again: it goes on from where it stopped. The "
                          "recompile takes 5-15 minutes; leave the tool open until it says Done.")
        return None, "Not built yet: choose your disc on Big Red One's Set up page and press Build."
    lay = layout(Path(output), Path(output))
    return [str(exe), str(lay.game), str(lay.saves)], ""


# ---------------------------------------------------------------------------
# The whole build
# ---------------------------------------------------------------------------
@dataclass
class BuildOptions:
    source: str
    output_dir: str
    work_dir: str
    jobs: int = 1
    install_deps: bool = True
    cleanup: bool = True
    tool_version: str = ""
    clear_lift_cache: bool = False   # [waw-decomp/speed1] 2026-11-23 empty the lift cache first (--clear-lift-cache)
    # [acct2-ocr] 2026-11-12: on Linux, also the Windows version (bin/bro.exe and play.bat, for a Windows PC), made
    # with llvm-mingw. On Windows the build is the Windows version anyway.
    windows: bool = False


def run_build(o: BuildOptions, runner: Runner, log: Log, cancel: threading.Event,
              on_step: Optional[Callable[[int, int, str], None]] = None) -> str:
    """Every step (see the module's text); returns where the game went. A build that stopped goes on: the game's
    files and the finished recompiler steps are kept."""
    ok, why = supported()
    if not ok:
        raise SetupError(why)
    if not o.output_dir.strip():
        raise SetupError("Choose an output folder first (Set up).")
    work = Path(os.path.expanduser(o.work_dir.strip() or str(common.default_work_dir())))
    lay = layout(Path(os.path.expanduser(o.output_dir.strip())), work)
    titles = ["Checking the disc", "Checking this PC", "Copying the game's files (beside the next steps)",
              "Getting xboxrecomp and Capstone",
              "Recompiling default.xbe", "Building", "Installing"]

    def step(i: int) -> None:
        runner.check_cancel()
        log.step(titles[i])
        if on_step:
            on_step(i, len(titles), titles[i])

    step(0)
    check_project(lay)
    disc, line = check_disc(o.source)
    if disc is None:
        raise SetupError(line)
    log.info(line)
    if common.overlaps(lay.work, lay.out):
        raise SetupError(f"The work folder {work} and the output folder {lay.out.parent} are inside each other: "
                         "pick two separate folders.")
    common.claim_work_dir(work)
    # [waw-decomp] 2026-11-16 (rc2-paths; owner 14:33 "make sure ANY character that windows supports within a path
    # is allowed"): the compilers, CMake and xboxrecomp reach the work folder and the project folder through links
    # with plain names when their paths have characters a build tool chokes on (common.alias_path: a ";" split
    # bro/CMakeLists.txt's lists of sources). The output folder is only written by this tool's own Python.
    lay = layout(lay.out.parent, common.build_path(work), common.project_link())
    if lay.tools.parent != work:
        log.info(f"The work folder {work} and the project folder are reached through the links {lay.tools.parent} "
                 f"and {lay.repo} (their paths have characters some build tools can't take).")
    check_room(disc, lay, windows=IS_WINDOWS or o.windows, cache=True)   # [waw-decomp/speed1] always kept
    o.jobs = build_speed.jobs(o.jobs, log)   # [waw-decomp/speed1] 2026-11-23 no more jobs than the free memory holds
    # The build's log is also kept in <output>/bro/logs/build.log (the one before as build.previous.log).
    try:
        lay.logs.mkdir(parents=True, exist_ok=True)
        if (lay.logs / "build.log").exists():
            (lay.logs / "build.log").replace(lay.logs / "build.previous.log")
    except OSError as e:
        raise SetupError(f"Can't write to the output folder {lay.out} ({e.strerror or e}).") from None
    earlier, log.file = log.file, lay.logs / "build.log"
    try:
        return _build_steps(o, lay, work, disc, runner, log, cancel, step)
    finally:
        log.file = earlier


def _build_steps(o: BuildOptions, lay: Layout, work: Path, disc: Disc, runner: Runner, log: Log,
                 cancel: threading.Event, step: Callable[[int], None]) -> str:
    log.info(f"Setup tool {o.tool_version or '?'}, xboxrecomp {XBOXRECOMP_VERSION} ({XBOXRECOMP_COMMIT[:12]}), "
             f"{len(lay.patches)} patches, output {lay.out}, work {lay.work}")
    step(1)
    if IS_WINDOWS:
        # [acct2-ocr] Git, CMake and Ninja on PATH or pinned in tools/; the compiler and the rest: bro_windows
        host = bro_windows.find_host_tools(lay.tools, runner)
        found = Tools(git=host.git, cmake=host.cmake, ninja=host.ninja, glslc=host.glslc)
        APPROVAL.require(bro_windows.host_downloads(host, lay.tools) + downloads(lay, found, runner, False), log)
        host = bro_windows.ensure_host_tools(host, lay.tools, o.install_deps, runner, log, cancel)
        tools = Tools(git=host.git, cmake=host.cmake, ninja=host.ninja, glslc=host.glslc)
    else:
        found = find_tools(runner, lay.sdk_sdl3)
        APPROVAL.require(downloads(lay, found, runner, o.install_deps, o.windows), log)   # one list, before anything
        tools = ensure_tools(runner, log, o.install_deps, found, lay.sdk_sdl3)
        log.info("SDL3: " + (f"Call of Duty 3's SDK's ({tools.sdl3})" if tools.sdl3 else "the system's"))
    step(2)
    # default.xbe first; the rest of the disc is copied beside the recompile and the build (GameCopy).
    place_xbe(disc, lay)
    copier = GameCopy(disc, lay, log, cancel).start()
    try:
        step(3)
        ensure_xboxrecomp(lay, tools.git, runner, log)
        py = ensure_python(lay, runner, log)
        # [waw-decomp] 2026-11-16 the work copy is made again only when xboxrecomp or a patch changed (it holds the
        # steps' tables); which steps run is up to their keys
        if not (common.read_json(lay.source_stamp) == source_key(lay) and (lay.xboxrecomp / ".git").exists()):
            prepare_xboxrecomp(lay, tools.git, runner, log)
        keys = step_keys(lift_inputs(lay, py, disc.xbe_md5, runner), lay)
        step(4)
        # [waw-decomp/speed1] 2026-11-23 kept with cleanup on too (owner 21:00), in tools/ (LiftCache)
        cache = LiftCache(lay.lift_cache, log)
        if o.clear_lift_cache:
            cache.clear()
        drop_old_lift_cache(lay, log)
        done = restore_lift(lay, keys, disc.xbe_md5, cache, log)
        lay.gen.mkdir(parents=True, exist_ok=True)     # (bro/CMakeLists.txt wants it there to configure)
        early = _EarlyBuilds(o, lay, tools, log, cancel).start() if done < len(STEPS) else None
        try:
            ran = recompile(lay, py, disc.xbe_md5, runner, log, on_step=lambda what, frac: log.progress(frac, what),
                            keys=keys, keep_gen=not o.cleanup)
            keep_lift(cache, lay, disc.xbe_md5, keys, ran, recompile_key(lay, disc.xbe_md5), cancel, log)
            step(5)
            exe, exe_windows = _build_all(o, lay, tools, runner, log, cancel, early)
        finally:
            if early is not None:
                early.stop()
        copier.finish()
    except BaseException:
        copier.stop()
        raise
    step(6)
    install(lay, exe, disc, o.tool_version, exe_windows, log)
    if o.cleanup:
        common.safe_rmtree(lay.work, work, [lay.out, disc.path], log)
        log.info(f"Removed the build files in {lay.work}. xboxrecomp and Capstone stay in {lay.tools}.")
    else:   # [waw-decomp] 2026-11-16
        log.info(f"Kept the recompiler's output and the build in {lay.work} for the next build (cleanup is off).")
    log.progress(1.0, "Done")
    text = (f"{GAME_NAME} is in {lay.out}: Play starts it (or {lay.launcher.name} there).\n"
            f"Saves: {lay.saves}")
    if o.windows and not IS_WINDOWS:
        text += ("\nThe Windows version: bin/bro.exe and play.bat there (copy the whole bro folder to a Windows PC)."
                 if exe_windows else "\nThe Windows version wasn't made: see the log.")
    return text


def _build_windows(o: BuildOptions, lay: Layout, tools: Tools, runner: Runner, log: Log,
                   cancel: threading.Event, jobs: Optional[int] = None, keep: bool = False) -> Optional[Path]:
    """[acct2-ocr] bro.exe (bro_windows.build). On Linux it comes besides the Linux game: if it fails, the reason is
    logged and the Linux game is installed all the same (as Call of Duty 3's Windows version). [waw-decomp]
    2026-11-16: jobs (default o.jobs) and keep (build() has it)."""
    try:
        return bro_windows.build(lay, tools.git, tools.cmake, tools.ninja, tools.glslc, jobs or o.jobs, runner, log,
                                 cancel, line_filter=_progress_lines(log, "Compiling the Windows version", 0.0),
                                 keep=keep)
    except Cancelled:
        raise
    except Exception as e:  # noqa: BLE001 - downloads, unpacking, the compiler: all reported the same way
        if IS_WINDOWS:
            raise
        log.warn(f"The Windows version wasn't made: {e}")
        return None


# ---------------------------------------------------------------------------
# [waw-decomp] 2026-11-16 The processor's other cores while the recompiler runs, and both builds side by side
# ---------------------------------------------------------------------------
# The recompiler is one Python process: one core, and up to 3.6 GB of memory (rc-xbox, 11-16). common.recommended_jobs
# gives each compiler job 2 GB, so the builds beside it get two jobs fewer (a 16 GB PC: 8 jobs, 6 beside the lift).
LIFT_JOB_SLOTS = 2
# bro/CMakeLists.txt's target for everything the program links but the recompiled C.
LIBRARIES_TARGET = "bro_libraries"
# Linux and Windows side by side share the jobs, so together they use no more memory than one build did. GCC takes
# about twice clang's processor time on the generated C (rc-xbox, 11-16: 671 s and 320 s at -j8), so Linux gets two
# thirds and both end at about the same time. Below 3 jobs one after the other is as quick.
LINUX_JOB_SHARE = 2 / 3
SIDE_BY_SIDE_MIN_JOBS = 3


def _follow(cancel: threading.Event, stop: threading.Event) -> None:
    """Sets `stop` when `cancel` is set (the user's Stop), until `stop` is set: on a thread of its own."""
    def watch() -> None:
        while not stop.wait(0.5):
            if cancel.is_set():
                stop.set()
    threading.Thread(target=watch, name="bro-follow-cancel", daemon=True).start()


def _wait(thread: threading.Thread, cancel: threading.Event, stop: threading.Event) -> None:
    """Waits for `thread`; the user's Stop meanwhile stops it too."""
    while thread.is_alive():
        thread.join(0.5)
        if cancel.is_set():
            stop.set()


class _EarlyBuilds:
    """While the recompiler writes the C, what doesn't need it is built beside it, on a thread of its own with
    LIFT_JOB_SLOTS fewer jobs: the Windows version's compiler, sources, SDL3 (27 s, once) and import library, and
    both versions' configure and libraries (LIBRARIES_TARGET). What fails here is only noted: the build after the
    recompile does the same again, and says what went wrong there."""

    def __init__(self, o: BuildOptions, lay: Layout, tools: Tools, log: Log, cancel: threading.Event):
        self.o, self.lay, self.tools, self.log, self.cancel = o, lay, tools, log, cancel
        self.stopped = threading.Event()
        self.configured: set = set()      # "linux", "windows": build folders configured by this build
        self._thread = threading.Thread(target=self._run, name="bro-early-builds", daemon=True)

    def start(self) -> "_EarlyBuilds":
        _follow(self.cancel, self.stopped)
        self._thread.start()
        return self

    def _run(self) -> None:
        o, lay, log = self.o, self.lay, self.log
        runner = common.Runner(log, self.stopped)
        jobs = max(1, o.jobs - LIFT_JOB_SLOTS)
        log.info(f"Building what doesn't need the recompiled C beside the recompiler ({jobs} jobs).")
        t0 = time.monotonic()
        if IS_WINDOWS or o.windows:
            try:
                bro_windows.build(lay, self.tools.git, self.tools.cmake, self.tools.ninja, self.tools.glslc, jobs,
                                  runner, _NoBar(log), self.stopped, keep=not o.cleanup, target=LIBRARIES_TARGET)
                self.configured.add("windows")
            except Cancelled:
                return
            except Exception as e:  # noqa: BLE001 - the build after the recompile tries again and reports it
                log.info(f"(The Windows version's libraries weren't built beside the recompiler: {e})")
        if not IS_WINDOWS:
            try:
                build(lay, self.tools, jobs, runner, _NoBar(log), keep=not o.cleanup, target=LIBRARIES_TARGET)
                self.configured.add("linux")
            except Cancelled:
                return
            except Exception as e:  # noqa: BLE001 - as above
                log.info(f"(The libraries weren't built beside the recompiler: {e})")
        log.info(f"Built what doesn't need the recompiled C ({format_minutes(time.monotonic() - t0)}).")

    def wait(self) -> None:
        _wait(self._thread, self.cancel, self.stopped)

    def stop(self) -> None:
        self.stopped.set()
        self._thread.join(timeout=60)


def _build_all(o: BuildOptions, lay: Layout, tools: Tools, runner: Runner, log: Log, cancel: threading.Event,
               early: Optional[_EarlyBuilds]) -> Tuple[Optional[Path], Optional[Path]]:
    """The Linux program (not on Windows) and the Windows one (on Windows, or when asked): side by side when both
    are made and there are SIDE_BY_SIDE_MIN_JOBS jobs, else one after the other. Returns (Linux's, Windows')."""
    linux, windows = not IS_WINDOWS, IS_WINDOWS or o.windows
    configured = set()
    if early is not None:
        early.wait()        # its compilers work in the same build folders
        configured = early.configured
        runner.check_cancel()
    # With cleanup on, a build folder starts new in each build (as before), unless this build configured it already.
    keep_linux = not o.cleanup or "linux" in configured
    keep_windows = not o.cleanup or "windows" in configured
    if not (linux and windows and o.jobs >= SIDE_BY_SIDE_MIN_JOBS):
        exe = build(lay, tools, o.jobs, runner, log, keep=keep_linux) if linux else None
        exe_windows = _build_windows(o, lay, tools, runner, log, cancel, keep=keep_windows) if windows else None
        return exe, exe_windows
    linux_jobs = max(1, round(o.jobs * LINUX_JOB_SHARE))
    windows_jobs = max(1, o.jobs - linux_jobs)
    log.info(f"Building the Linux and Windows versions side by side ({linux_jobs} and {windows_jobs} jobs).")
    stop = threading.Event()
    result: Dict[str, object] = {}

    def windows_side() -> None:
        try:
            result["exe"] = _build_windows(o, lay, tools, common.Runner(log, stop), log, stop, jobs=windows_jobs,
                                           keep=keep_windows)
        except BaseException as e:  # noqa: BLE001 - handed to the build's own thread below
            result["error"] = e

    _follow(cancel, stop)
    side = threading.Thread(target=windows_side, name="bro-windows-build", daemon=True)
    side.start()
    try:
        exe = build(lay, tools, linux_jobs, runner, log, keep=keep_linux)
        _wait(side, cancel, stop)
    except BaseException:
        stop.set()
        side.join(timeout=60)
        raise
    finally:
        stop.set()      # (ends _follow's thread)
    runner.check_cancel()
    if "error" in result:
        raise result["error"]   # type: ignore[misc]
    return exe, result.get("exe")   # type: ignore[return-value]

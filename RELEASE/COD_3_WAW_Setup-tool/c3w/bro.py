"""Call of Duty 2: Big Red One (the original Xbox game) built for PC from your own disc: the setup tool's side of
the port in bro/ ([acct2-bro], owner 2026-11-06 07:31: "add a tab within the setup tool for that. Follow similar
steps to that of cod3").

The Big Red One tab (games.py) builds it in the same steps as Call of Duty 3's Set up page:

1. Check the disc: an Xbox disc image (.iso: redump or trimmed XISO) or a folder copied from the disc, whose
   default.xbe is the one the port was made from (the port's tables name addresses in that program).
2. Copy the disc's files into <output>/bro/game: the game reads them from there.
3. Fetch xboxrecomp (MIT; the recompiler for original Xbox programs) at the commit the port was made with into
   the work folder's tools/, and a Python with Capstone (the disassembler xboxrecomp uses). Both are listed for
   approval first, like every download.
4. Recompile default.xbe to C with xboxrecomp's own steps: its parser, the disassembly (seeded with
   bro/config/seed_functions.json), function IDs, ABI analysis, then the lift, with bro/config/
   stand_in_functions.tsv as the manual functions. About an hour, mostly the disassembly. The C is the game's
   code, so it stays in the work folder and is deleted with it.
5. Build bro/ with CMake against that C: graphics on ../ngl_vk; the window, sound and pads through SDL3, the static
   SDL3 Call of Duty 3's SDK builds when that SDK is in the same work folder (both games then use one SDL), else the
   system's own SDL3 (owner 2026-11-07 10:04: "Ok just use what cod3 has").
6. Install bin/bro, saves/, a launcher (play.sh) and bro.json in <output>/bro.

Linux only for now: the Windows build is the next step (Port status). Plain logic, no Tk, so the tests run
without Tkinter. The disc and default.xbe are only read; what runs is the port's own C.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from . import common, deps
from .common import IS_LINUX, IS_WINDOWS, APPROVAL, Install, Log, Runner, SetupError, human, is_within
from .xiso import XisoImage

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

# The default.xbe the port was made from (MD5): its tables (stand-ins, seeds) name addresses in this program.
KNOWN_XBE: Dict[str, str] = {
    "3b50372b121dc4e51b2b3dbda7cba19d": "Call of Duty 2: Big Red One, the disc the port was made from",
}

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

# The recompiler's steps: (key, what the log says, share of the build's time).
STEPS: Tuple[Tuple[str, str, float], ...] = (
    ("xbe_parser", "Reading default.xbe", 0.01),
    ("disasm", "Disassembling (the long step: about an hour)", 0.80),
    ("func_id", "Naming functions", 0.04),
    ("abi_analysis", "Working out how each function is called", 0.05),
    ("recomp", "Turning the program into C", 0.10),
)


def supported() -> Tuple[bool, str]:
    """(True, "") where the build works, else (False, why)."""
    if IS_LINUX:
        return True, ""
    return False, ("Big Red One builds on Linux for now; the Windows version is the next step (Port status). On "
                   "Windows, build it in a Linux installation, or wait for the next version of the tool.")


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
        return None, (f"This {GAME_NAME} isn't the version the port was made from (its default.xbe's MD5 is "
                      f"{disc.xbe_md5}), so its tables wouldn't fit. Please report it, with that MD5.")
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


def downloads(lay: Layout, t: Tools, runner: Runner, allow_install: bool) -> List[Install]:
    """Everything the build would download or install on this PC, for one approval list before anything starts:
    the packages (when some are missing and installing is on), xboxrecomp and Capstone (when not there yet)."""
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
    return items


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
    runner.run([git, "-C", src, "fetch", "--depth", "1", XBOXRECOMP_URL, XBOXRECOMP_COMMIT])
    runner.run([git, "-C", src, "checkout", "-q", "--force", "--detach", XBOXRECOMP_COMMIT])
    if _git_head(git, src, runner) != XBOXRECOMP_COMMIT:
        raise SetupError(f"xboxrecomp in {src} isn't at commit {XBOXRECOMP_COMMIT[:12]} after the download.")
    return src


def prepare_xboxrecomp(lay: Layout, git: Path, runner: Runner, log: Log) -> None:
    """A copy of the fetched source in the work folder (its steps write their tables into it), with the port's
    patches (bro/patches) applied."""
    if lay.xboxrecomp.exists():
        common.safe_rmtree(lay.xboxrecomp, lay.work, [lay.out], log)
    lay.stamp.unlink(missing_ok=True)   # the steps' tables were in the copy just removed
    lay.work.mkdir(parents=True, exist_ok=True)
    runner.run([git, "clone", "-q", "--no-checkout", lay.xboxrecomp_cache, lay.xboxrecomp])
    runner.run([git, "-C", lay.xboxrecomp, "checkout", "-q", "--detach", XBOXRECOMP_COMMIT])
    for patch in lay.patches:
        runner.run([git, "-C", lay.xboxrecomp, "apply", "--whitespace=nowarn", patch])
        log.info(f"Applied {patch.name}")


def ensure_python(lay: Layout, runner: Runner, log: Log) -> Path:
    """A Python that can import capstone (tools/bro-python, made once, after approval)."""
    py = lay.python
    if py.is_file() and runner.capture([py, "-c", "import capstone"])[0] == 0:
        return py
    APPROVAL.require([capstone_install(lay)], log)
    if not py.is_file():
        runner.run([sys.executable, "-m", "venv", lay.python_env])
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
    if disc.kind == "iso":
        with XisoImage(disc.path) as img:
            img.extract(lay.game, log, cancel)
    elif common.resolved(disc.path) != common.resolved(lay.game):
        if common.overlaps(disc.path, lay.game):
            raise SetupError(f"The disc folder {disc.path} and the game folder {lay.game} are inside each other: "
                             "pick the disc's folder, or another output folder.")
        _copy_folder(disc.path, lay.game, log, cancel)
    xbe = _folder_xbe(lay.game)
    if xbe is None:
        raise SetupError(f"default.xbe didn't arrive in {lay.game}.")
    lay.xbe.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(xbe, lay.xbe)
    if hashlib.md5(lay.xbe.read_bytes()).hexdigest() != disc.xbe_md5:
        raise SetupError("default.xbe changed while it was copied: build again.")


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


def check_room(disc: Disc, lay: Layout) -> None:
    """Enough free space for the game's files (output) and the build (work)."""
    have_game = sum(f.stat().st_size for f in _folder_files(lay.game)) if lay.game.is_dir() else 0
    needs = [(lay.out, max(0, disc.size - have_game)), (lay.work, WORK_ROOM)]
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
    """What the recompiled C depends on: when any of it changes, it is made again."""
    return {"xbe_md5": xbe_md5, "xboxrecomp": XBOXRECOMP_COMMIT,
            "patches": {p.name: _file_md5(p) for p in lay.patches},
            "seeds": _file_md5(lay.seeds), "stand_ins": _file_md5(lay.stand_ins)}


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
              on_step: Optional[Callable[[str, float], None]] = None) -> None:
    """default.xbe to C (lay.gen) with xboxrecomp's steps. Each finished step is noted in the stamp, so a build that
    stopped goes on from the step it stopped in."""
    key = recompile_key(lay, xbe_md5)
    stamp = common.read_json(lay.stamp)
    done = list(stamp.get("done", [])) if isinstance(stamp, dict) and stamp.get("key") == key else []
    if done and not (lay.xboxrecomp / ".git").exists():
        done = []   # the tables are in xboxrecomp's folder: gone with it
    shutil.copy2(lay.seeds, lay.work / "seed_functions.json")
    (lay.work / "manual_functions.json").write_text(json.dumps(manual_functions(lay.stand_ins)), encoding="utf-8")
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    env.pop("PYTHONPATH", None)
    start = 0.0
    for step, what, share in STEPS:
        if on_step:
            on_step(what, start)
        if step in done:
            log.info(f"{what}: done before, kept.")
            start += share
            continue
        if step == "recomp":
            for d in (lay.gen, lay.recomp_out):
                if d.exists():
                    common.safe_rmtree(d, lay.work, [lay.out], log)
                d.mkdir(parents=True)
        log.step(what)
        t0 = time.monotonic()
        with _StepClock(log, what, start, share) as clock:
            runner.run(step_command(step, lay, py), cwd=lay.xboxrecomp, env=env,
                       line_filter=_progress_lines(log, what, clock.where))
        log.info(f"{what}: {format_minutes(time.monotonic() - t0)}.")
        done.append(step)
        common.write_json(lay.stamp, {"key": key, "done": done})
        start += share
    if not any(lay.gen.glob("*.c")):
        raise SetupError(f"The recompiler wrote no C into {lay.gen}: see the log above.")


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


# About how long the recompile's steps take in all; STEPS' shares are of this time.
RECOMPILE_SECONDS = 3600


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
           f"-DBRO_GEN_DIR={lay.gen}", "-DCMAKE_BUILD_TYPE=Release", f"-DPython3_EXECUTABLE={sys.executable}"]
    if t.sdl3:
        cmd.append(f"-DSDL3_DIR={t.sdl3}")    # Call of Duty 3's SDK's static SDL3 (bro/CMakeLists.txt's find_package)
    if t.ninja:
        cmd[1:1] = ["-G", "Ninja"]
    return cmd


def build_command(lay: Layout, t: Tools, jobs: int) -> List[str]:
    return [str(t.cmake), "--build", str(lay.build), "-j", str(max(1, jobs))]


def build(lay: Layout, t: Tools, jobs: int, runner: Runner, log: Log) -> Path:
    """bro/ with CMake against the recompiled C; returns the program."""
    if lay.build.exists():
        common.safe_rmtree(lay.build, lay.work, [lay.out], log)   # a new configure each time: the C may be new
    runner.run(configure_command(lay, t))
    runner.run(build_command(lay, t, jobs), line_filter=_progress_lines(log, "Compiling", 0.0))
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


def install(lay: Layout, exe: Path, disc: Disc, tool_version: str) -> None:
    """The program into <output>/bro/bin, the launcher, saves/ and bro.json."""
    lay.exe.parent.mkdir(parents=True, exist_ok=True)
    tmp = lay.exe.with_name(lay.exe.name + ".new")
    shutil.copy2(exe, tmp)
    if not IS_WINDOWS:
        tmp.chmod(tmp.stat().st_mode | 0o755)
    tmp.replace(lay.exe)
    lay.saves.mkdir(parents=True, exist_ok=True)
    lay.logs.mkdir(parents=True, exist_ok=True)
    if not IS_WINDOWS:
        lay.launcher.write_text(LAUNCHER_SH, encoding="utf-8")
        lay.launcher.chmod(0o755)
    common.write_json(lay.manifest, {
        "game": GAME_NAME, "tool_version": tool_version, "built": time.strftime("%Y-%m-%d %H:%M"),
        "xbe_md5": disc.xbe_md5, "xboxrecomp": XBOXRECOMP_COMMIT,
        ("exe_windows" if IS_WINDOWS else "exe_linux"): lay.exe.relative_to(lay.out).as_posix(),
        "game_dir": "game", "saves_dir": "saves"})


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
                          "disassembly takes about an hour; leave the tool open until it says Done.")
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
    titles = ["Checking the disc", "Checking this PC", "Copying the game's files", "Getting xboxrecomp and Capstone",
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
    check_room(disc, lay)
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
    found = find_tools(runner, lay.sdk_sdl3)
    APPROVAL.require(downloads(lay, found, runner, o.install_deps), log)   # one list, before anything is fetched
    tools = ensure_tools(runner, log, o.install_deps, found, lay.sdk_sdl3)
    log.info("SDL3: " + (f"Call of Duty 3's SDK's ({tools.sdl3})" if tools.sdl3 else "the system's"))
    step(2)
    copy_game(disc, lay, log, cancel)
    step(3)
    ensure_xboxrecomp(lay, tools.git, runner, log)
    py = ensure_python(lay, runner, log)
    stamp = common.read_json(lay.stamp)
    if not (isinstance(stamp, dict) and stamp.get("key") == recompile_key(lay, disc.xbe_md5)
            and (lay.xboxrecomp / ".git").exists()):
        prepare_xboxrecomp(lay, tools.git, runner, log)
    step(4)
    recompile(lay, py, disc.xbe_md5, runner, log,
              on_step=lambda what, frac: log.progress(frac, what))
    step(5)
    exe = build(lay, tools, o.jobs, runner, log)
    step(6)
    install(lay, exe, disc, o.tool_version)
    if o.cleanup:
        common.safe_rmtree(lay.work, work, [lay.out, disc.path], log)
        log.info(f"Removed the build files in {lay.work}. xboxrecomp and Capstone stay in {lay.tools}.")
    log.progress(1.0, "Done")
    return (f"{GAME_NAME} is in {lay.out}: Play starts it (or {lay.launcher.name} there).\n"
            f"Saves: {lay.saves}")

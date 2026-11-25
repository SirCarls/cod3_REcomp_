"""Call of Duty: Finest Hour (the original Xbox game) built for PC from your own disc: the setup tool's side of the
port in fh/ ([acct2-decomp] 2026-11-21, owner 11:18: "add finest hour to the tabs"; asked how much the tab should
do, he chose a full tab like Big Red One's). fh/ and tools/fh_decomp/ are acct2-fh's: this module only reads them
and follows fh/README.md's Build and Run, and dev/agents/acct2-fh/tools/fh_gen3.sh, the script the port is built
with.

The Finest Hour tab (games.py) builds it in Big Red One's steps (c3w/bro.py, whose general parts are used here):

1. Check the disc: an Xbox disc image (.iso: redump or trimmed XISO) or a folder copied from the disc, whose
   default.xbe is in fh/config/discs.tsv with its level loader's back edge (below). Today that is the Europe disc
   (English, French, Spanish, Italian; version 3), the port's base. The USA disc (version 1) is listed without one:
   its loader is elsewhere, so it is named and turned down until the port has its address.
2. Copy the disc's files into <output>/fh/game: default.xbe first, the rest beside the next steps (bro.GameCopy).
3. xboxrecomp at Big Red One's commit (the same download, kept in the work folder's tools/) and Capstone. The work
   copy gets bro/patches/xboxrecomp-lazy-decode.patch (the recompiler's speed; the same C), fh/patches/*.patch
   (the game's paths found whatever their case) and fh/patches' three changes to xboxrecomp's kernel, in
   fh_gen3.sh's order: thread priorities under the guest CPU lock, CreateSuspended honoured, the console's language.
4. Recompile default.xbe with xboxrecomp's steps: its parser, the disassembly, function IDs, ABI analysis, then the
   lift, with fh/config/stand_in_functions.tsv as the manual functions and --back-edge-yield: the game's level
   loader waits in a loop with no kernel call in it, and under the guest CPU lock only a back edge lets another
   thread run. The C must have the loader's back edge from discs.tsv (fh_gen3.sh's check). Each step is done again
   only when something it reads changed (bro.py's stamp), so a build that stopped goes on.
5. Build fh/ with CMake: graphics on ../ngl_vk, the window, sound and pads through SDL3. SDL3, FFmpeg's source (the
   films) and Dear ImGui (the F2 window) are Call of Duty 3's SDK's when it is in the same work folder; without the
   SDK the system's SDL3 is used and the films and the F2 window are left out (the log says so).
6. Install bin/fh, fh.toml (the port's settings, next to bin/; a player's own is kept), saves/, logs/, a launcher
   (play.sh) and fh.json in <output>/fh.
7. With "Make HD textures" on, the HD texture pack from the disc's own textures (c3w/disc_hd.py, [setup-layout]
   2026-11-24): `rip fh-hd-textures` reads COD.PAK, waifu2x upscales the pictures into <output>/fh/hd. The Play
   page's "HD textures" starts the game with FH_HD_TEXTURES at that folder.

Linux for now (owner 11:18's choice: Linux first, then Windows). Plain logic, no Tk, so the tests run without
Tkinter. The disc and default.xbe are only read; what runs is the port's own C.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

from . import bro, bro_windows, common, controller_db, disc_hd, upscale
from .common import IS_LINUX, IS_WINDOWS, APPROVAL, Log, Runner, SetupError, human
from .xiso import XisoImage
from . import build_speed   # [waw-decomp/speed1] 2026-11-23 jobs, linker, compiler cache and link pool for this PC

GAME_NAME = "Call of Duty: Finest Hour"
OUTPUT_SUBDIR = "fh"           # <output>/fh: the game, its settings, saves and launcher
WORK_SUBDIR = "fh"             # <work>/fh: the recompiled C and the build
MANIFEST = "fh.json"
EXE = "fh"

# The changes fh_gen3.sh makes to xboxrecomp's kernel, in its order (each a script: python3 SCRIPT XBOXRECOMP_DIR,
# which stops without changing anything when the checkout isn't the one it was written against).
PY_PATCHES = ("xboxrecomp_guest_priority.py", "xboxrecomp_create_suspended.py", "xboxrecomp_console_language.py")
# Big Red One's patches the Finest Hour lift is made with (fh/README.md: "with its lazy-decode patch").
BRO_PATCHES = ("xboxrecomp-lazy-decode.patch",)

# The recompiler's steps: (key, what the log says, share of the build's time), Big Red One's.
STEPS: Tuple[Tuple[str, str, float], ...] = (
    ("xbe_parser", "Reading default.xbe", 0.01),
    ("disasm", "Disassembling (the long step: several minutes)", 0.50),
    ("func_id", "Naming functions", 0.02),
    ("abi_analysis", "Working out how each function is called", 0.01),
    ("recomp", "Turning the program into C", 0.46),
)

WINDOW_SIZES = ("", "1280x960", "1600x1200", "1920x1080", "2560x1440", "3840x2160")
# fh.toml's game.language (FH_LANGUAGE; fh/src/main.c): the languages the Europe disc has. "" = fh.toml's.
LANGUAGES: Tuple[Tuple[str, str], ...] = (("", "As in fh.toml"), ("english", "English"), ("french", "French"),
                                          ("spanish", "Spanish"), ("italian", "Italian"))
# Room besides the game's files: the recompiler's tables and the C (about 0.6 GB), the build (about 0.4 GB).
WORK_ROOM = 1_500_000_000


def supported() -> Tuple[bool, str]:
    """(True, "") where the build works, else (False, why). Linux for now."""
    if IS_LINUX:
        return True, ""
    return False, "Finest Hour builds on Linux for now; its Windows build comes next."


# ---------------------------------------------------------------------------
# The discs (fh/config/discs.tsv: xbe_md5, name, loader_back_edge, notes)
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class DiscRow:
    xbe_md5: str
    name: str
    loader_back_edge: str    # "0x000691DB", or "" when the port has no address for this disc's loader yet
    notes: str


def project_root() -> Path:
    """The project folder: the one holding fh/, bro/ and ngl_vk/ next to the setup tool."""
    return Path(__file__).resolve().parent.parent.parent


def read_discs(path: Optional[Path] = None) -> Dict[str, DiscRow]:
    """The discs the port knows, by their default.xbe's MD5 ({} when the table isn't there)."""
    path = Path(path) if path else project_root() / "fh" / "config" / "discs.tsv"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return {}
    rows: Dict[str, DiscRow] = {}
    for i, line in enumerate(lines):
        cells = line.split("\t")
        md5 = cells[0].strip().lower()
        if i == 0 or not md5 or md5.startswith("#") or not re.fullmatch(r"[0-9a-f]{32}", md5):
            continue
        cells += [""] * (4 - len(cells))
        edge = cells[2].strip()
        if edge and not re.fullmatch(r"0x[0-9A-Fa-f]{1,8}", edge):
            edge = ""   # (not an address: the disc can't be built until it has one)
        rows[md5] = DiscRow(md5, cells[1].strip(), edge, cells[3].strip())
    return rows


def back_edge_mark(edge: str) -> str:
    """The text the lift writes at the loader's back edge (fh_gen3.sh's LOADER_EDGE): RECOMP_BACK_EDGE(0x000691DBu)."""
    return f"RECOMP_BACK_EDGE(0x{int(edge, 16):08X}u)"


@dataclass(frozen=True)
class Disc:
    kind: str           # "iso" or "folder"
    path: Path          # the image, or the folder holding default.xbe
    xbe_md5: str
    info: "bro.XbeInfo"
    files: int
    size: int           # bytes of game files
    row: Optional[DiscRow]


def open_disc(text: str, discs: Optional[Dict[str, DiscRow]] = None) -> Disc:
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
    known = read_discs() if discs is None else discs
    if path.is_dir():
        xbe = bro._folder_xbe(path)
        if xbe is None:
            raise SetupError(f"{path.name} has no default.xbe: pick the folder that holds the disc's files.")
        data = xbe.read_bytes()
        files = bro._folder_files(path)
        md5 = hashlib.md5(data).hexdigest()
        return Disc("folder", path, md5, bro.xbe_info(data[:65536]), len(files),
                    sum(f.stat().st_size for f in files), known.get(md5))
    try:
        with XisoImage(path) as img:
            if img.find("default.xbe") is None:
                raise SetupError(f"{path.name} is a disc image without default.xbe: not an original Xbox game.")
            data = img.read("default.xbe")
            md5 = hashlib.md5(data).hexdigest()
            return Disc("iso", path, md5, bro.xbe_info(data[:65536]), len(img.files), img.total_size,
                        known.get(md5))
    except OSError as e:
        raise SetupError(f"Couldn't read {path.name}: {e.strerror or e}") from None


def check_disc(text: str, discs: Optional[Dict[str, DiscRow]] = None) -> Tuple[Optional[Disc], str]:
    """(the disc, a line for the Set up page) when it can be built; (None, why) when not."""
    try:
        disc = open_disc(text, discs)
    except SetupError as e:
        return None, str(e)
    if disc.row is None:
        if "finest hour" not in disc.info.title.lower():
            return None, f"This is {disc.info.title or 'another game'}, not {GAME_NAME}."
        return None, (f"This {GAME_NAME} disc isn't one the port knows (its default.xbe's MD5 is {disc.xbe_md5}), "
                      "so the port's tables may not fit it. Please report it, with that MD5.")
    if not disc.row.loader_back_edge:
        return None, (f"This is {GAME_NAME}, {disc.row.name}: the port doesn't build from this disc yet (its level "
                      "loader is at another address, which the port doesn't have yet). The Europe disc (English, "
                      "French, Spanish, Italian) builds.")
    what = "disc image" if disc.kind == "iso" else "folder"
    return disc, f"Found: {GAME_NAME}, {disc.row.name} ({what}, {disc.files} files, {human(disc.size)})."


# ---------------------------------------------------------------------------
# Where things go: Big Red One's layout with Finest Hour's folders and files
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Layout(bro.Layout):
    """bro.Layout's folders for Finest Hour: <output>/fh, <work>/fh; tools/ (xboxrecomp's download, Capstone's
    Python) is shared with Big Red One, as both use the same ones."""

    @property
    def exe(self) -> Path:
        return self.out / "bin" / common.exe_name(EXE)

    @property
    def settings(self) -> Path:
        """fh.toml: the port reads it from the folder bin/ is in (fh/src/fh_settings.c)."""
        return self.out / "fh.toml"

    @property
    def manifest(self) -> Path:
        return self.out / MANIFEST

    @property
    def bro_src(self) -> Path:   # (bro.Layout's name for the port's folder in the project)
        return self.repo / "fh"

    @property
    def fh_src(self) -> Path:
        return self.repo / "fh"

    @property
    def patches(self) -> List[Path]:
        own = sorted((self.fh_src / "patches").glob("*.patch"), key=lambda p: p.name)
        return [self.repo / "bro" / "patches" / n for n in BRO_PATCHES] + own

    @property
    def py_patches(self) -> List[Path]:
        return [self.fh_src / "patches" / n for n in PY_PATCHES]

    @property
    def stand_ins(self) -> Path:
        return self.fh_src / "config" / "stand_in_functions.tsv"

    @property
    def discs(self) -> Path:
        return self.fh_src / "config" / "discs.tsv"

    @property
    def default_settings(self) -> Path:
        return self.fh_src / "config" / "fh.toml"

    @property
    def sdk(self) -> Path:
        """Call of Duty 3's SDK in the same work folder (pipeline.py's), when it has been fetched there."""
        return self.tools.parent / "rexglue-sdk"


def layout(output: Path, work: Path, repo: Optional[Path] = None) -> Layout:
    return Layout(Path(repo or project_root()), Path(output) / OUTPUT_SUBDIR, Path(work) / WORK_SUBDIR,
                  Path(work) / "tools")


def check_project(lay: Layout) -> None:
    """The port's sources are next to the setup tool (a copy with only the tool has none)."""
    need = [lay.fh_src / "CMakeLists.txt", lay.stand_ins, lay.discs, lay.default_settings,
            lay.repo / "ngl_vk" / "CMakeLists.txt", lay.repo / "tools" / "fh_decomp" / "fh_stubs.py",
            *lay.patches, *lay.py_patches]
    missing = [str(p.relative_to(lay.repo)) for p in need if not p.is_file()]
    if missing:
        raise SetupError("The Finest Hour port's sources aren't in this copy of the project (missing: "
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
# What this PC needs: Big Red One's (the same packages: bro.LINUX_PACKAGES), said for Finest Hour
# ---------------------------------------------------------------------------
def ensure_tools(runner: Runner, log: Log, allow_install: bool, found: Optional["bro.Tools"] = None,
                 sdk_sdl3: Optional[Path] = None) -> "bro.Tools":
    t = found or bro.find_tools(runner, sdk_sdl3)
    if t.problems() and allow_install:
        log.info("Finest Hour's build needs: " + "; ".join(t.problems()))
        bro.install_packages(runner, log)
        t = bro.find_tools(runner, sdk_sdl3)
    if t.problems():
        raise SetupError("Finest Hour's build needs: " + "; ".join(t.problems()) + ". Turn on installing what's "
                         "missing (Set up), or install them yourself.")
    return t


# ---------------------------------------------------------------------------
# xboxrecomp's work copy, with Finest Hour's changes
# ---------------------------------------------------------------------------
def source_key(lay: Layout) -> dict:
    """What the work copy of xboxrecomp is made from: the commit, the patches and the kernel changes."""
    return {**bro.source_key(lay), "py_patches": {p.name: bro._file_md5(p) for p in lay.py_patches}}


def prepare_xboxrecomp(lay: Layout, git: Path, runner: Runner, log: Log) -> None:
    """bro.prepare_xboxrecomp's copy (with lay.patches applied), then the kernel changes (fh_gen3.sh's order)."""
    bro.prepare_xboxrecomp(lay, git, runner, log)
    lay.source_stamp.unlink(missing_ok=True)   # written again once the changes are in
    for script in lay.py_patches:
        runner.run([bro_windows.console_python(), "-I", "-B", str(script), str(lay.xboxrecomp)])
        log.info(f"Applied {script.name}")
    common.write_json(lay.source_stamp, source_key(lay))


# ---------------------------------------------------------------------------
# The recompile
# ---------------------------------------------------------------------------
def step_command(key: str, lay: Layout, py: Path) -> List[str]:
    """One of xboxrecomp's steps, run from its folder (fh/README.md's Build; fh_gen3.sh's lift)."""
    xbe = str(lay.xbe)
    if key == "xbe_parser":
        return [str(py), "-m", "tools.xbe_parser", xbe, "--json", str(lay.analysis_json)]
    if key == "disasm":
        return [str(py), "-m", "tools.disasm", xbe, "-v"]
    if key in ("func_id", "abi_analysis"):
        return [str(py), "-m", f"tools.{key}", xbe, "-v"]
    if key == "recomp":
        return [str(py), "-m", "tools.recomp", xbe, "--game-name", "Finest Hour", "--all", "--split", "250",
                "--gen-dir", str(lay.gen), "-o", str(lay.recomp_out),
                "--manual-functions", str(lay.work / "manual_functions.json"), "--back-edge-yield", "-v"]
    raise ValueError(key)


def step_keys(lay: Layout, py: Path, xbe_md5: str, runner: Runner) -> Dict[str, str]:
    """Each step's key: what it reads (bro.LiftInputs' list, without seeds) and the key of the step before it."""
    rc, out = runner.capture([py, "-c", "import sys, capstone; print(sys.version.split()[0]); "
                                        "print(capstone.__version__)"])
    words = out.split()
    if rc != 0 or len(words) < 2:
        raise SetupError(f"The recompiler's Python ({py}) didn't say its version and Capstone's: {out.strip()[:300]}")
    tools = bro.tree_sha256(lay.xboxrecomp / "tools")
    header = bro._sha256(lay.xboxrecomp / "templates" / "runtime" / bro.RUNTIME_HEADER)
    stand_ins = sorted({bro._address(a) for a in bro.manual_functions(lay.stand_ins)})
    keys: Dict[str, str] = {}
    before = ""
    for step, _, _ in STEPS:
        options = [a.replace(str(lay.work), "<work>") for a in step_command(step, lay, Path("python"))[1:]]
        parts = {"step": step, "after": before, "xbe_md5": xbe_md5, "tools": tools, "python": words[0],
                 "capstone": words[1], "options": options}
        if step == "recomp":
            parts["stand_ins"] = stand_ins
            parts["runtime_header"] = header
        keys[step] = before = bro._sha256_text(json.dumps(parts, sort_keys=True))
    return keys


def recompile(lay: Layout, py: Path, keys: Dict[str, str], back_edge: str, runner: Runner, log: Log,
              keep_gen: bool = False) -> List[str]:
    """default.xbe to C (lay.gen) with xboxrecomp's steps; each finished step is noted in the stamp (bro.py's), so
    a build that stopped goes on from the step it stopped in. Returns the steps it ran."""
    done = bro.read_stamp(lay)
    (lay.work / "manual_functions.json").write_text(json.dumps(bro.manual_functions(lay.stand_ins)),
                                                    encoding="utf-8")
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    env.pop("PYTHONPATH", None)
    start = 0.0
    names = [s for s, _, _ in STEPS]
    ran: List[str] = []
    for i, (step, what, share) in enumerate(STEPS):
        log.progress(start, what)
        if done.get(step) == keys[step] and bro._outputs_there(step, lay):
            log.info(f"{what}: done before, kept.")
            start += share
            continue
        for later in names[i:]:   # this step's output is about to change: it and the later ones aren't done
            done.pop(later, None)
        bro.write_stamp(lay, done)
        if step == "recomp":
            if lay.recomp_out.exists():
                common.safe_rmtree(lay.recomp_out, lay.work, [lay.out], log)
            if lay.gen.exists() and not keep_gen:
                common.safe_rmtree(lay.gen, lay.work, [lay.out], log)
            for d in (lay.gen, lay.recomp_out):
                d.mkdir(parents=True, exist_ok=True)
        log.step(what)
        t0 = time.monotonic()
        with bro._StepClock(log, what, start, share) as clock:
            runner.run(step_command(step, lay, py), cwd=lay.xboxrecomp, env=env,
                       line_filter=bro._progress_lines(log, what, clock.where))
        log.info(f"{what}: {bro.format_minutes(time.monotonic() - t0)}.")
        if step == "recomp":
            bro.remove_stale_c(lay, log)
        done[step] = keys[step]
        bro.write_stamp(lay, done)
        ran.append(step)
        start += share
    check_back_edge(lay, back_edge)
    return ran


def check_back_edge(lay: Layout, edge: str) -> int:
    """The level loader's back edge is in the C (fh_gen3.sh's check): without it the game stops at the first level
    load under the guest CPU lock. Returns how many back edges the C has in all."""
    files = sorted(lay.gen.glob("recomp_0*.c"))
    if not files:
        raise SetupError(f"The recompiler wrote no C into {lay.gen}: see the log above.")
    mark, edges, found = back_edge_mark(edge), 0, False
    for f in files:
        text = f.read_text(encoding="utf-8", errors="replace")
        edges += text.count("RECOMP_BACK_EDGE(0x")
        found = found or mark in text
    if not found:
        raise SetupError(f"The recompiled C has no {mark}: the level loader's wait loop has no back edge, so the "
                         "game would stop at its first level. Please report it.")
    return edges


# ---------------------------------------------------------------------------
# The build
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class SdkParts:
    """What of Call of Duty 3's SDK the build can use (None: not there)."""
    sdl3: Optional[Path]      # its static SDL3's CMake package
    ffmpeg: Optional[Path]    # FFmpeg 4.4's source (the films; fh/cmake/fh_ffmpeg.cmake builds it)
    imgui: Optional[Path]     # Dear ImGui (the F2 window)


def sdk_parts(lay: Layout) -> SdkParts:
    sdl3 = Path(lay.sdk_sdl3)
    ffmpeg = lay.sdk / "thirdparty" / "FFmpeg"
    imgui = lay.sdk / "thirdparty" / "imgui"
    return SdkParts(sdl3 if (sdl3 / "SDL3Config.cmake").is_file() else None,
                    ffmpeg if (ffmpeg / "configure").is_file() else None,
                    imgui if (imgui / "imgui.h").is_file() else None)


# Ninja's "[done/total]" and Make's "[ 45%]" at the start of a build line.
_NINJA_COUNT = re.compile(r"^\[(\d+)/(\d+)\]")
_MAKE_PERCENT = re.compile(r"^\[\s*(\d+)%\]")


def compile_lines(log: Log) -> Callable[[str], None]:
    """The compiler's lines as the status line, the bar at how far the build says it has got. [acct2-decomp]
    2026-11-21: with a fixed 0.0 (as bro.build has it) the bar stood at 0% for the whole compile, 333 files and
    several minutes (fhtest), which looks like a frozen tool."""
    where = [0.0]
    show = bro._progress_lines(log, "Compiling", where)

    def handle(line: str) -> None:
        text = line.strip()
        count = _NINJA_COUNT.match(text)
        percent = _MAKE_PERCENT.match(text)
        if count and int(count.group(2)) > 0:
            where[0] = min(1.0, int(count.group(1)) / int(count.group(2)))
        elif percent:
            where[0] = min(1.0, int(percent.group(1)) / 100.0)
        show(line)
    return handle


def configure_command(lay: Layout, t: "bro.Tools", parts: SdkParts) -> List[str]:
    cmd = [str(t.cmake), "-S", str(lay.fh_src), "-B", str(lay.build), f"-DXBOXRECOMP_DIR={lay.xboxrecomp}",
           f"-DFH_GEN_DIR={lay.gen}", "-DCMAKE_BUILD_TYPE=Release",
           f"-DPython3_EXECUTABLE={bro_windows.console_python()}"]
    if parts.sdl3:
        cmd.append(f"-DSDL3_DIR={parts.sdl3}")   # (fh/CMakeLists.txt's find_package(SDL3 CONFIG))
    if parts.ffmpeg:
        cmd.append(f"-DFH_FFMPEG_SOURCE_DIR={parts.ffmpeg}")
    if parts.imgui:
        cmd.append(f"-DFH_IMGUI_DIR={parts.imgui}")
    if t.ninja:
        cmd[1:1] = ["-G", "Ninja"]
    return cmd


def build(lay: Layout, t: "bro.Tools", parts: SdkParts, jobs: int, runner: Runner, log: Log,
          keep: bool = False) -> Path:
    """fh/ with CMake against the recompiled C; returns the program. keep (cleanup off): the build folder of the
    last build is used again when it was configured the same way (bro_windows.configure_folder)."""
    # [waw-decomp/speed1] 2026-11-23: speed for this PC (c3w/build_speed.py); a folder not kept starts new
    jobs = build_speed.jobs(jobs, log)
    speed = build_speed.cmake_args(lay.build, bro.host_compilers(), ninja=bool(t.ninja),
                                   fresh=True if not keep else None, log=log)
    bro_windows.configure_folder(configure_command(lay, t, parts) + speed, lay.build, keep, lay, runner, log)
    runner.run([str(t.cmake), "--build", str(lay.build), "-j", str(max(1, jobs))],
               line_filter=compile_lines(log))
    exe = lay.build / common.exe_name(EXE)
    if not exe.is_file():
        raise SetupError(f"The build finished without {exe.name} in {lay.build}: see the log above.")
    cache = lay.build / "CMakeCache.txt"
    text = cache.read_text(encoding="utf-8", errors="replace") if cache.is_file() else ""
    if "FH_GLSLC:FILEPATH=FH_GLSLC-NOTFOUND" in text or "Vulkan_INCLUDE_DIR:PATH=Vulkan_INCLUDE_DIR-NOTFOUND" in text:
        raise SetupError("The build found no Vulkan or no glslc, so the game couldn't draw anything: install them "
                         "(Vulkan development files and glslc) and build again.")
    if not parts.sdl3 and "SDL3_DIR:PATH=SDL3_DIR-NOTFOUND" in text:
        raise SetupError("The build found no SDL3, so the game would have no window, sound or pads: install SDL3's "
                         "development files, or build Call of Duty 3 first with the same work folder, and build again.")
    return exe


# ---------------------------------------------------------------------------
# Installing and starting
# ---------------------------------------------------------------------------
# RECOMP_GUEST_LOCK=1: one guest thread runs at a time, as on the console's one CPU (fh/README.md: "Run with
# RECOMP_GUEST_LOCK=1"; the game's loading and title threads share RenderWare's lists with no lock).
LAUNCHER_SH = """#!/bin/sh
# Call of Duty: Finest Hour, built by the COD3 setup tool from your own disc. Starts the game with its files in game/
# and its saves in saves/; its settings are in fh.toml here (the F2 window changes them too). The setup tool's Play
# page has the same start with options.
here=$(cd "$(dirname "$0")" && pwd)
RECOMP_GUEST_LOCK=${RECOMP_GUEST_LOCK:-1}
export RECOMP_GUEST_LOCK
mkdir -p "$here/saves" "$here/logs"
exec "$here/bin/fh" "$here/game" "$here/saves" "$@"
"""


def install(lay: Layout, exe: Path, disc: Disc, tool_version: str, log: Optional[Log] = None) -> None:
    """The program into <output>/fh/bin, fh.toml beside bin/ (the port's defaults the first time; a player's own
    is kept), the launcher (play.sh), saves/, logs/ and fh.json; [input-port] the controller database beside it."""
    bro._place(exe, lay.exe, True)
    # [input-port] 2026-11-24: fh_pad.c reads gamecontrollerdb.txt next to the program (SDL_GameControllerDB)
    controller_db.copy_next_to(lay.exe.parent, log.warn if log is not None else None)
    lay.saves.mkdir(parents=True, exist_ok=True)
    lay.logs.mkdir(parents=True, exist_ok=True)
    if not lay.settings.is_file():
        tmp = lay.settings.with_name(lay.settings.name + ".new")
        shutil.copyfile(lay.default_settings, tmp)
        tmp.replace(lay.settings)
    lay.launcher.write_text(LAUNCHER_SH, encoding="utf-8")
    lay.launcher.chmod(0o755)
    common.write_json(lay.manifest, {
        "game": GAME_NAME, "tool_version": tool_version, "built": time.strftime("%Y-%m-%d %H:%M"),
        "xbe_md5": disc.xbe_md5, "disc": disc.row.name if disc.row else "", "xboxrecomp": bro.XBOXRECOMP_COMMIT,
        "exe_linux": lay.exe.relative_to(lay.out).as_posix(), "game_dir": "game", "saves_dir": "saves",
        "settings": lay.settings.name})


@dataclass
class PlayOptions:
    """The Play page's choices: the port's own settings, passed as the variables that win over fh.toml."""
    fullscreen: bool = False
    vsync: bool = True
    size: str = ""            # "1280x960"; "" = fh.toml's
    language: str = ""        # LANGUAGES; "" = fh.toml's
    skip_films: bool = False
    hd_textures: bool = True  # the HD pack (disc_hd.py), when a build made one

    def problems(self) -> List[str]:
        out = []
        if self.size and not re.fullmatch(r"\d{2,5}x\d{2,5}", self.size):
            out.append(f"Window size: {self.size} isn't WIDTHxHEIGHT")
        if self.language not in {key for key, _ in LANGUAGES}:
            out.append(f"Unknown language: {self.language}")
        return out

    def to_settings(self) -> dict:
        return {f"fh_{k}": v for k, v in asdict(self).items()}

    @classmethod
    def from_settings(cls, s: dict) -> "PlayOptions":
        """The choices saved in settings `s`; a value of the wrong type or out of range is left at its default."""
        o = cls()
        for k, default in asdict(cls()).items():
            v = s.get(f"fh_{k}")
            if type(v) is type(default):   # (a bool isn't taken for a number, nor a number for a bool)
                setattr(o, k, v)
        if o.size and not re.fullmatch(r"\d{2,5}x\d{2,5}", o.size):
            o.size = ""
        if o.language not in {key for key, _ in LANGUAGES}:
            o.language = ""
        return o


PLAY_VARIABLES = ("RECOMP_GUEST_LOCK", "FH_FULLSCREEN", "FH_VSYNC", "FH_SIZE", "FH_LANGUAGE", "FH_SKIP_FILMS",
                  disc_hd.GAMES["fh"].variable)


def play_env(opts: PlayOptions, base: Optional[Dict[str, str]] = None, game_out: Optional[Path] = None
             ) -> Dict[str, str]:
    """The variables a start sets; `game_out` (<output>/fh) is where the HD pack is looked for."""
    env = dict(os.environ if base is None else base)
    for name in PLAY_VARIABLES:
        env.pop(name, None)
    env["RECOMP_GUEST_LOCK"] = "1"
    if opts.fullscreen:
        env["FH_FULLSCREEN"] = "1"
    if not opts.vsync:
        env["FH_VSYNC"] = "0"
    if opts.size:
        env["FH_SIZE"] = opts.size
    if opts.language:
        env["FH_LANGUAGE"] = opts.language
    if opts.skip_films:
        env["FH_SKIP_FILMS"] = "1"
    env.update(disc_hd.play_variables("fh", game_out, opts.hd_textures))
    return env


def last_build(output: Path) -> str:
    """How the last build into an output folder ended (bro.last_build's reading of <output>/fh/logs/build.log)."""
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
            return None, f"Not built yet. {ended} Press Build again: it goes on from where it stopped."
        return None, "Not built yet: choose your disc on Finest Hour's Set up page and press Build."
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
    # [setup-layout] 2026-11-24 the HD texture pack (disc_hd.py) and waifu2x's settings (the App's, as the other
    # games use them).
    hd_build: bool = False
    upscale_settings: upscale.Settings = field(default_factory=upscale.Settings)


TITLES = ("Checking the disc", "Checking this PC", "Copying the game's files (beside the next steps)",
          "Getting xboxrecomp and Capstone", "Recompiling default.xbe", "Building", "Installing")


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

    def step(i: int) -> None:
        runner.check_cancel()
        log.step(TITLES[i])
        if on_step:
            on_step(i, len(TITLES), TITLES[i])

    step(0)
    check_project(lay)
    disc, line = check_disc(o.source, read_discs(lay.discs))
    if disc is None:
        raise SetupError(line)
    log.info(line)
    if common.overlaps(lay.work, lay.out):
        raise SetupError(f"The work folder {work} and the output folder {lay.out.parent} are inside each other: "
                         "pick two separate folders.")
    common.claim_work_dir(work)
    # The work and project folders through plain-named links when their paths have characters build tools can't
    # take (bro.run_build's reason).
    lay = layout(lay.out.parent, common.build_path(work), common.project_link())
    check_room(disc, lay)
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


def check_room(disc: Disc, lay: Layout) -> None:
    """Enough free space for the game's files (output) and the build (work)."""
    have = sum(f.stat().st_size for f in bro._folder_files(lay.game)) if lay.game.is_dir() else 0
    # [waw-decomp/speed1] 2026-11-23 and the lift cache's entry (bro.LiftCache)
    for folder, need in ((lay.out, max(0, disc.size - have)), (lay.work, WORK_ROOM + bro.LIFT_CACHE_ROOM)):
        probe = folder
        while not probe.exists() and probe != probe.parent:
            probe = probe.parent
        try:
            free = shutil.disk_usage(probe).free
        except OSError:
            continue
        if free < need:
            raise SetupError(f"Not enough room in {probe}: the build needs {human(need)} there, {human(free)} is free.")


def _build_steps(o: BuildOptions, lay: Layout, work: Path, disc: Disc, runner: Runner, log: Log,
                 cancel: threading.Event, step: Callable[[int], None]) -> str:
    log.info(f"Setup tool {o.tool_version or '?'}, xboxrecomp {bro.XBOXRECOMP_VERSION} "
             f"({bro.XBOXRECOMP_COMMIT[:12]}), {len(lay.patches)} patches and {len(lay.py_patches)} kernel changes, "
             f"output {lay.out}, work {lay.work}")
    step(1)
    found = bro.find_tools(runner, lay.sdk_sdl3)
    hd_downloads = disc_hd.downloads(lay.tools) if o.hd_build else []
    APPROVAL.require(bro.downloads(lay, found, runner, o.install_deps) + hd_downloads, log)   # one list, first
    tools = ensure_tools(runner, log, o.install_deps, found, lay.sdk_sdl3)
    parts = sdk_parts(lay)
    log.info("SDL3: " + (f"Call of Duty 3's SDK's ({parts.sdl3})" if parts.sdl3 else "the system's") +
             "; the films (FFmpeg): " + ("the SDK's" if parts.ffmpeg else "left out (no Call of Duty 3 SDK)") +
             "; the F2 window (Dear ImGui): " + ("the SDK's" if parts.imgui else "left out (no Call of Duty 3 SDK)"))
    step(2)
    bro.place_xbe(disc, lay)
    copier = bro.GameCopy(disc, lay, log, cancel).start()
    try:
        step(3)
        bro.ensure_xboxrecomp(lay, tools.git, runner, log)
        py = bro.ensure_python(lay, runner, log)
        if not (common.read_json(lay.source_stamp) == source_key(lay) and (lay.xboxrecomp / ".git").exists()):
            prepare_xboxrecomp(lay, tools.git, runner, log)
        keys = step_keys(lay, py, disc.xbe_md5, runner)
        # [waw-decomp/speed1] 2026-11-23 (owner 21:00): the recompiler's output kept between builds,
        # cleanup on or off, as Big Red One's (bro.LiftCache, <work>/tools/lift-cache/fh)
        cache = bro.LiftCache(lay.lift_cache, log, GAME_NAME)
        if o.clear_lift_cache:
            cache.clear()
        bro.restore_lift(lay, keys, disc.xbe_md5, cache, log)
        step(4)
        ran = recompile(lay, py, keys, disc.row.loader_back_edge if disc.row else "", runner, log,
                        keep_gen=not o.cleanup)
        bro.keep_lift(cache, lay, disc.xbe_md5, keys, ran, {**source_key(lay), "xbe_md5": disc.xbe_md5},
                      cancel, log)
        step(5)
        exe = build(lay, tools, parts, o.jobs, runner, log, keep=not o.cleanup)
        copier.finish()
    except BaseException:
        copier.stop()
        raise
    step(6)
    install(lay, exe, disc, o.tool_version, log)
    if o.hd_build:
        # Before the cleanup: the ripper may be built in the work folder. A failure here is only a warning.
        log.step("Making HD textures")
        tool_root = lay.repo / Path(__file__).resolve().parent.parent.name
        disc_hd.build("fh", lay.out, tool_root, lay.tools, lay.work / "hd", o.upscale_settings, o.jobs, runner, log,
                      cancel)
    if o.cleanup:
        common.safe_rmtree(lay.work, work, [lay.out, disc.path], log)
        log.info(f"Removed the build files in {lay.work}. xboxrecomp and Capstone stay in {lay.tools}.")
    else:
        log.info(f"Kept the recompiler's output and the build in {lay.work} for the next build (cleanup is off).")
    log.progress(1.0, "Done")
    return (f"{GAME_NAME} is in {lay.out}: Play starts it (or {lay.launcher.name} there).\n"
            f"Saves: {lay.saves}. Its settings: {lay.settings} (the F2 window in the game changes them too).")


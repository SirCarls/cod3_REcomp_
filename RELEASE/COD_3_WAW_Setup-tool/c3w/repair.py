"""Check a finished build in the output folder and say what to rebuild.

check_install() looks at what a build leaves in the output folder (see
pipeline.Pipeline.package / windows_game / finish and the record it writes,
cod3_waw.json) and lists only real problems: missing or empty files, a
texture pack whose index names files that aren't there, ripped files from an
older ripper, a record naming files that don't exist. Nothing is changed and
nothing is run.

repair_overrides() turns a report into BuildOptions changes that make the
build redo only the steps those problems need (BuildOptions.only_steps).
Up-to-date work is still skipped the usual way (the SDK's stamp, ninja, the
ripper keeping unchanged files), so a repair is quick when the work folder
was kept.
"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set

from . import binaries, cross, rippers, upscale, user_textures
from .common import IS_LINUX, IS_WINDOWS, exe_name, is_within, read_json
from .pipeline import RECOMP_FILES, TOOL_VERSION, missing_levels

# What a repair can do about an issue (Issue.fix):
#   rebuild_game     build the Linux version again (AppImage or bin/)
#   rebuild_windows  build the Windows version again (cod3.exe and its DLLs)
#   repack_hd        pack the textures again from the pictures already there
#                    (the HD texture pack, or your own texture pack)
#   rerip            take the files off the disc again and rip them (or only
#                    copy the ripper, or rip World at War, by Issue.area)
#   relaunchers      write the record, the launcher copy and Launcher.sh/.bat
FIXES = ("rebuild_game", "rebuild_windows", "repack_hd", "rerip", "relaunchers")
FIX_LABELS = {
    "rebuild_game": "Build the Linux version again",
    "rebuild_windows": "Build the Windows version again",
    "repack_hd": "Pack the textures again",
    "rerip": "Rip the game files again",
    "relaunchers": "Write the launchers again",
}

MANIFEST = "cod3_waw.json"
DDS_HEADER = 128            # bytes: "DDS " and the header; a texture file is longer
MIN_APPIMAGE = 1024 * 1024  # the game's AppImage is far bigger; less is a cut-off copy
LIST_MAX = 5                # names shown in an issue's detail


@dataclass
class Issue:
    severity: str              # "error" or "warning"
    title: str                 # one short line
    detail: str = ""           # what exactly is wrong (file names)
    fix: Optional[str] = None  # one of FIXES, or None (nothing to do automatically)
    area: str = ""             # what part: linux, windows, assets, data, hd, user, ripper, waw, launcher, record


@dataclass
class Report:
    out_dir: Path
    issues: List[Issue] = field(default_factory=list)
    manifest: dict = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        """True when there is no error (warnings are fine)."""
        return not any(i.severity == "error" for i in self.issues)

    @property
    def fixes(self) -> List[str]:
        """The fixes the issues ask for, in FIXES order."""
        wanted = {i.fix for i in self.issues if i.fix}
        return [f for f in FIXES if f in wanted]

    def summary(self) -> str:
        errors = sum(1 for i in self.issues if i.severity == "error")
        warnings = len(self.issues) - errors
        if not self.issues:
            return "No problems found."
        parts = [f"{errors} problem{'s' if errors != 1 else ''}" if errors else "",
                 f"{warnings} warning{'s' if warnings != 1 else ''}" if warnings else ""]
        return ", ".join(p for p in parts if p) + " found."

    def add(self, severity: str, title: str, detail: str = "", fix: Optional[str] = None, area: str = "") -> None:
        self.issues.append(Issue(severity, title, detail, fix, area))


def _names(items: List[str]) -> str:
    """A few names, then how many more."""
    shown = ", ".join(items[:LIST_MAX])
    return shown + (f" and {len(items) - LIST_MAX} more" if len(items) > LIST_MAX else "")


def _size(p: Path) -> int:
    try:
        return p.stat().st_size if p.is_file() else -1
    except OSError:
        return -1


def _head(p: Path, n: int) -> bytes:
    try:
        with open(p, "rb") as f:
            return f.read(n)
    except OSError:
        return b""


def _has_files(folder: Path, pattern: str = "*") -> bool:
    try:
        return folder.is_dir() and any(p.is_file() for p in folder.glob(pattern))
    except OSError:
        return False


# ---------------------------------------------------------------------------
# The record (cod3_waw.json)
# ---------------------------------------------------------------------------
def _recorded(report: Report, key: str, value: object = None) -> Optional[Path]:
    """The path the record gives for key (or value, one entry of a list
    under key), inside the output folder, or None. A path that would lead
    outside the folder is reported, never followed."""
    if value is None:
        value = report.manifest.get(key)
    if not value:
        return None
    if not isinstance(value, str) or "\0" in value:
        report.add("error", "The build's record has a bad entry", f"{MANIFEST}: {key}", None, "record")
        return None
    parts = [p for p in re.split(r"[\\/]+", value) if p not in ("", ".")]
    path = report.out_dir.joinpath(*parts) if parts else report.out_dir
    if not parts or ".." in parts or re.match(r"^[A-Za-z]:", value) or value.startswith(("/", "\\")) \
            or not is_within(path, report.out_dir):
        report.add("error", "The build's record names a place outside the output folder",
                   f"{MANIFEST}: {key} = {value}", None, "record")
        return None
    return path


# ---------------------------------------------------------------------------
# The game
# ---------------------------------------------------------------------------
def _check_packed(report: Report, key: str, fix: str, area: str, label: str) -> Optional[bool]:
    """The files the record lists for a version (cod3_files_linux /
    _windows, written since 2026-10-12): True when all are there, False when
    some aren't (reported), None when the record has no list."""
    files = report.manifest.get(key)
    if not isinstance(files, list) or not files:
        return None
    missing = []
    for rel in files:
        path = _recorded(report, key, rel)
        if path is not None and _size(path) <= 0:
            missing.append(path.relative_to(report.out_dir).as_posix())
    if missing:
        report.add("error", f"Files of the {label} are missing", _names(missing), fix, area)
        return False
    return True


# Libraries a build packs next to the game (the game's own, the SDK's, the
# C++ runtimes); anything else it imports comes with the system. Only used
# for records without a file list, when this PC can't tell by itself.
_PACKED_HINTS = ("cod3", "rex", "fidelityfx", "sdl")
_PACKED_PREFIXES = binaries.VC_RUNTIME_PREFIXES + ("libc++", "libunwind", "libwinpthread")


def _looks_packed(name: str) -> bool:
    low = name.lower()
    return low.startswith(_PACKED_PREFIXES) or any(h in low for h in _PACKED_HINTS)


def _levels_and_plugin(report: Report, folder: Path, windows: bool, fix: str, area: str, label: str) -> None:
    missing = missing_levels(folder)
    if missing:
        report.add("error", f"Level files are missing from the {label}",
                   f"In {folder.name}/: the libraries for {_names(missing)}.", fix, area)
    if not binaries.find_gpu_plugin(binaries.GPU_PLUGIN, [folder], windows):
        names = " or ".join(binaries.gpu_plugin_names(binaries.GPU_PLUGIN, windows))
        report.add("error", f"The {label} is missing its GPU plugin", f"{names} isn't in {folder.name}/.", fix, area)


def _check_linux(report: Report, exe: Path) -> None:
    fix, area, label = "rebuild_game", "linux", "Linux version"
    rel = exe.relative_to(report.out_dir).as_posix()
    size = _size(exe)
    if size < 0:
        report.add("error", "The Linux version is missing", f"{rel} isn't there.", fix, area)
        return
    if size == 0 or _head(exe, 4) != b"\x7fELF" or (exe.suffix == ".AppImage" and size < MIN_APPIMAGE):
        report.add("error", "The Linux version is damaged", f"{rel} is empty or cut off.", fix, area)
        return
    if _check_packed(report, "cod3_files_linux", fix, area, label) is not None or exe.suffix == ".AppImage":
        return   # the files the build packed are all checked (an AppImage is one file)
    folder = exe.parent
    _levels_and_plugin(report, folder, False, fix, area, label)
    if IS_LINUX:
        # Libraries the game needs that are neither next to it nor on this PC.
        try:
            start = [exe] + sorted(folder.glob("*.so*"))
            system = binaries.linux_system_libraries()
            _files, missing = binaries.gather(start, [folder], lambda n: n in system or not _looks_packed(n))
        except (OSError, ValueError, IndexError) as e:
            report.add("error", "The Linux version is damaged", f"{folder.name}/: {e}", fix, area)
            return
        libs = sorted({lib for _, lib in missing})
        if libs:
            report.add("error", "The Linux version needs libraries that are missing", _names(libs), fix, area)


def _windows_system_dll(name: str) -> bool:
    if cross.is_windows_system_dll(name):
        return True
    # On Windows, System32 tells; elsewhere, by the name.
    return binaries.windows_is_system_dll(name) if IS_WINDOWS else not _looks_packed(name)


def _check_windows(report: Report, exe: Path) -> None:
    fix, area, label = "rebuild_windows", "windows", "Windows version"
    rel = exe.relative_to(report.out_dir).as_posix()
    size = _size(exe)
    if size < 0:
        report.add("error", "The Windows version is missing", f"{rel} isn't there.", fix, area)
        return
    if size == 0 or _head(exe, 2) != b"MZ":
        report.add("error", "The Windows version is damaged", f"{rel} is empty or cut off.", fix, area)
        return
    if _check_packed(report, "cod3_files_windows", fix, area, label) is not None:
        return
    folder = exe.parent
    _levels_and_plugin(report, folder, True, fix, area, label)
    # cod3.exe and its DLLs: every DLL they import is next to them or part
    # of Windows (the same rule the build packed them by).
    try:
        start = [exe] + sorted(p for p in folder.iterdir() if p.suffix.lower() == ".dll" and p.is_file())
        _files, missing = binaries.gather(start, [folder], _windows_system_dll)
    except (OSError, ValueError, IndexError) as e:
        report.add("error", "The Windows version is damaged", f"{folder.name}/: {e}", fix, area)
        return
    dlls = sorted({lib for _, lib in missing}, key=str.lower)
    if dlls:
        report.add("error", "The Windows version is missing DLLs", f"{folder.name}/: {_names(dlls)}", fix, area)


def _check_game(report: Report) -> None:
    linux = _recorded(report, "cod3_exe_linux")
    windows = _recorded(report, "cod3_exe_windows")
    if not linux and not windows:
        # A record from an older tool: only "cod3_exe", for the system that built it.
        only = _recorded(report, "cod3_exe")
        if only and only.suffix.lower() == ".exe":
            windows = only
        elif only:
            linux = only
    if linux:
        _check_linux(report, linux)
    if windows:
        _check_windows(report, windows)


# ---------------------------------------------------------------------------
# Files from the disc, the rips and the texture packs
# ---------------------------------------------------------------------------
def _check_assets(report: Report) -> None:
    assets = report.out_dir / "assets"
    if not assets.is_dir():
        report.add("error", "The game files from the disc are missing", "assets/ isn't there.", "rerip", "assets")
        return
    missing = [f for f in RECOMP_FILES if _size(assets / f) <= 0]
    if missing:
        report.add("error", "Game files from the disc are missing", f"In assets/: {_names(missing)}",
                   "rerip", "assets")
    # A file the extraction was writing when it stopped.
    parts = []
    for root, _dirs, files in os.walk(assets):
        parts += [Path(root, f).relative_to(assets).as_posix() for f in files if f.endswith(".part")]
    if parts:
        report.add("error", "Taking the files off the disc didn't finish", f"In assets/: {_names(sorted(parts))}",
                   "rerip", "assets")


def guess_layout(textures: Path) -> Optional[int]:
    """The ripper output layout of a rip without a layout record, from its
    hd_keys.tsv: none (layout 1), 5 columns (2-3), 6 (4-5) or 7 (6). None
    when there's nothing to tell by."""
    keys = textures / upscale.KEYS_FILE
    try:
        text = keys.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 1 if _has_files(textures) else None
    for line in text.splitlines():
        if line.strip() and not line.startswith("#"):
            columns = len(line.rstrip("\r").split("\t"))
            return 6 if columns >= 7 else 4 if columns == 6 else 2
    return None


def _check_rip(report: Report) -> None:
    data = _recorded(report, "cod3_data")
    if not data:
        return
    textures = data / "textures"
    if not data.is_dir() or not _has_files(textures):
        report.add("warning", "The ripped Call of Duty 3 files are missing",
                   f"{data.relative_to(report.out_dir).as_posix()}/textures is empty or not there.", "rerip", "data")
        return
    recorded = rippers.recorded_layout(data)
    layout = recorded if recorded is not None else guess_layout(textures)
    if layout is not None and layout < rippers.RIP_OUTPUT_VERSION:
        report.add("warning", "The ripped files are from an older ripper",
                   "Rip them again so the HD textures and your own textures fit them.", "rerip", "data")
    elif (textures / upscale.KEYS_FILE).is_file() and _size(textures / upscale.KEYS_FILE) == 0:
        report.add("warning", "The list of ripped textures is empty", f"textures/{upscale.KEYS_FILE} is empty.",
                   "rerip", "data")


def _index_files(index: Path) -> Optional[List[str]]:
    """The file names a pack's index.tsv lists (5th column), or None if it
    can't be read."""
    try:
        text = index.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    rows = [line.rstrip("\r").split("\t") for line in text.splitlines() if line.strip() and not line.startswith("#")]
    return [r[4] for r in rows if len(r) >= 5]


def _check_pack(report: Report, pack: Path, label: str, area: str, sources_left: bool, how: str) -> None:
    """A texture pack: its index exists and every file it names is there.
    sources_left: the pictures it's packed from are still there, so packing
    again repairs it."""
    rel = pack.relative_to(report.out_dir).as_posix()
    fix = "repack_hd" if sources_left else None
    index = pack / upscale.PACK_INDEX
    files = _index_files(index)
    if files is None:   # the game doesn't use a pack without its index
        report.add("warning", f"The {label} is incomplete", f"{rel}/{upscale.PACK_INDEX} is missing." +
                   ("" if sources_left else " " + how), fix, area)
        return
    bad = [f for f in files if f != Path(f).name or f in ("", ".", "..") or "\\" in f]
    if bad:
        report.add("error", f"The {label}'s index names files outside it", f"{rel}: {_names(bad)}", fix, area)
    missing = [f for f in files if f not in bad and
               (_size(pack / f) <= DDS_HEADER or _head(pack / f, 4) != b"DDS ")]
    if missing:
        report.add("error", f"The {label} is missing {len(missing)} texture{'s' if len(missing) != 1 else ''}",
                   f"In {rel}/: {_names(missing)}" + ("" if sources_left else ". " + how), fix, area)


def _check_hd(report: Report) -> None:
    data = report.out_dir / "data" / "cod3"
    pack = _recorded(report, "cod3_hd_textures")
    if not pack and (data / upscale.PACK_NAME).is_dir():
        pack = data / upscale.PACK_NAME
    upscaled = _recorded(report, "cod3_textures_upscaled")
    pngs = bool(upscaled) and _has_files(upscaled, "*.png")
    if not pngs:
        pngs = any(_has_files(data / n, "*.png") for n in (upscale.OUTPUT_NAME, upscale.LEGACY_OUTPUT_NAME))
    if pack:
        _check_pack(report, pack, "HD texture pack", "hd", pngs,
                    "The upscaled pictures it's made from are gone too: tick \"Upscale the textures\" and build, "
                    "or delete the pack to play with the game's own textures.")


def _check_user_pack(report: Report) -> None:
    data = report.out_dir / "data" / "cod3"
    pack = _recorded(report, "cod3_user_textures")
    if not pack and (data / user_textures.PACK_NAME / user_textures.PACK_INDEX).is_file():
        pack = data / user_textures.PACK_NAME
    if pack:
        pictures = bool(user_textures.user_pictures(data))
        _check_pack(report, pack, "user texture pack", "user", pictures,
                    f"Your pictures in data/cod3/{user_textures.PNG_DIR_NAME} are gone too.")


def _check_ripper(report: Report) -> None:
    named = _recorded(report, "ripper")
    here = report.out_dir / "tools" / exe_name("rip")
    for rip in dict.fromkeys(p for p in (named, here) if p):
        if _size(rip) <= 0:
            report.add("warning", "The ripper is missing from the output folder",
                       f"{rip.relative_to(report.out_dir).as_posix()} isn't there. The launcher uses it to add "
                       "custom maps and to pack your own textures.", "rerip", "ripper")
            return


def _check_waw(report: Report) -> None:
    data = _recorded(report, "waw_data")
    if data and not _has_files(data, "*/*"):
        report.add("warning", "The World at War files are missing",
                   f"{data.relative_to(report.out_dir).as_posix()} is empty or not there.", "rerip", "waw")


# ---------------------------------------------------------------------------
# Launchers
# ---------------------------------------------------------------------------
_VERSION = re.compile(r'^TOOL_VERSION\s*=\s*"([^"]+)"', re.MULTILINE)


def _check_launchers(report: Report) -> None:
    out = report.out_dir
    fix, area = "relaunchers", "launcher"
    launcher = out / "launcher"
    if _size(launcher / "cod3_waw_setup.py") <= 0 or not (launcher / "c3w").is_dir():
        report.add("error", "The launcher in the output folder is missing", "launcher/ is missing or incomplete.",
                   fix, area)
    else:
        here = Path(__file__).resolve().parent
        if here != (launcher / "c3w").resolve():
            missing = sorted(p.name for p in here.glob("*.py") if _size(launcher / "c3w" / p.name) <= 0)
            try:
                m = _VERSION.search((launcher / "c3w" / "pipeline.py").read_text(encoding="utf-8", errors="replace"))
            except OSError:
                m = None
            if missing:
                report.add("warning", "The launcher in the output folder is older than this tool",
                           f"It lacks launcher/c3w/{_names(missing)}.", fix, area)
            elif not m or m.group(1) != TOOL_VERSION:
                report.add("warning", "The launcher in the output folder is from another version of this tool",
                           f"Its version: {m.group(1) if m else 'unknown'}; this tool: {TOOL_VERSION}.", fix, area)
    for name, own in (("Launcher.sh", not IS_WINDOWS), ("Launcher.bat", IS_WINDOWS)):
        if _size(out / name) <= 0:
            report.add("error" if own else "warning", f"{name} is missing",
                       "It starts the launcher" + (" on this system." if own else " on the other system."), fix, area)


# ---------------------------------------------------------------------------
def check_install(out_dir: Path) -> Report:
    """Look over the build in out_dir and list real problems. Reads files
    only (a few seconds: the game's libraries are read for their imports)."""
    out = Path(out_dir).resolve()
    report = Report(out)
    if not out.is_dir():
        report.add("error", "The output folder isn't there", str(out_dir), None, "record")
        return report
    manifest_path = out / MANIFEST
    if not manifest_path.is_file():
        report.add("error", "There is no finished build in this folder",
                   f"{MANIFEST} is missing. Build the game first.", None, "record")
        return report
    manifest = read_json(manifest_path)
    if not isinstance(manifest, dict) or not manifest:
        report.add("error", "The build's record can't be read",
                   f"{MANIFEST} is damaged. Build the game again.", None, "record")
        return report
    report.manifest = manifest
    _check_game(report)
    _check_assets(report)
    _check_rip(report)
    _check_hd(report)
    _check_user_pack(report)
    _check_ripper(report)
    _check_waw(report)
    _check_launchers(report)
    return report


# The build steps (pipeline Step.key) each fix runs, by area.
_STEPS: Dict[str, Set[str]] = {
    "rebuild_game": {"sdk", "recompile", "package", "linux_wsl"},
    "rebuild_windows": {"sdk", "recompile", "package", "windows"},
    "repack_hd:hd": {"hd_pack"},
    "repack_hd:user": {"user_pack"},
    "rerip:assets": {"assets", "rip_cod3", "user_pack"},
    "rerip:data": {"rip_cod3", "user_pack"},
    "rerip:ripper": {"ripper"},
    "rerip:waw": {"waw"},
    "relaunchers": {"finish"},
}


def repair_overrides(report: Report) -> dict:
    """BuildOptions field overrides that make a build redo only what the
    report's issues need. {} when there is nothing to repair. Apply them to
    the options the GUI would build with:
    dataclasses.replace(options, **repair_overrides(report))."""
    steps: Set[str] = set()
    areas = set()
    for i in report.issues:
        if not i.fix:
            continue
        areas.add(i.area)
        steps |= _STEPS.get(f"{i.fix}:{i.area}", _STEPS.get(i.fix, set()))
    if not steps:
        return {}
    out = report.out_dir
    data = out / "data" / "cod3"
    if "rip_cod3" in steps and ((data / upscale.PACK_NAME).is_dir() or
                                any(_has_files(data / n, "*.png") for n in (upscale.OUTPUT_NAME,
                                                                            upscale.LEGACY_OUTPUT_NAME))):
        steps.add("hd_pack")   # a new rip can change which picture a texture has
    fixes = set(report.fixes)
    game, windows = "rebuild_game" in fixes, "rebuild_windows" in fixes
    overrides = {
        "only_steps": sorted(steps),
        "output_dir": str(out),
        "build_recomp": game or windows,
        "build_linux": game,
        "build_windows": windows,
    }
    # The game files already in the output folder are the source, unless
    # they are what's broken: then the disc is needed.
    needs_assets = steps & {"assets", "rip_cod3", "recompile", "linux_wsl", "sdk"}
    if needs_assets and "assets" not in areas and (out / "assets" / "default.xex").is_file():
        overrides["cod3_source"] = str(out / "assets")
    return overrides

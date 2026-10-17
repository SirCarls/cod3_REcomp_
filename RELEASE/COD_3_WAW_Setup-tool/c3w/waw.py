"""Call of Duty: World at War as a content source.

World at War is never run. Its files are read (never changed) and ripped
into the output folder by the Rust ripper, for the native engine.

Where the files come from (the owner's rule):

1. **Steam folder.** This is the preferred source, and when one is given it
   is the only source used: the other choices are ignored.
2. Without a Steam folder, the user picks one of:
   - their **Xbox 360** disc image (.iso);
   - their **PC disc**: a disc image, the disc drive, or a folder installed
     from the disc.

Mods and custom maps (UGX zombies maps, Spain at War and others; switched
off for now, see MODS_ENABLED) are unpacked natively, without running their
installers, into the output folder
(`data/waw/custom_maps/<name>`), never into World at War's own mods folder.
Installers (.exe) are opened with innoextract or 7-Zip, archives with
Python's zipfile (.zip), 7-Zip (.7z, .rar) or unar (.rar).
"""
from __future__ import annotations

import os
import re
import threading
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

from .common import IS_WINDOWS, Cancelled, Log, Runner, SetupError, copy_tree, is_within, safe_rmtree
from .xiso import XisoImage, looks_like_xiso

STEAM_APPID = "10090"
STEAM_INSTALL_DIR = "Call of Duty World at War"
STOCK_ZOMBIE_MAPS = [
    ("Nacht der Untoten", "nazi_zombie_prototype"),
    ("Verruckt", "nazi_zombie_asylum"),
    ("Shi No Numa", "nazi_zombie_sumpf"),
    ("Der Riese", "nazi_zombie_factory"),
]

# Source kinds, in the order the GUI and CLI present them.
STEAM = "steam"
XBOX360 = "xbox360"
PC_DISC = "pc_disc"
NONE = "none"
KIND_LABELS = {
    STEAM: "Steam folder",
    XBOX360: "Xbox 360 disc image",
    PC_DISC: "PC disc",
    NONE: "no World at War",
}

# Where things go inside the output folder.
OUTPUT_DATA = Path("data") / "waw"
CUSTOM_MAPS = OUTPUT_DATA / "custom_maps"

# Mods and custom maps are switched off for now (owner, 2026-10-10): they will
# come back as a separate project. The code below stays so they can be turned
# on again; with this off, the GUI page, the `add-map` command and the input
# folder scan for mods are left out.
MODS_ENABLED = False


def _child(folder: Path, name: str) -> Optional[Path]:
    """Case-insensitive lookup: Linux file systems are case-sensitive, and WaW
    folders copied from Windows are not consistent about case."""
    if not folder.is_dir():
        return None
    direct = folder / name
    if direct.exists():
        return direct
    low = name.lower()
    for p in folder.iterdir():
        if p.name.lower() == low:
            return p
    return None


# ---------------------------------------------------------------------------
# Finding and checking sources
# ---------------------------------------------------------------------------
def validate_install(folder: Path) -> Tuple[bool, str]:
    """Is `folder` an installed World at War (Steam, or installed from the
    PC disc)? Ripping needs main/ and zone/; CoDWaW.exe only confirms it."""
    folder = Path(folder)
    if not folder.is_dir():
        return False, "Folder not found."
    missing = [n for n in ("main", "zone", "CoDWaW.exe") if not _child(folder, n)]
    if missing:
        return False, "Not a World at War install (missing " + ", ".join(missing) + ")."
    kind = "Steam" if find_steamapps(folder) else "installed from disc"
    return True, f"World at War found ({kind})."


def find_steamapps(folder: Path) -> Optional[Path]:
    for p in [Path(folder)] + list(Path(folder).parents):
        if p.name.lower() == "steamapps":
            return p
    return None


def _steam_roots() -> List[Path]:
    """Folders where a Steam client keeps its data, on this system."""
    roots: List[Path] = []
    if IS_WINDOWS:
        try:
            import winreg
            for hive, key, value in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                                     (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                                     (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath")):
                try:
                    with winreg.OpenKey(hive, key) as k:
                        roots.append(Path(str(winreg.QueryValueEx(k, value)[0])))
                except OSError:
                    pass
        except ImportError:
            pass
        for var in ("ProgramFiles(x86)", "ProgramFiles"):
            if os.environ.get(var):
                roots.append(Path(os.environ[var]) / "Steam")
    else:
        home = Path.home()
        data_home = Path(os.environ["XDG_DATA_HOME"]) if os.environ.get("XDG_DATA_HOME") else home / ".local" / "share"
        flatpak = home / ".var" / "app" / "com.valvesoftware.Steam"
        roots += [home / ".steam" / "steam", home / ".steam" / "root", home / ".steam" / "debian-installation",
                  data_home / "Steam", home / ".local" / "share" / "Steam",
                  flatpak / ".local" / "share" / "Steam", flatpak / "data" / "Steam",
                  home / "snap" / "steam" / "common" / ".local" / "share" / "Steam"]
    return [r for r in roots if r.is_dir()]


def _steam_libraries(root: Path) -> List[Path]:
    """The Steam library folders listed in root/steamapps/libraryfolders.vdf."""
    libraries = [root]
    vdf = root / "steamapps" / "libraryfolders.vdf"
    try:
        text = vdf.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return libraries
    for match in re.finditer(r'"path"\s+"([^"]+)"', text):
        libraries.append(Path(match.group(1).replace("\\\\", "\\")))
    return libraries


def steam_libraries() -> List[Path]:
    """Every Steam library folder on this computer, each once (the same
    library is often reached from two Steam roots through links)."""
    seen, out = set(), []
    for root in _steam_roots():
        for library in _steam_libraries(root):
            try:
                key = str(library.resolve())
            except OSError:
                key = str(library)
            key = key.lower() if IS_WINDOWS else key
            if key not in seen:
                seen.add(key)
                out.append(library)
    return out


def find_steam_waw() -> Optional[Path]:
    """World at War in any Steam library on this computer, if installed."""
    for library in steam_libraries():
        folder = library / "steamapps" / "common" / STEAM_INSTALL_DIR
        if validate_install(folder)[0]:
            return folder
    return None


def check_xbox360_image(path: Path) -> Tuple[bool, str]:
    path = Path(path)
    if not path.is_file():
        return False, "File not found."
    if not looks_like_xiso(path):
        return False, "Not an Xbox 360 disc image."
    try:
        with XisoImage(path) as img:
            # The Xbox 360 disc keeps its fastfiles in the root folder (the PC
            # version uses zone/<language>/).
            fastfiles = [e for e in img.files if e.path.lower().endswith(".ff")]
    except (OSError, SetupError) as e:
        return False, str(e)
    if not fastfiles:
        return False, "No World at War fastfiles (*.ff) on this disc image."
    return True, f"Xbox 360 disc image with {len(fastfiles)} fastfiles."


def check_pc_disc(path: Path) -> Tuple[bool, str]:
    path = Path(path)
    if path.is_file():
        return True, "PC disc image: it will be unpacked with 7-Zip."
    if path.is_dir():
        if validate_install(path)[0]:
            return True, "World at War installed from the PC disc."
        if any(p.name.lower() in ("data1.hdr", "data1.cab", "setup.exe") for p in path.iterdir()):
            return True, "PC disc (drive or copy): its installer files will be unpacked."
        return False, "This folder doesn't look like the World at War PC disc."
    return False, "Not found."


@dataclass
class Source:
    kind: str                 # STEAM, XBOX360, PC_DISC or NONE
    path: Optional[Path] = None

    @property
    def label(self) -> str:
        return KIND_LABELS[self.kind] + (f": {self.path}" if self.path else "")


def choose_source(steam: str, xbox360: str, pc_disc: str, alternative: str) -> Tuple[Source, List[str]]:
    """Applies the source rule. Returns the source and notes for the log.

    - A Steam folder, when given, is the only source used.
    - Otherwise `alternative` (XBOX360 or PC_DISC) picks between the other two.
      If only one of them is filled in, that one is used.
    """
    notes: List[str] = []
    if steam.strip():
        ignored = [KIND_LABELS[k] for k, v in ((XBOX360, xbox360), (PC_DISC, pc_disc)) if v.strip()]
        if ignored:
            notes.append("A Steam folder is set, so it is the only World at War source used "
                         f"(ignoring the {' and '.join(ignored)}).")
        return Source(STEAM, Path(steam.strip())), notes
    given = {k: v.strip() for k, v in ((XBOX360, xbox360), (PC_DISC, pc_disc)) if v.strip()}
    if not given:
        return Source(NONE), notes
    kind = alternative if alternative in given else next(iter(given))
    return Source(kind, Path(given[kind])), notes


def check_source(source: Source) -> Tuple[bool, str]:
    if source.kind == NONE or not source.path:
        return True, "No World at War source: only Call of Duty 3 is set up."
    if source.kind == STEAM:
        ok, msg = validate_install(source.path)
        if ok and not find_steamapps(source.path):
            msg += " (Not inside a Steam library; it is used as given.)"
        return ok, msg
    if source.kind == XBOX360:
        return check_xbox360_image(source.path)
    return check_pc_disc(source.path)


def guess_kind(path: Path) -> str:
    """For the command line's plain --waw: which kind of source is this?"""
    path = Path(path)
    if path.is_dir():
        return STEAM if find_steamapps(path) else PC_DISC
    if path.is_file() and looks_like_xiso(path):
        return XBOX360
    return PC_DISC


# ---------------------------------------------------------------------------
# Getting the files to rip
# ---------------------------------------------------------------------------
def fastfiles(folder: Path) -> List[Path]:
    """Every fastfile of a World at War folder, except in mods/ folders (those
    are the player's own and are skipped). The PC version keeps them under
    zone/; the Xbox 360 disc has them in its root folder."""
    zone = _child(Path(folder), "zone") or Path(folder)
    if not zone.is_dir():
        return []
    found = []
    for root, dirs, files in os.walk(zone):
        dirs[:] = [d for d in dirs if d.lower() != "mods"]
        found += [Path(root) / f for f in files if f.lower().endswith(".ff")]
    return sorted(found)


def prepare(source: Source, work: Path, sevenzip: Optional[Path], unshield: Optional[Path],
            runner: Runner, log: Log, cancel: threading.Event) -> Optional[Path]:
    """The folder holding zone/ for `source`. A Steam or installed folder is
    used in place (read-only); disc images are unpacked into `work`."""
    if source.kind == NONE or not source.path:
        return None
    if source.kind == STEAM:
        return source.path
    if source.kind == XBOX360:
        dest = work / "waw_xbox360"
        log.info(f"Copying the fastfiles out of {source.path.name} (read-only)...")
        with XisoImage(source.path) as img:
            img.extract(dest, log, cancel, only=lambda rel: rel.lower().endswith(".ff"))
        return dest
    # PC disc.
    if source.path.is_dir() and validate_install(source.path)[0]:
        return source.path
    return unpack_pc_disc(source.path, work / "waw_pc_disc", sevenzip, unshield, runner, log)


def unpack_pc_disc(disc: Path, dest: Path, sevenzip: Optional[Path], unshield: Optional[Path],
                   runner: Runner, log: Log) -> Path:
    """Unpacks the retail PC DVD (an image, the drive or a copy of it) and
    returns the folder holding zone/. The disc keeps the game in
    InstallShield cabinets, which `unshield` opens."""
    dest.mkdir(parents=True, exist_ok=True)
    disc_root = disc
    if disc.is_file():
        if not sevenzip:
            raise SetupError("Opening a disc image needs 7-Zip; the build installs it. Run the build again.")
        log.info(f"Unpacking {disc.name} with 7-Zip...")
        runner.run([sevenzip, "x", "-y", f"-o{dest / 'disc'}", disc], quiet=True)
        disc_root = dest / "disc"

    def locate(base: Path) -> Optional[Path]:
        for root, _, _ in os.walk(base):
            if _child(Path(root), "zone") and _child(Path(root), "main"):
                return Path(root)
        return None

    found = locate(disc_root)
    if found:
        return found
    cabinets = [p for p in Path(disc_root).rglob("*") if p.name.lower() == "data1.cab"]
    if cabinets and unshield:
        out = dest / "installed"
        log.info("Unpacking the disc's InstallShield cabinets with unshield...")
        runner.run([unshield, "-d", out, "x", cabinets[0]], quiet=True)
        found = locate(out)
        if found:
            return found
    hint = ("unshield (the build installs it on Linux)" if not IS_WINDOWS else
            "a tool this computer doesn't have")
    raise SetupError("The World at War PC disc keeps the game in installer cabinets, which need "
                     f"{hint}.\nInstead you can install World at War from the disc as usual, then choose "
                     "the installed folder as the PC disc source.")


# ---------------------------------------------------------------------------
# Custom zombies maps (UGX and others)
# ---------------------------------------------------------------------------
@dataclass
class CustomMap:
    name: str
    folder: Path      # <output>/data/waw/custom_maps/<name>

    @property
    def fastfiles(self) -> List[Path]:
        return sorted(p for p in self.folder.rglob("*") if p.suffix.lower() == ".ff")


def list_custom_maps(output: Path) -> List[CustomMap]:
    root = Path(output) / CUSTOM_MAPS
    if not root.is_dir():
        return []
    return [CustomMap(d.name, d) for d in sorted(root.iterdir()) if d.is_dir()]


@dataclass
class Unpackers:
    """The external unpackers found on this computer (any may be missing)."""
    sevenzip: Optional[Path] = None
    innoextract: Optional[Path] = None
    unar: Optional[Path] = None


def _unpack(src: Path, dest: Path, unpackers: Unpackers, runner: Runner) -> None:
    """Unpacks an installer or archive without running it. Each tool writes
    into `dest` only (zip entries that would land outside it are refused;
    7-Zip, innoextract and unar strip absolute and "../" paths themselves)."""
    name = src.name.lower()
    if name.endswith(".zip"):
        with zipfile.ZipFile(src) as z:
            for m in z.infolist():
                if not is_within(dest / m.filename, dest):
                    raise SetupError(f"Unsafe path in archive: {m.filename}")
            z.extractall(dest)
        return
    attempts = []
    if name.endswith(".exe") and unpackers.innoextract:
        attempts.append([unpackers.innoextract, "--silent", "--extract", "--output-dir", dest, src])
    if unpackers.sevenzip:
        attempts.append([unpackers.sevenzip, "x", "-y", f"-o{dest}", src])
    if name.endswith(".rar") and unpackers.unar:
        attempts.append([unpackers.unar, "-quiet", "-force-overwrite", "-output-directory", dest, src])
    if not attempts:
        raise SetupError("Unpacking installers and archives needs 7-Zip, innoextract or unar. Use 'Check this PC' "
                         "on the Set up page (it installs them), then try again.")
    for cmd in attempts:
        rc = runner.run(cmd, check=False, quiet=True)
        if rc == 0 and any(_map_content(dest)):
            return
    hint = " For .rar files, install unar (Check this PC does it)." if name.endswith(".rar") else ""
    raise SetupError(f"Could not unpack {src.name}: none of the unpackers could open it.{hint}")


def _map_content(staged: Path) -> Tuple[List[Path], List[Path]]:
    """(mod folders, usermaps folders) anywhere in an unpacked map."""
    mods: List[Path] = []
    maps: List[Path] = []
    for root, dirs, files in os.walk(staged):
        r = Path(root)
        low = r.name.lower()
        if low == "mods":
            mods += [r / d for d in dirs]
            dirs[:] = []
        elif low == "usermaps":
            maps += [r / d for d in dirs]
            dirs[:] = []
        elif any(f.lower() == "mod.ff" for f in files) and r != staged:
            mods.append(r)
            dirs[:] = []
    if not mods and any(f.lower() == "mod.ff" for f in os.listdir(staged)):
        mods.append(staged)
    return mods, maps


def install_custom_map(src: Path, output: Path, scratch: Path, unpackers: Unpackers, runner: Runner, log: Log,
                       cancel: threading.Event) -> List[CustomMap]:
    """Unpacks a map (installer .exe, archive or folder) into the output
    folder. Its installer is never run, and nothing goes into World at War's
    own folders."""
    src = Path(src)
    if not src.exists():
        raise SetupError(f"{src} does not exist.")
    staged = src
    tmp = scratch / "map-unpack"
    if src.is_file():
        if tmp.exists():
            safe_rmtree(tmp, scratch, [src], log)
        tmp.mkdir(parents=True)
        log.info(f"Unpacking {src.name} (the installer is not run)...")
        _unpack(src, tmp, unpackers, runner)
        staged = tmp
    mods, usermaps = _map_content(staged)
    if not mods and not usermaps:
        raise SetupError(f"No World at War mod or map (mod.ff or a usermaps folder) found in {src.name}.")
    installed = {}
    for kind, folders in (("mods", mods), ("usermaps", usermaps)):
        for folder in folders:
            target = Path(output) / CUSTOM_MAPS / folder.name
            copy_tree(folder, target / kind, log, cancel, f"{folder.name}/{kind}")
            installed[folder.name.lower()] = CustomMap(folder.name, target)
    if src.is_file() and tmp.exists():
        safe_rmtree(tmp, scratch, [src], log)
    log.info("Installed: " + ", ".join(m.name for m in installed.values()))
    return list(installed.values())


def remove_custom_map(m: CustomMap, output: Path, log: Log) -> None:
    safe_rmtree(m.folder, Path(output) / CUSTOM_MAPS, [], log)


__all__ = [
    "STEAM", "XBOX360", "PC_DISC", "NONE", "KIND_LABELS", "OUTPUT_DATA", "CUSTOM_MAPS", "MODS_ENABLED",
    "STOCK_ZOMBIE_MAPS",
    "Source", "CustomMap", "Unpackers", "Cancelled", "validate_install", "find_steamapps", "find_steam_waw",
    "steam_libraries",
    "check_xbox360_image", "check_pc_disc", "choose_source", "check_source", "guess_kind", "fastfiles",
    "prepare", "unpack_pc_disc", "list_custom_maps", "install_custom_map", "remove_custom_map",
]

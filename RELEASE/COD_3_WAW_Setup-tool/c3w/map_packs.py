"""World at War map packs (DLC) from the official PC patches.

The Xbox 360 disc and the PC disc don't have the map packs. The Steam version
has all of them, so with a Steam folder no patch is used: by the owner's
rule, the Steam folder is then the only source.

The official PC patches bring the map packs as ordinary fastfiles. The user
downloads a patch (the GUI's "Download page" button opens the page for it).
This module takes the game files out of the download without running its
installer, trying each way in turn:

1. a .zip download is opened with Python's zipfile;
2. the installer inside it is unpacked with Python's zipfile (when it's a zip
   self-extractor), 7-Zip, unshield (InstallShield cabinets) or innoextract
   (Inno Setup): whichever opens it;
3. as a last resort, fastfiles stored uncompressed inside the installer are
   found by their header and cut out.

Only fastfiles (zone/<language>/*.ff) and loose-file archives (main/*.iwd)
are kept. The pipeline then rips just the fastfiles the main source lacks.
`describe()` reports what a download is made of, for the log.
"""
from __future__ import annotations

import os
import re
import shutil
import threading
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

from .common import Log, Runner, SetupError, default_input_dir, human, is_within, safe_rmtree
from .waw import OUTPUT_DATA

# Where the files taken out of the patches go, in the output folder.
OUTPUT = OUTPUT_DATA / "map_packs"


@dataclass(frozen=True)
class KnownPatch:
    title: str
    page: str                        # where to download it (opened in the browser)
    archive: str                     # the file name of the download
    archive_bytes: int
    archive_md5: str
    installer: str                   # the installer inside the download
    maps: Tuple[Tuple[str, str], ...]  # (name in game, zone name)


# Facts from the ModDB pages (see docs/SOURCES.md). The installer names are
# the ones the patch lists give. A zone name is filled in only where a
# source gave it; the rest show up once the patch is unpacked.
MODDB = "https://www.moddb.com/games/call-of-duty-world-at-war/downloads/"
MAP_PACK_1 = KnownPatch(
    title="Patch 1.4 + Map Pack 1",
    page=MODDB + "cod-waw-12-to-14-patch-map-pack-1",
    archive="codwaw-v1.2_1.4.zip",
    archive_bytes=608_022_898,
    archive_md5="9069570bef4e05296d9818fcf6a7b0f2",
    installer="CoDWaW-1.2-1.4-PatchSetup.exe",
    maps=(("Knee Deep", "mp_kneedeep"), ("Nightfire", "mp_nachtfeuer"), ("Station", "mp_subway"),
          ("Verruckt", "nazi_zombie_asylum")),
)
MAP_PACK_2 = KnownPatch(
    title="Patch 1.5 + Map Pack 2",
    page=MODDB + "cod-waw-14-to-15-patch-map-pack-2",
    archive="codwaw-v1.4_1.5.zip",
    archive_bytes=1_019_315_298,
    archive_md5="c60912dc72b7cefc8233a0064b17a2da",
    installer="CoDWaW-1.4-1.5_PatchSetup.exe",
    maps=(("Banzai", ""), ("Corrosion", ""), ("Sub Pens", ""), ("Shi No Numa", "nazi_zombie_sumpf")),
)
MAP_PACK_3 = KnownPatch(
    title="Patch 1.6 + Map Pack 3",
    page=MODDB + "cod-waw-15-to-16-patch-map-pack-3",
    archive="codwaw-v1.5_1.6.zip",
    archive_bytes=1_295_780_255,
    archive_md5="2e361036e9652bb48a827eb240419c80",
    installer="CoDWaW-1.5-1.6-PatchSetup.exe",
    maps=(("Battery", "mp_drum"), ("Breach", "mp_bgate"), ("Revolution", "mp_vodka"),
          ("Der Riese", "nazi_zombie_factory")),
)
# In patch order: a later patch's file replaces an earlier one's.
KNOWN_PATCHES = (MAP_PACK_1, MAP_PACK_2, MAP_PACK_3)


def patch_order(downloads: Iterable[Path]) -> List[Path]:
    """Known patches in release order, then any others by name."""
    order = {p.archive.lower(): i for i, p in enumerate(KNOWN_PATCHES)}
    order.update({p.installer.lower(): i for i, p in enumerate(KNOWN_PATCHES)})
    return sorted((Path(d) for d in downloads), key=lambda d: (order.get(d.name.lower(), len(order)), d.name.lower()))

# Fastfile headers (see docs/formats/waw.md): magic, then version 387.
FF_MAGICS = (b"IWffu100", b"IWff0100")
FF_VERSION = 387
PC_FF_HEADER = b"IWffu100" + FF_VERSION.to_bytes(4, "little")
GAME_FILE_SUFFIXES = (".ff", ".iwd")
# Containers the unpacking looks inside, a level at a time.
CONTAINER_SUFFIXES = (".zip", ".exe", ".7z", ".msi", ".cab", ".rar")
MAX_DEPTH = 3

# Markers reported by describe(), to tell what made an installer.
SIGNATURES: Dict[bytes, str] = {
    b"Nullsoft": "NSIS installer",
    b"Inno Setup Setup Data": "Inno Setup installer",
    b"InstallShield": "InstallShield",
    b"ISc(": "InstallShield cabinet",
    b"MSCF\0\0\0\0": "Microsoft cabinet",
    b"7z\xbc\xaf\x27\x1c": "7-Zip archive",
    b"PK\x03\x04": "zip entry",
    b"Rar!\x1a\x07": "RAR archive",
    b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1": "MSI (compound file)",
    b"RTPatch": "RTPatch data",
    b"IWffu100": "World at War fastfile",
    b"IWff0100": "signed fastfile",
}
CHUNK = 16 * 1024 * 1024


# ---------------------------------------------------------------------------
# Finding and recognizing downloads
# ---------------------------------------------------------------------------
def identify(path: Path) -> Optional[KnownPatch]:
    """The known patch this download is, by its name or exact size."""
    path = Path(path)
    name = path.name.lower()
    size = path.stat().st_size if path.is_file() else -1
    for patch in KNOWN_PATCHES:
        if name in (patch.archive.lower(), patch.installer.lower()) or size == patch.archive_bytes:
            return patch
    return None


def looks_like_patch(path: Path) -> bool:
    name = Path(path).name.lower()
    return bool(identify(path)) or bool(re.match(r"codwaw.*(patch|v1\.\d).*\.(zip|exe)$", name))


def find_downloads(folders: Iterable[Path] = ()) -> List[Path]:
    """Patch downloads in the input folder, the user's Downloads folder and
    `folders`, in patch order."""
    found: List[Path] = []
    for folder in [default_input_dir(), Path.home() / "Downloads", *folders]:
        try:
            entries = list(Path(folder).iterdir())
        except OSError:
            continue
        found += [p for p in entries if p.is_file() and looks_like_patch(p)]
    return patch_order(set(found))


def is_fastfile(path: Path) -> bool:
    """A World at War fastfile (PC or Xbox 360), by its header."""
    try:
        with open(path, "rb") as f:
            head = f.read(12)
    except OSError:
        return False
    if len(head) < 12 or head[:8] not in FF_MAGICS:
        return False
    return FF_VERSION in (int.from_bytes(head[8:12], "little"), int.from_bytes(head[8:12], "big"))


# ---------------------------------------------------------------------------
# Describing a download (for the log)
# ---------------------------------------------------------------------------
def scan_signatures(path: Path, signatures: Dict[bytes, str] = SIGNATURES,
                    cancel: Optional[threading.Event] = None) -> Dict[bytes, List[int]]:
    """Offsets of each marker in the file (all of them, read in chunks)."""
    found: Dict[bytes, List[int]] = {s: [] for s in signatures}
    overlap = max(len(s) for s in signatures) - 1
    with open(path, "rb") as f:
        base, tail = 0, b""
        while True:
            if cancel is not None and cancel.is_set():
                break
            block = f.read(CHUNK)
            if not block:
                break
            data = tail + block
            start = base - len(tail)
            for sig, offsets in found.items():
                i = data.find(sig)
                while i >= 0:
                    # A match wholly inside the kept tail was found last time.
                    if i + len(sig) > len(tail):
                        offsets.append(start + i)
                    i = data.find(sig, i + 1)
            base += len(block)
            tail = data[-overlap:]
    return found


def pe_overlay(path: Path) -> Optional[int]:
    """Where the data appended to a Windows program starts (installers keep
    their payload there), or None if this isn't a Windows program."""
    with open(path, "rb") as f:
        head = f.read(4096)
    if head[:2] != b"MZ" or len(head) < 0x40:
        return None
    pe = int.from_bytes(head[0x3C:0x40], "little")
    if head[pe:pe + 4] != b"PE\0\0":
        return None
    sections = int.from_bytes(head[pe + 6:pe + 8], "little")
    optional_size = int.from_bytes(head[pe + 20:pe + 22], "little")
    table = pe + 24 + optional_size
    end = 0
    for i in range(sections):
        s = table + 40 * i
        raw_size = int.from_bytes(head[s + 16:s + 20], "little")
        raw_pointer = int.from_bytes(head[s + 20:s + 24], "little")
        end = max(end, raw_pointer + raw_size)
    return end


def describe(path: Path, cancel: Optional[threading.Event] = None) -> List[str]:
    """What a download is made of: lines for the log."""
    path = Path(path)
    lines = [f"{path.name}: {human(path.stat().st_size)} ({path.stat().st_size} bytes)"]
    patch = identify(path)
    if patch:
        lines.append(f"  known download: {patch.title} (maps: {', '.join(n for n, _ in patch.maps)})")
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            members = z.infolist()
        lines.append(f"  zip archive, {len(members)} entries:")
        for m in members[:40]:
            lines.append(f"    {m.filename}  {m.file_size} bytes (stored as {m.compress_size}, method {m.compress_type})")
        if len(members) > 40:
            lines.append(f"    ... and {len(members) - 40} more")
        if path.suffix.lower() == ".zip":
            return lines
    overlay = pe_overlay(path)
    if overlay is not None:
        lines.append(f"  Windows program; appended data starts at {overlay:#x}")
        with open(path, "rb") as f:
            f.seek(overlay)
            lines.append(f"  first bytes there: {f.read(32).hex(' ')}")
    for sig, offsets in scan_signatures(path, cancel=cancel).items():
        if offsets:
            shown = ", ".join(f"{o:#x}" for o in offsets[:4])
            lines.append(f"  {SIGNATURES[sig]}: {len(offsets)} found (first at {shown})")
    return lines


# ---------------------------------------------------------------------------
# Taking the files out
# ---------------------------------------------------------------------------
@dataclass
class Unpackers:
    """External unpackers the setup tool installs (any may be missing)."""
    sevenzip: Optional[Path] = None
    unshield: Optional[Path] = None
    innoextract: Optional[Path] = None


def _has_game_files(folder: Path) -> bool:
    return any(f.lower().endswith(GAME_FILE_SUFFIXES) for _, _, files in os.walk(folder) for f in files)


def _extract_zip(archive: Path, dest: Path) -> None:
    with zipfile.ZipFile(archive) as z:
        for m in z.infolist():
            if not is_within(dest / m.filename, dest):
                raise SetupError(f"Unsafe path in {archive.name}: {m.filename}")
        z.extractall(dest)


def _try_tools(container: Path, dest: Path, unpackers: Unpackers, runner: Runner, log: Log) -> None:
    """Unpacks an installer or archive with each tool in turn, until one
    gives game files. (7-Zip opens any Windows program, so "it opened" alone
    doesn't mean it found the installer's files.)"""
    attempts = []
    if unpackers.sevenzip:
        attempts.append(("7-Zip", [unpackers.sevenzip, "x", "-y", f"-o{dest / '7z'}", container]))
    if unpackers.innoextract and container.suffix.lower() == ".exe":
        attempts.append(("innoextract", [unpackers.innoextract, "--silent", "--extract", "--output-dir",
                                         dest / "inno", container]))
    for name, cmd in attempts:
        log.info(f"Trying {name} on {container.name}...")
        runner.run(cmd, check=False, quiet=True)
        if _has_game_files(dest):
            break
    # InstallShield keeps its files in data1.cab (with data1.hdr), which unshield opens.
    cabinets = [p for p in dest.rglob("*") if p.name.lower() == "data1.cab"]
    if cabinets and unpackers.unshield:
        log.info("Unpacking InstallShield cabinets with unshield...")
        runner.run([unpackers.unshield, "-d", dest / "unshield", "x", cabinets[0]], check=False, quiet=True)


def _unpack(container: Path, dest: Path, unpackers: Unpackers, runner: Runner, log: Log,
            cancel: threading.Event, depth: int = 0) -> None:
    """Unpacks `container` into `dest`, then any installer or archive found in
    it, until game files turn up."""
    if cancel.is_set() or depth > MAX_DEPTH:
        return
    dest.mkdir(parents=True, exist_ok=True)
    if depth > 0 and container.suffix.lower() == ".exe":
        for line in describe(container, cancel):
            log.info(line)
    plain_zip = False
    if zipfile.is_zipfile(container):
        log.info(f"Opening {container.name} as a zip archive...")
        try:
            _extract_zip(container, dest)
            plain_zip = container.suffix.lower() == ".zip"
        except (zipfile.BadZipFile, OSError) as e:
            log.warn(f"{container.name}: {e}")
    if not _has_game_files(dest) and not plain_zip:
        _try_tools(container, dest, unpackers, runner, log)
    if _has_game_files(dest):
        return
    inner = [p for p in dest.rglob("*") if p.is_file() and p.suffix.lower() in CONTAINER_SUFFIXES]
    for p in sorted(inner, key=lambda p: -p.stat().st_size):
        _unpack(p, p.parent / (p.name + ".unpacked"), unpackers, runner, log, cancel, depth + 1)
        if _has_game_files(dest):
            return


def _name_from_zone(text: bytes) -> Optional[str]:
    m = re.search(rb"maps/(?:mp/)?([A-Za-z0-9_]+)\.d3dbsp", text)
    if m:
        return m.group(1).decode().lower()
    m = re.search(rb"loadscreen_([A-Za-z0-9_]+)", text)
    if m:
        return m.group(1).decode().lower() + "_load"
    return None


def carve_fastfiles(container: Path, dest: Path, log: Log, cancel: threading.Event) -> List[Path]:
    """Cuts out the PC fastfiles stored uncompressed inside `container`. Each
    one is found by its header and ends where its zlib stream ends. Their
    names aren't stored, so each is named after the map it holds when that
    can be read from the zone, or after where it was found."""
    starts = scan_signatures(container, {PC_FF_HEADER: "fastfile"}, cancel)[PC_FF_HEADER]
    dest.mkdir(parents=True, exist_ok=True)
    out: List[Path] = []
    with open(container, "rb") as f:
        for start in starts:
            if cancel.is_set():
                break
            f.seek(start + len(PC_FF_HEADER))
            inflater = zlib.decompressobj()
            fed, name, tail = 0, None, b""
            try:
                while not inflater.eof:
                    block = f.read(1024 * 1024)
                    if not block:
                        break
                    fed += len(block)
                    zone = inflater.decompress(block)
                    if name is None:
                        name = _name_from_zone(tail + zone)
                        tail = zone[-256:]
            except zlib.error:
                continue
            if not inflater.eof:
                continue
            end = start + len(PC_FF_HEADER) + fed - len(inflater.unused_data)
            stem = name or f"fastfile_{start:x}"
            target = dest / f"{stem}.ff"
            n = 2
            while target.exists():
                target = dest / f"{stem}_{n}.ff"
                n += 1
            with open(target, "wb") as w:
                f.seek(start)
                remaining = end - start
                while remaining:
                    block = f.read(min(remaining, CHUNK))
                    w.write(block)
                    remaining -= len(block)
            log.info(f"  found {target.name} at {start:#x} ({human(end - start)})")
            out.append(target)
    return out


def _place(found: Path, dest: Path) -> Path:
    """Where a game file goes: under dest/zone/ or dest/main/, keeping the
    path from the zone or main folder on when it has one."""
    parts = [p.lower() for p in found.parts]
    for marker in ("zone", "main"):
        if marker in parts:
            i = len(parts) - 1 - parts[::-1].index(marker)
            return dest.joinpath(*found.parts[i:])
    return dest / ("zone" if found.suffix.lower() == ".ff" else "main") / found.name


def extract(patch: Path, dest: Path, scratch: Path, unpackers: Unpackers, runner: Runner, log: Log,
            cancel: threading.Event) -> List[Path]:
    """Takes the fastfiles and .iwd files out of a patch download into
    dest/zone/ and dest/main/. Returns the fastfiles."""
    patch = Path(patch)
    if not patch.is_file():
        raise SetupError(f"Map pack download not found: {patch}")
    for line in describe(patch, cancel):
        log.info(line)
    work = scratch / "map-pack-unpack"
    if work.exists():
        safe_rmtree(work, scratch, [patch, dest], log)
    try:
        _unpack(patch, work, unpackers, runner, log, cancel)
        if not _has_game_files(work):
            log.info("No unpacker opened it; looking for fastfiles stored inside the installer...")
            installers = [p for p in work.rglob("*") if p.is_file() and p.suffix.lower() == ".exe"] or [patch]
            for installer in installers:
                carve_fastfiles(installer, work / "carved" / "zone", log, cancel)
        placed: List[Path] = []
        skipped = 0
        for root, dirs, files in os.walk(work):
            dirs[:] = [d for d in dirs if d.lower() != "mods"]
            for name in files:
                found = Path(root) / name
                if not name.lower().endswith(GAME_FILE_SUFFIXES):
                    continue
                whole = is_fastfile(found) if name.lower().endswith(".ff") else zipfile.is_zipfile(found)
                if not whole:
                    skipped += 1   # a patch's difference data, not a whole file
                    continue
                target = _place(found.relative_to(work), dest)
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    target.unlink()   # from an earlier patch: the later one wins
                shutil.move(str(found), str(target))
                placed.append(target)
    finally:
        if work.exists():
            safe_rmtree(work, scratch, [patch, dest], log)
    if skipped:
        log.warn(f"{skipped} .ff/.iwd files in {patch.name} aren't whole files (patch data); they were skipped.")
    fastfiles = sorted(p for p in placed if p.suffix.lower() == ".ff")
    if not fastfiles:
        raise SetupError(f"No World at War fastfiles could be taken out of {patch.name}.\n"
                         "The log above says what the download is made of; please report it.")
    log.info(f"Took {len(fastfiles)} fastfiles and {len(placed) - len(fastfiles)} .iwd files out of {patch.name}.")
    return fastfiles


def missing_from(source: List[Path], extra: List[Path]) -> Tuple[List[Path], List[str]]:
    """The fastfiles of `extra` that `source` doesn't have (by name), and the
    names of the ones it already has."""
    have = {p.stem.lower() for p in source}
    new = [p for p in extra if p.stem.lower() not in have]
    return new, sorted({p.stem for p in extra if p.stem.lower() in have})


__all__ = ["OUTPUT", "KnownPatch", "MAP_PACK_1", "MAP_PACK_2", "MAP_PACK_3", "KNOWN_PATCHES", "patch_order",
           "Unpackers", "identify", "looks_like_patch", "find_downloads", "is_fastfile", "scan_signatures", "pe_overlay", "describe", "carve_fastfiles",
           "extract", "missing_from"]

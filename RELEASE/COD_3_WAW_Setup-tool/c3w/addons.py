"""[acct2-decomp] Add-ons for Call of Duty 3: the Xbox 360 map packs now, mods
later. Set up's "Your disc" section has the button; this file does the work.

A map pack is a marketplace download (an STFS "LIVE" package, title
415607E1, content type 2). The game finds it the way the console did: as a
folder named after the package, in the content area of the shared userdata
folder that cod3 and cod3mp both use:

    <output>/userdata/0000000000000000/415607E1/00000002/<package name>/mp/...

The game reads mp/dlmaps.txt there for the maps' names: "//" comments, then
one line per map and language, 7 columns lined up with tabs (pack, id, menu
name, map folder, loading-screen title and place, language). Nothing needs a
licence: neither program asks for one.

Safety (docs: dev/agents/acct2-decomp/PLAN-MAPS-DISCS-LANGUAGES.md, part 3):
- Only packs whose SHA-1 is known are installed (KNOWN_PACKS); anything else
  is refused. Title and content type are checked too.
- Packages are read by stfs.py, which checks every number and name in them.
  Only files under mp/ with the extensions the packs use (.cod, .wbk, .txt)
  are copied; nothing is ever run.
- dlmaps.txt is checked line by line before anything is copied: 7 fields, printable text only, no "%" (the game formats these strings),
  sensible lengths, and each map's folder in the package.
- Zip downloads are never unpacked by name: each member is only looked at, and
  a package found in one is copied to the scratch folder under a name of our
  own, with its size counted as it's read.
- Installs go to a new folder first and replace the old one only when every
  file is in; a cancelled or failed add leaves the old one as it was.
- Removing only deletes folders this file installed (listed in addons.json),
  inside userdata.

The list of what's installed is userdata/addons.json. Its "kind" is
"map_pack" for now; "mod" is kept for the mod support to come (mods will need
their own checks before anything installs them).
"""
from __future__ import annotations

import hashlib
import os
import re
import threading
import time
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional

from . import common
from .common import Cancelled, Log, SetupError
from .stfs import HEADER_READ, MAGICS, StfsError, StfsPackage

TITLE_ID = 0x415607E1
CONTENT_MARKETPLACE = 0x00000002
CONTENT_DIR = Path("0000000000000000") / f"{TITLE_ID:08X}" / f"{CONTENT_MARKETPLACE:08X}"
REGISTRY = "addons.json"
KINDS = ("map_pack", "mod")       # "mod": reserved, nothing installs one yet
ALLOWED_EXT = {".cod", ".wbk", ".txt"}
DLMAPS = "mp/dlmaps.txt"
PACKAGE_NAME = re.compile(r"^[0-9A-F]{40,42}$")
MAX_PACKAGE = 2 << 30             # the biggest pack is under 1 GB
MAX_ZIP_TOTAL = 8 << 30
MAX_ZIP_RATIO = 200               # packages hardly compress; a huge ratio is a zip bomb
SPARE_SPACE = 64 << 20


@dataclass(frozen=True)
class KnownPack:
    name: str
    package: str       # the package's file name, also its folder name in userdata
    size: int


# The Xbox 360 releases of the map packs (SHA-1 of the whole package file).
KNOWN_PACKS: Dict[str, KnownPack] = {
    "25d430492bb8c5ea6e323af9d4a460fe12f1cd5f": KnownPack("Champs map", "BE285241D408C51237C35315B633A7777E18071341",
                                                          176078848),
    "4b673c286073745d0afafdc0677028a8d8febabe": KnownPack("Valor map pack",
                                                          "8DA6A35A055E7456B8F5B53786A55813CA508E1241", 895438848),
    "b1c9f6a00734f5381d09a022856393a15a844841": KnownPack("Bravo map pack",
                                                          "86767399401D3377CB1CDFA61991BEF72ADCE4FC41", 996433920),
}


@dataclass
class MapRow:
    pack: str
    id: str
    display: str
    pak: str
    title: str
    location: str
    language: str


@dataclass
class Addon:
    id: str
    kind: str
    name: str
    sha1: str
    folder: str                      # relative to userdata, "/" separated
    maps: List[str] = field(default_factory=list)
    files: int = 0
    bytes: int = 0
    installed: str = ""
    present: bool = True             # its folder is still there

    def to_json(self) -> dict:
        return {k: getattr(self, k) for k in ("id", "kind", "name", "sha1", "folder", "maps", "files", "bytes",
                                              "installed")}


# ---------------------------------------------------------------- dlmaps.txt
_FIELD_OK = re.compile(r"^[\x20-\x7e\xa0-\xff]*$")   # printable Latin-1 (Valor has an É)
_PAK_OK = re.compile(r"^[A-Za-z0-9_]{1,32}$")


def parse_dlmaps(data: bytes) -> List[MapRow]:
    """The map list of a pack, checked; SetupError says what is wrong."""
    if len(data) > 64 << 10:
        raise SetupError("dlmaps.txt is far too big")
    text = data.decode("latin-1")
    rows: List[MapRow] = []
    for n, line in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        if not line.strip() or line.lstrip().startswith("//"):
            continue                                   # blank lines and "//" comments
        # Columns are lined up with runs of tabs (and lines start with one), so
        # empty pieces between tabs aren't fields.
        f = [v for v in line.split("\t") if v.strip()]
        if len(f) != 7:
            raise SetupError(f"dlmaps.txt line {n}: {len(f)} fields, not 7")
        if f[0].strip().lower() == "mappack":
            continue                                   # a header line
        for v in f:
            if len(v) > 64 or not _FIELD_OK.match(v) or "%" in v:
                raise SetupError(f"dlmaps.txt line {n}: a field has characters or a length the game can't take")
        row = MapRow(*[v.strip() for v in f])
        if not _PAK_OK.match(row.pak):
            raise SetupError(f"dlmaps.txt line {n}: odd map name {row.pak!r}")
        if not re.match(r"^[0-9]{1,6}$", row.id):
            raise SetupError(f"dlmaps.txt line {n}: odd map id {row.id!r}")
        if not re.match(r"^[A-Za-z]{2}$", row.language):
            raise SetupError(f"dlmaps.txt line {n}: odd language {row.language!r}")
        rows.append(row)
        if len(rows) > 256:
            raise SetupError("dlmaps.txt has too many lines")
    if not rows:
        raise SetupError("dlmaps.txt lists no maps")
    return rows


# ---------------------------------------------------------------- checking a package
@dataclass
class Checked:
    path: Path
    sha1: str
    known: KnownPack
    maps: List[str]
    files: int
    bytes: int


def _sha1(path: Path, cancel: threading.Event) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            if cancel.is_set():
                raise Cancelled()
            h.update(chunk)
    return h.hexdigest()


def check_package(path: Path, cancel: threading.Event, sha1: Optional[str] = None) -> Checked:
    """Everything about a package that can be checked without installing it."""
    path = Path(path)
    size = path.stat().st_size
    if size > MAX_PACKAGE:
        raise SetupError(f"{path.name}: too big to be a map pack ({common.human(size)})")
    sha1 = sha1 or _sha1(path, cancel)
    known = KNOWN_PACKS.get(sha1)
    if known is None:
        raise SetupError(f"{path.name} is not a map pack this tool knows (SHA-1 {sha1}). Only the Xbox 360 "
                         "Call of Duty 3 map packs are added; a changed or unknown package is refused.")
    if size != known.size:
        raise SetupError(f"{path.name}: the size doesn't match the {known.name}")
    try:
        with StfsPackage(path) as pkg:
            if pkg.title_id != TITLE_ID or pkg.content_type != CONTENT_MARKETPLACE:
                raise SetupError(f"{path.name}: not Call of Duty 3 marketplace content "
                                 f"(title {pkg.title_id:08X}, type {pkg.content_type:08X})")
            entries = pkg.entries()
            files = [e for e in entries if not e.is_dir]
            for e in entries:
                top = e.path.split("/")[0].lower()
                if top != "mp":
                    raise SetupError(f"{path.name}: unexpected file {e.path}")
                if not e.is_dir and Path(e.path).suffix.lower() not in ALLOWED_EXT:
                    raise SetupError(f"{path.name}: unexpected file {e.path}")
            total = sum(e.size for e in files)
            if total > size:
                raise SetupError(f"{path.name}: its files add up to more than the package")
            dl = [e for e in files if e.path.lower() == DLMAPS]
            if len(dl) != 1:
                raise SetupError(f"{path.name}: no mp/dlmaps.txt")
            rows = parse_dlmaps(pkg.read_file(dl[0], 64 << 10))
            folders = {e.path.lower() for e in entries if e.is_dir}
            maps = sorted({r.pak for r in rows}, key=str.lower)
            for m in maps:
                if f"mp/{m.lower()}" not in folders:
                    raise SetupError(f"{path.name}: dlmaps.txt lists {m}, but the package has no folder for it")
    except StfsError as e:
        raise SetupError(f"{path.name}: {e}") from None
    return Checked(path, sha1, known, maps, len(files), total)


# ---------------------------------------------------------------- finding packages
def _looks_like_package(head: bytes) -> bool:
    return head[:4] in MAGICS


def _copy_member(z: zipfile.ZipFile, info: zipfile.ZipInfo, dst: Path, cancel: threading.Event) -> str:
    """Copy one zip member to dst, counting bytes; its SHA-1."""
    h = hashlib.sha1()
    n = 0
    with z.open(info) as src, open(dst, "wb") as out:
        for chunk in iter(lambda: src.read(1 << 20), b""):
            if cancel.is_set():
                raise Cancelled()
            n += len(chunk)
            if n > info.file_size or n > MAX_PACKAGE:
                raise SetupError(f"{info.filename}: bigger than the zip says")
            h.update(chunk)
            out.write(chunk)
    if n != info.file_size:
        raise SetupError(f"{info.filename}: shorter than the zip says")
    return h.hexdigest()


def packages_in_zip(archive: Path, scratch: Path, cancel: threading.Event, log: Log) -> List[tuple]:
    """(temporary package file, sha1) for each package in a zip; the caller
    deletes the files. Members are never written under their own names."""
    out: List[tuple] = []
    try:
        z = zipfile.ZipFile(archive)
    except (zipfile.BadZipFile, OSError) as e:
        raise SetupError(f"{archive.name}: not a zip file I can read ({e})") from None
    with z:
        infos = [i for i in z.infolist() if not i.is_dir()]
        if sum(i.file_size for i in infos) > MAX_ZIP_TOTAL:
            raise SetupError(f"{archive.name}: unpacks to too much")
        for info in infos:
            if info.file_size < HEADER_READ or info.file_size > MAX_PACKAGE:
                continue
            if info.compress_size and info.file_size / info.compress_size > MAX_ZIP_RATIO:
                raise SetupError(f"{archive.name}: {info.filename} is packed suspiciously tightly")
            if info.flag_bits & 0x1:
                raise SetupError(f"{archive.name}: password-protected")
            with z.open(info) as f:
                head = f.read(4)
            if not _looks_like_package(head):
                continue
            scratch.mkdir(parents=True, exist_ok=True)
            dst = scratch / f"addon-{len(out)}-{int(time.time() * 1000)}.stfs"
            log.info(f"Found a package in {archive.name}: {info.filename} ({common.human(info.file_size)})")
            try:
                out.append((dst, _copy_member(z, info, dst, cancel)))
            except BaseException:
                dst.unlink(missing_ok=True)
                for p, _ in out:
                    p.unlink(missing_ok=True)
                raise
    return out


def find_sources(paths: Iterable[Path], max_depth: int = 6) -> List[Path]:
    """Files to look at from what the user picked: files as they are, and the
    files in picked folders (not following links, at most max_depth deep)."""
    found: List[Path] = []
    for p in paths:
        p = Path(p)
        if p.is_file():
            found.append(p)
        elif p.is_dir():
            base = len(p.parts)
            for root, dirs, names in os.walk(p, followlinks=False):
                r = Path(root)
                if len(r.parts) - base >= max_depth:
                    dirs[:] = []
                dirs.sort()
                for n in sorted(names):
                    q = r / n
                    if not q.is_symlink() and q.is_file():
                        found.append(q)
    return found


# ---------------------------------------------------------------- the registry
def userdata(output: Path) -> Path:
    return Path(output) / "userdata"


def read_registry(output: Path) -> List[Addon]:
    data = common.read_json(userdata(output) / REGISTRY)
    out: List[Addon] = []
    for a in data.get("addons", []) if isinstance(data, dict) else []:
        try:
            item = Addon(**{k: a[k] for k in ("id", "kind", "name", "sha1", "folder")},
                         maps=list(a.get("maps", [])), files=int(a.get("files", 0)), bytes=int(a.get("bytes", 0)),
                         installed=str(a.get("installed", "")))
        except (KeyError, TypeError, ValueError):
            continue
        if (item.kind not in KINDS or not PACKAGE_NAME.match(item.id)
                or item.folder != (CONTENT_DIR / item.id).as_posix()):
            continue                          # only folders this file would have made
        item.present = (userdata(output) / item.folder).is_dir()
        out.append(item)
    return out


def _write_registry(output: Path, items: List[Addon]) -> None:
    common.write_json(userdata(output) / REGISTRY, {"version": 1, "addons": [a.to_json() for a in items]})


# ---------------------------------------------------------------- install / remove
def _install(c: Checked, output: Path, cancel: threading.Event, log: Log) -> Addon:
    root = userdata(output)
    content = root / CONTENT_DIR
    content.mkdir(parents=True, exist_ok=True)
    name = c.known.package
    if not PACKAGE_NAME.match(name):          # never from the package itself, but check anyway
        raise SetupError(f"odd package name {name!r}")
    final, new, old = content / name, content / f".{name}.c3w-new", content / f".{name}.c3w-old"
    for p in (new, old):
        common.safe_rmtree(p, root, [], log)
    if common.free_space(content) < c.bytes + SPARE_SPACE:
        raise SetupError(f"Not enough free space for the {c.known.name} ({common.human(c.bytes)}) in {content}")
    log.step(f"Adding the {c.known.name} ({', '.join(c.maps)})")
    done = 0
    try:
        with StfsPackage(c.path) as pkg:
            entries = pkg.entries()
            new.mkdir()
            for e in entries:
                target = new.joinpath(*e.path.split("/"))
                if not common.is_within(target, new):
                    raise SetupError(f"unsafe path {e.path}")
                if e.is_dir:
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)

                def check():
                    if cancel.is_set():
                        raise Cancelled()
                with open(target, "xb") as out:
                    pkg.copy_file(e, out.write, check)
                done += e.size
                log.progress(done / max(c.bytes, 1), f"{c.known.name}: {e.path}")
        if final.exists():
            final.rename(old)
        new.rename(final)
        common.safe_rmtree(old, root, [], log)
    except BaseException:
        common.safe_rmtree(new, root, [], log)
        if old.exists() and not final.exists():
            old.rename(final)
        raise
    return Addon(id=name, kind="map_pack", name=c.known.name, sha1=c.sha1, folder=(CONTENT_DIR / name).as_posix(),
                 maps=c.maps, files=c.files, bytes=c.bytes, installed=time.strftime("%Y-%m-%d %H:%M:%S"))


def add(sources: Iterable[Path], output: Path, scratch: Path, cancel: threading.Event, log: Log) -> List[Addon]:
    """Install every known map pack found in `sources` (packages, zips holding
    them, or folders of either). Returns what was added; SetupError if none."""
    output = Path(output)
    if not output.is_dir():
        raise SetupError("Choose the output folder on Set up first (the packs go into its userdata folder).")
    files = find_sources(sources)
    if not files:
        raise SetupError("Nothing to add there.")
    added: List[Addon] = []
    refused: List[str] = []
    for f in files:
        if cancel.is_set():
            raise Cancelled()
        if common.is_within(f, userdata(output)):
            continue                                    # already installed files
        temps: List[tuple] = []
        try:
            with open(f, "rb") as fh:
                head = fh.read(4)
            if _looks_like_package(head):
                candidates = [(f, None)]
            elif zipfile.is_zipfile(f):
                candidates = temps = packages_in_zip(f, Path(scratch), cancel, log)
            else:
                continue
            for pkg, sha in candidates:
                try:
                    c = check_package(pkg, cancel, sha)
                except SetupError as e:
                    refused.append(str(e))
                    log.warn(str(e))
                    continue
                a = _install(c, output, cancel, log)
                items = [x for x in read_registry(output) if x.id != a.id] + [a]
                _write_registry(output, items)
                added.append(a)
                log.info(f"Added the {a.name}: {', '.join(a.maps)} ({a.files} files, {common.human(a.bytes)})")
        except OSError as e:
            refused.append(f"{f.name}: {e}")
        finally:
            for p, _ in temps:
                Path(p).unlink(missing_ok=True)
    if not added:
        raise SetupError("No map pack was added.\n" + ("\n".join(refused) if refused else
                                                        "No Xbox 360 package was found there."))
    return added


def remove(output: Path, addon_id: str, log: Log) -> None:
    """Delete an add-on this tool installed, and its line in addons.json."""
    items = read_registry(output)
    match = [a for a in items if a.id == addon_id]
    if not match:
        raise SetupError(f"{addon_id} isn't in {REGISTRY}")
    a = match[0]
    root = userdata(output)
    folder = root / a.folder
    if not PACKAGE_NAME.match(folder.name) or not common.is_within(folder, root / CONTENT_DIR):
        raise SetupError(f"Refusing to delete {folder}")
    common.safe_rmtree(folder, root / CONTENT_DIR, [], log)
    _write_registry(output, [x for x in items if x.id != addon_id])


def summary(output: Optional[Path]) -> str:
    """One line for Set up: what's installed."""
    if not output or not Path(output).is_dir():
        return "none"
    items = read_registry(Path(output))
    if not items:
        return "none"
    return ", ".join(a.name + ("" if a.present else " (folder missing)") for a in items)

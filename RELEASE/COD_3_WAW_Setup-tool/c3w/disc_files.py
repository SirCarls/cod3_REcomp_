"""[acct2-decomp] The Call of Duty 3 discs' own files: the MD5 and size of
every file on each known disc, and a check of a game folder (or a disc
image) against them.

The lists (c3w/disc_md5/*.tsv: md5, size, path) were made from the four
discs the project has (USA/Europe, Germany, Europe Fr/Es/It, Japan), read in
place with c3w/xiso.py, and match the files the build takes off them. They
hold only names, sizes and checksums: no game data. A disc is told by its
default.xex SHA-1. The Japanese disc is a build of its own (another
default.xex, 451 of its archives differ): it has a list here so its files can
be checked and told apart, but pipeline.KNOWN_XEX doesn't accept it for a
build.

Used by:
  - `cod3_waw_setup.py verify-files [folder or image]` (the MD5s, file by file);
  - Tools > Repair > "Check game files (MD5)" in the window;
  - repair.check_install, by size only (instant), so Repair can take a file
    off the disc again when it's missing or cut off;
  - c3w/censored.py, which swaps 84 campaign archives with the German disc's
    (a file that is another known disc's version of the same path counts as
    "from another disc", not as changed; assets/censored/ is not looked at),
    and six level archives with copies it made (listed with their MD5s in
    assets/censored/levels.tsv: such a copy counts as "censored", not as
    changed).

Nothing is changed and nothing is run: files are only read.
"""
from __future__ import annotations

import hashlib
import os
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, Iterator, List, Optional, Tuple

from .common import Cancelled, SetupError, sha1_file

DATA_DIR = Path(__file__).resolve().parent / "disc_md5"
SKIP_DIR = "censored"   # assets/censored/: spare copies kept by c3w/censored.py, not the game's files
CHUNK = 8 * 1024 * 1024


@dataclass(frozen=True)
class Disc:
    key: str          # the list's file name, without .tsv
    name: str         # as pipeline.KNOWN_XEX names it
    xex_sha1: str     # SHA-1 of its default.xex (pipeline.KNOWN_XEX)


DISCS: Tuple[Disc, ...] = (
    Disc("usa_europe", "Call of Duty 3 (USA, Europe)", "681724c40e250e584e6585dd903e05afa00a8634"),
    Disc("germany", "Call of Duty 3 (Germany)", "52447cb4189bc4168e71324255e9e7acf5495eb8"),
    Disc("japan", "Call of Duty 3 (Japan)", "44cbd1e1d4e6d2e6b3ca1fdb525713cca144a888"),
    Disc("europe_fr_es_it", "Call of Duty 3 (Europe: French, Spanish, Italian)",
         "6f9247bb74c4006632a8556dcd1d41332139ff83"),
)


@dataclass(frozen=True)
class Known:
    path: str         # as on the disc (forward slashes)
    size: int
    md5: str


_lists: Dict[str, Dict[str, Known]] = {}


def known_files(disc: Disc) -> Dict[str, Known]:
    """The disc's files by lower-case path."""
    if disc.key not in _lists:
        _lists[disc.key] = load_list(DATA_DIR / f"{disc.key}.tsv")
    return _lists[disc.key]


def load_list(path: Path) -> Dict[str, Known]:
    out: Dict[str, Known] = {}
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError as e:
        raise SetupError(f"The list of the disc's files can't be read: {path.name} ({e})")
    for n, line in enumerate(lines, 1):
        if not line.strip() or line.startswith("#") or line.startswith("md5\t"):
            continue
        parts = line.split("\t", 2)
        if len(parts) != 3 or len(parts[0]) != 32 or not parts[1].isdigit():
            raise SetupError(f"{path.name}, line {n}: not 'md5<TAB>size<TAB>path'")
        md5, size, rel = parts[0].lower(), int(parts[1]), parts[2]
        out[rel.lower()] = Known(rel, size, md5)
    return out


def disc_by_xex(sha1: str) -> Optional[Disc]:
    return next((d for d in DISCS if d.xex_sha1 == (sha1 or "").lower()), None)


def disc_by_key(key: str) -> Optional[Disc]:
    return next((d for d in DISCS if d.key == key), None)


# ---------------------------------------------------------------------------
# What is checked: a folder (a game's assets/, or the output folder) or a disc
# image, as (path, size, reader) for every file.
# ---------------------------------------------------------------------------
class _Source:
    label: str

    def files(self) -> List[Tuple[str, int]]:
        raise NotImplementedError

    def chunks(self, rel: str) -> Iterator[bytes]:
        raise NotImplementedError

    def xex_sha1(self) -> Optional[str]:
        h = hashlib.sha1()
        found = False
        for rel, _ in self.files():
            if rel.lower() == "default.xex":
                for b in self.chunks(rel):
                    h.update(b)
                found = True
                break
        return h.hexdigest() if found else None

    def close(self) -> None:
        pass


class _FolderSource(_Source):
    def __init__(self, root: Path):
        self.root = root
        self.label = str(root)
        self._files: Optional[List[Tuple[str, int]]] = None

    def files(self) -> List[Tuple[str, int]]:
        if self._files is None:
            out = []
            for dirpath, dirs, names in os.walk(self.root):
                dirs.sort()
                if Path(dirpath) == self.root and SKIP_DIR in dirs:
                    dirs.remove(SKIP_DIR)   # the censored campaign's spare copies (c3w/censored.py)
                for n in sorted(names):
                    p = Path(dirpath, n)
                    try:
                        if p.is_file():
                            out.append((p.relative_to(self.root).as_posix(), p.stat().st_size))
                    except OSError:
                        pass
            self._files = out
        return self._files

    def chunks(self, rel: str) -> Iterator[bytes]:
        with open(self.root / rel, "rb") as f:
            for b in iter(lambda: f.read(CHUNK), b""):
                yield b

    def xex_sha1(self) -> Optional[str]:
        x = self.root / "default.xex"
        return sha1_file(x) if x.is_file() else None


class _ImageSource(_Source):
    def __init__(self, image: Path):
        from .xiso import SECTOR, XisoImage
        self._img = XisoImage(image)
        self._sector = SECTOR
        self.label = image.name
        self._by = {e.path: e for e in self._img.files}

    def files(self) -> List[Tuple[str, int]]:
        return [(e.path, e.size) for e in self._img.files]

    def chunks(self, rel: str) -> Iterator[bytes]:
        e = self._by[rel]
        f = self._img._f
        f.seek(self._img.base + e.sector * self._sector)
        left = e.size
        while left:
            b = f.read(min(CHUNK, left))
            if not b:
                raise SetupError(f"The image is truncated (while reading {rel}).")
            left -= len(b)
            yield b

    def close(self) -> None:
        self._img.close()


def game_files_folder(target: Path) -> Path:
    """A game's files from the disc: assets/ of an output folder, or the folder itself."""
    target = Path(target)
    if (target / "assets" / "default.xex").is_file():
        return target / "assets"
    return target


def _open(target: Path) -> _Source:
    target = Path(target)
    if target.is_file():
        return _ImageSource(target)
    if target.is_dir():
        return _FolderSource(game_files_folder(target))
    raise SetupError(f"{target} isn't there.")


# ---------------------------------------------------------------------------
# The check
# ---------------------------------------------------------------------------
# Row.status:
#   ok          the same MD5 as on the disc (or the same size, for a quick check)
#   changed     the same size, another MD5: the file was changed or is damaged
#   size        another size: cut off, or changed
#   missing     on the disc, not in the folder
#   other_disc  not on this disc, but the same file as on another known disc
#               (the Play page's "Add voices from another disc" puts them there)
#   censored    [acct2-decomp] a level archive's copy made by the censored campaign
#               (c3w/censored.py; its size and MD5 are in assets/censored/levels.tsv)
#   extra       not on any known disc (the tool's own files, or added by hand)
STATUS_TEXT = {
    "ok": "Same as the disc",
    "changed": "Changed or damaged (another MD5)",
    "size": "Another size (cut off or changed)",
    "missing": "Missing",
    "other_disc": "From another disc",
    "censored": "The censored campaign's copy",
    "extra": "Not on the disc",
}
PROBLEMS = ("changed", "size", "missing")


@dataclass
class Row:
    path: str
    status: str
    size: Optional[int]           # yours (None when missing)
    md5: str = ""                 # yours ("" when not hashed)
    expected_size: Optional[int] = None
    expected_md5: str = ""        # the disc's
    note: str = ""                # other_disc: which disc


@dataclass
class Report:
    source: str
    disc: Optional[Disc]
    deep: bool
    rows: List[Row] = field(default_factory=list)

    def count(self, *status: str) -> int:
        return sum(1 for r in self.rows if r.status in status)

    @property
    def problems(self) -> List[Row]:
        return [r for r in self.rows if r.status in PROBLEMS]

    def summary(self) -> str:
        if self.disc is None:
            return (f"{self.source}: not a known Call of Duty 3 disc (default.xex is missing or "
                    "unknown), so there is no list to check against.")
        how = "MD5" if self.deep else "size"
        n_disc = len(known_files(self.disc))
        text = (f"{self.disc.name}: {self.count('ok')} of {n_disc} files the same as on the disc "
                f"(by {how})")
        bad = [f"{self.count(s)} {STATUS_TEXT[s].lower()}" for s in PROBLEMS if self.count(s)]
        text += "; " + ", ".join(bad) if bad else "; nothing missing or changed"
        more = [f"{n} {what}" for n, what in ((self.count("other_disc"), "from another disc"),
                                              (self.count("censored"), "made by the censored campaign"),
                                              (self.count("extra"), "not on any disc")) if n]
        if more:
            text += ". Also " + ", ".join(more)
        return text + "."


def check(target: Path, disc: Optional[Disc] = None, deep: bool = True,
          cancel: Optional[threading.Event] = None,
          progress: Optional[Callable[[int, int, str], None]] = None) -> Report:
    """Compares a game folder (or a disc image) with the list of its disc.
    `disc`: which list (default: told by default.xex). `deep`: compare MD5s
    (reads every file); otherwise sizes only. `progress(done_bytes,
    total_bytes, path)` is called as files are read."""
    src = _open(Path(target))
    try:
        disc = disc or disc_by_xex(src.xex_sha1() or "")
        report = Report(src.label, disc, deep)
        if disc is None:
            return report
        known = known_files(disc)
        others = [(d, known_files(d)) for d in DISCS if d is not disc]
        made = _censored_copies(src)
        have = {rel.lower(): (rel, size) for rel, size in src.files()}
        total = sum(size for rel, size in have.values()) if deep else 0
        done = 0
        for low, (rel, size) in sorted(have.items()):
            if cancel is not None and cancel.is_set():
                raise Cancelled()
            k = known.get(low)
            cands = [(d, o[low]) for d, o in others if low in o]
            md5 = ""
            if deep and (k is not None or cands):
                h = hashlib.md5()
                for b in src.chunks(rel):
                    if cancel is not None and cancel.is_set():
                        raise Cancelled()
                    h.update(b)
                    done += len(b)
                    if progress:
                        progress(done, total, rel)
                md5 = h.hexdigest()
            # [acct2-decomp] another disc's version of a file of the same name (the
            # censored campaign swaps 84 archives: c3w/censored.py) is from that disc
            other = next(((d, x) for d, x in cands if x.size == size and (not deep or x.md5 == md5)), None)
            mine = made.get(low)
            if k is not None:
                if size != k.size:
                    status = "size"
                elif deep and md5 != k.md5:
                    status = "changed"
                else:
                    status = "ok"
                if status != "ok" and other is not None:
                    report.rows.append(Row(rel, "other_disc", size, md5, other[1].size, other[1].md5,
                                           other[0].name))
                    continue
                # [acct2-decomp] a level archive's copy the censored campaign made (c3w/censored.py)
                if status != "ok" and mine is not None and size == mine[0] and (not deep or md5 == mine[1]):
                    report.rows.append(Row(rel, "censored", size, md5, k.size, k.md5,
                                           "made from the game's own file by the censored campaign"))
                    continue
                report.rows.append(Row(rel, status, size, md5, k.size, k.md5))
            elif other is not None:
                report.rows.append(Row(rel, "other_disc", size, md5, other[1].size, other[1].md5, other[0].name))
            else:
                report.rows.append(Row(rel, "extra", size, md5))
        for low, k in sorted(known.items()):
            if low not in have:
                report.rows.append(Row(k.path, "missing", None, "", k.size, k.md5))
        return report
    finally:
        src.close()


def _censored_copies(src: _Source) -> Dict[str, Tuple[int, str]]:
    """[acct2-decomp] The level copies c3w/censored.py made for a game folder (lower-case path:
    size, MD5), from assets/censored/levels.tsv; none for a disc image or without the list."""
    if not isinstance(src, _FolderSource):
        return {}
    from . import censored          # here: censored imports this module
    return {rel.lower(): (m.size, m.md5) for rel, m in censored.made_copies(src.root).items()}


def write_tsv(report: Report, path: Path) -> None:
    """The report as a table (status, your MD5, the disc's MD5, sizes, path)."""
    with open(path, "w", encoding="utf-8", newline="\n") as w:
        w.write(f"# {report.summary()}\n")
        w.write("status\tmd5\tdisc_md5\tsize\tdisc_size\tpath\n")
        for r in report.rows:
            w.write(f"{r.status}\t{r.md5}\t{r.expected_md5}\t{'' if r.size is None else r.size}\t"
                    f"{'' if r.expected_size is None else r.expected_size}\t{r.path}\n")

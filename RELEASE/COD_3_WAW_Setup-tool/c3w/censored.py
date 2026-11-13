"""[acct2-decomp] The censored campaign: the German release's textures, on or off.

The German disc of Call of Duty 3 is censored in its campaign archives: 84
of them (characters, vehicles, props of the levels' zones) carry other
textures for the insignia, item for item the same as the USA/Europe disc's
otherwise (the same items, sizes and layout; only their names in the
archive's directory say no_* for of_*). The other campaign archives the
German disc changes also carry its German text and voice lists (sp/global.cod,
sp/frontend.cod and each level's own sp/<level>/<level>.cod), so they aren't
swapped whole. Six of those level archives also hold insignia textures of their
own (oil and fuel barrels, a crate top, the King Tiger:
lists/censored_level_textures.tsv): for those, a copy of the game's own level
archive is made with the other release's pixels at the same items (the same
sizes; the language stays the game's own), and swapped like the others. The
game code and the multiplayer files are not touched.

This swaps those 84 files in a game's assets/ folder with the other
release's, and back:

  - add:  copies the version your game doesn't have from another disc into
          assets/censored/ (the German disc for a game built from the USA,
          Europe or Fr/Es/It disc; the USA/Europe or Fr/Es/It disc for a game
          built from the German one). Each file is checked by its MD5
          (c3w/disc_md5/*.tsv) while it is copied. The six level archives'
          copies are made from the game's own file and the disc's (both
          checked by MD5) and listed with their MD5s in
          assets/censored/levels.tsv.
  - on / off: swaps them (renames inside assets/, so it's instant); the
          version not in use waits in assets/censored/german/ or
          assets/censored/uncut/.

Each file is checked by its MD5 before it is moved, so a swap that stopped
half way, or a game taken off the disc again, is put right by the next one.
The Japanese release is a build of its own (its archives differ from both),
so it isn't supported. Close the game first (Windows can't rename a file the
game has open).

Safety: only the 84 + 6 paths from the lists (relative, under sp/) are touched; a
file or folder that is a link is never moved, written or followed (it is
reported and left alone), and assets/censored/ itself may not be a link.

    python3 -m c3w.censored <game folder> --status
    python3 -m c3w.censored <game folder> --add <German disc .iso or folder>
    python3 -m c3w.censored <game folder> --on | --off
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

if __package__ in (None, ""):   # run as a file: python3 c3w/censored.py
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "c3w"

from . import cod_archive, disc_files   # noqa: E402
from .common import Cancelled, Log, SetupError, console_sink, human, is_within, sha1_file   # noqa: E402

STORE = "censored"                      # assets/censored/{german,uncut}/<path as on the disc>
GERMAN, UNCUT = "german", "uncut"
OTHER = {GERMAN: UNCUT, UNCUT: GERMAN}
GERMAN_DISC = "germany"
UNCUT_DISCS = ("usa_europe", "europe_fr_es_it")   # the same 84 files, byte for byte
CHUNK = 8 * 1024 * 1024
EXPECTED = 84                           # the German disc's texture-only archives
LEVEL_LIST = "censored_level_textures.tsv"   # lists/: the level archives' own insignia textures
LEVELS_MANIFEST = "levels.tsv"          # assets/censored/: the level copies this made, with their MD5s


@dataclass(frozen=True)
class Pair:
    rel: str                # as on the disc
    uncut: disc_files.Known
    german: disc_files.Known

    def known(self, version: str) -> disc_files.Known:
        return self.german if version == GERMAN else self.uncut


def has_language(rel: str) -> bool:
    """A campaign archive the German disc also changes for its language (its
    text, its voice and film lists): sp/global.cod, sp/frontend.cod and a
    level's own sp/<level>/<level>.cod."""
    parts = rel.lower().split("/")
    if parts in (["sp", "global.cod"], ["sp", "frontend.cod"]):
        return True
    return len(parts) == 3 and parts[0] == "sp" and parts[2] == parts[1] + ".cod"


_pairs: Optional[List[Pair]] = None


def _safe_rel(rel: str) -> bool:
    """A campaign path from the lists as it may be used under assets/: relative,
    forward slashes, under sp/, no .. or drive letters (the lists ship with the
    tool, but nothing outside assets/ is ever touched, whatever they say)."""
    parts = rel.split("/")
    return (rel.lower().startswith("sp/") and "\\" not in rel and ":" not in rel and
            all(part not in ("", ".", "..") for part in parts))


def _store(assets: Path) -> Path:
    """assets/censored/, refused when it is a link (or leads) out of assets/."""
    store = assets / STORE
    if store.is_symlink() or not is_within(store, assets):
        raise SetupError(f"{store} is a link to another folder: the censored campaign keeps its copies only "
                         "inside the game's own folder. Remove the link, then try again.")
    return store


def pairs() -> List[Pair]:
    """The campaign archives whose German version differs from the others
    only in its textures, from the discs' lists."""
    global _pairs
    if _pairs is None:
        de = disc_files.known_files(disc_files.disc_by_key(GERMAN_DISC))
        us = disc_files.known_files(disc_files.disc_by_key(UNCUT_DISCS[0]))
        out = []
        for low, g in sorted(de.items()):
            u = us.get(low)
            if (u is not None and low.startswith("sp/") and low.endswith(".cod") and u.md5 != g.md5
                    and not has_language(low) and _safe_rel(u.path)):
                out.append(Pair(u.path, u, g))
        _pairs = out
    return _pairs


@dataclass(frozen=True)
class Made:
    """Size and MD5 of a level archive this made or started from (like disc_files.Known)."""
    size: int
    md5: str


@dataclass(frozen=True)
class LevelRow:
    own: str                # the version of the game it was made for
    orig: Made              # the game's own file (a disc's, by its MD5)
    made: Made              # the copy with the other release's textures


_levels: Optional[Dict[str, List[Tuple[str, str]]]] = None


def level_list_path() -> Path:
    return Path(__file__).resolve().parent.parent / "lists" / LEVEL_LIST


def levels() -> Dict[str, List[Tuple[str, str]]]:
    """The level archives (path as on the disc) with insignia textures of their own: (the name
    in the uncut disc's directory, the name in the German disc's) for each texture. Only those
    on both discs' lists."""
    global _levels
    if _levels is None:
        de = disc_files.known_files(disc_files.disc_by_key(GERMAN_DISC))
        us = disc_files.known_files(disc_files.disc_by_key(UNCUT_DISCS[0]))
        try:
            text = level_list_path().read_text(encoding="utf-8")
        except OSError:
            text = ""
        out: Dict[str, List[Tuple[str, str]]] = {}
        for line in text.splitlines():
            c = line.rstrip("\r").split("\t")
            if line.startswith("#") or len(c) < 3:
                continue
            low = c[0].strip().lower()
            if low in de and low in us and has_language(low) and _safe_rel(us[low].path):
                out.setdefault(us[low].path, []).append((c[1].strip(), c[2].strip()))
        _levels = out
    return _levels


def _read_levels(store: Path) -> Dict[str, LevelRow]:
    rows: Dict[str, LevelRow] = {}
    try:
        text = (store / LEVELS_MANIFEST).read_text(encoding="utf-8")
    except OSError:
        return rows
    for line in text.splitlines():
        c = line.split("\t")
        if line.startswith("#") or len(c) != 6 or c[1] not in (GERMAN, UNCUT):
            continue
        try:
            rows[c[0]] = LevelRow(c[1], Made(int(c[2]), c[3]), Made(int(c[4]), c[5]))
        except ValueError:
            continue
    return rows


def _write_levels(store: Path, rows: Dict[str, LevelRow]) -> None:
    p = store / LEVELS_MANIFEST
    if p.is_symlink():
        raise SetupError(f"{p} is a link: not written.")
    text = ("# [acct2-decomp] Level archives the censored campaign made (c3w/censored.py): path, the game's version,\n"
            "# the game's own file (size, MD5), the copy with the other release's textures (size, MD5).\n"
            + "".join(f"{rel}\t{r.own}\t{r.orig.size}\t{r.orig.md5}\t{r.made.size}\t{r.made.md5}\n"
                      for rel, r in sorted(rows.items())))
    tmp = p.with_name(p.name + ".part")
    if tmp.is_symlink() or tmp.exists():
        tmp.unlink()
    with open(tmp, "x", encoding="utf-8") as o:
        o.write(text)
    tmp.replace(p)


def made_copies(assets: Path) -> Dict[str, Made]:
    """The level copies this made for the game in assets/ (path: size and MD5), for
    disc_files.check: such a file in use is the censored campaign's, not a damaged one."""
    store = assets / STORE
    if store.is_symlink():
        return {}
    return {rel: r.made for rel, r in _read_levels(store).items()}


def entries(assets: Path, own: str) -> Tuple[List[Pair], List[str]]:
    """(what the switch covers: the 84 and the level copies made for this game, the level
    archives still without a copy)."""
    store = assets / STORE
    rows = {} if store.is_symlink() else _read_levels(store)
    out, missing = list(pairs()), []
    for rel in levels():
        r = rows.get(rel)
        if r is None or r.own != own:
            missing.append(rel)
            continue
        out.append(Pair(rel, r.orig if own == UNCUT else r.made, r.made if own == UNCUT else r.orig))
    return out, missing


def copy_textures(own: bytes, donor: bytes, names: List[Tuple[str, str]]) -> Tuple[bytes, List[str], List[str]]:
    """`own` (a level archive) with the GPU data of the named textures taken from `donor` (the
    same level from the other release). names: (the name in own's directory, in donor's). Only
    textures of the same shape and size are copied, so every item stays where it is; the
    changed chunks are stored as they are, the others kept byte for byte. Returns (the new
    archive, the textures copied, the problems)."""
    oi, di = cod_archive.read_index(own), cod_archive.read_index(donor)
    os_, ds = oi.streamed(), di.streamed()
    if os_ is None or ds is None:
        return own, [], ["no streamed section"]
    si = oi.sections.index(os_)
    o_ent = {e.name.lower(): e for e in os_.entries}
    d_ent = {e.name.lower(): e for e in ds.entries}
    mine: Dict[int, Tuple[bytearray, Dict[Tuple[int, int], cod_archive.Item]]] = {}
    theirs: Dict[int, Tuple[bytes, Dict[Tuple[int, int], cod_archive.Item]]] = {}

    def load(cache, idx, sec, data, b, writable):
        if b not in cache:
            if not 0 <= b < len(sec.chunks):
                raise cod_archive.CodError(f"no chunk {b}")
            ch = cod_archive.unpack(data, sec.chunks[b])
            items = {(it.record, it.index): it for it in cod_archive.apkf_items(ch)}
            cache[b] = (bytearray(ch) if writable else ch, items)
        return cache[b]

    copied, problems = [], []
    for own_name, donor_name in names:
        eo, ed = o_ent.get(own_name.lower()), d_ent.get(donor_name.lower())
        if eo is None or ed is None:
            problems.append(f"{own_name}: not in the archive's directory")
            continue
        try:
            buf, items = load(mine, oi, os_, own, eo.b, True)
            dch, ditems = load(theirs, di, ds, donor, ed.b, False)
        except cod_archive.CodError as e:
            problems.append(f"{own_name}: {e}")
            continue
        a, b = items.get((eo.c >> 16, eo.c & 0xFFFF)), ditems.get((ed.c >> 16, ed.c & 0xFFFF))
        if a is None or b is None or a.type != cod_archive.TEX or b.type != cod_archive.TEX or len(a.parts) < 2:
            problems.append(f"{own_name}: not a texture where the directory says")
            continue
        (ao, an), (ag, gn) = a.parts[0], a.parts[1]
        (bo, bn), (bg, hn) = b.parts[0], b.parts[1]
        if gn == 0 or gn != hn or cod_archive.texture_shape(bytes(buf[ao:ao + an])) != \
                cod_archive.texture_shape(dch[bo:bo + bn]) or cod_archive.texture_shape(dch[bo:bo + bn]) is None:
            problems.append(f"{own_name}: another size or format in the other release")
            continue
        buf[ag:ag + gn] = dch[bg:bg + hn]
        copied.append(own_name)
    if not copied:
        return own, copied, problems
    new = {(si, b): bytes(buf) for b, (buf, _) in mine.items()}
    out = cod_archive.replace_chunks(own, oi, new)
    check = cod_archive.read_index(out)               # read back: the new chunks as written, the rest as they were
    for (s, b), data in new.items():
        if cod_archive.unpack(out, check.sections[s].chunks[b]) != data:
            raise SetupError("the level archive didn't read back as written")
    return out, copied, problems


def assets_folder(target: Path) -> Path:
    """A game's assets/ folder: the output folder, or assets/ itself."""
    target = Path(target)
    if (target / "assets" / "default.xex").is_file():
        return target / "assets"
    if (target / "default.xex").is_file():
        return target
    raise SetupError(f"{target} has no Call of Duty 3 game files (assets/default.xex): build the game first, "
                     "or choose the output folder.")


def own_version(assets: Path) -> str:
    """The version the game was built with: GERMAN or UNCUT, by its default.xex."""
    disc = disc_files.disc_by_xex(sha1_file(assets / "default.xex"))
    if disc is not None and disc.key == GERMAN_DISC:
        return GERMAN
    if disc is not None and disc.key in UNCUT_DISCS:
        return UNCUT
    name = disc.name if disc else "an unknown release"
    raise SetupError(f"This game was built from {name}. The censored campaign needs a game built from the "
                     "USA/Europe, Europe (Fr, Es, It) or German disc: the Japanese release is a build of its own, "
                     "and its campaign files are not the same as either version's.")


def _md5(path: Path, cancel: Optional[threading.Event] = None,
         tick: Optional[Callable[[int], None]] = None) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(CHUNK), b""):
            if cancel is not None and cancel.is_set():
                raise Cancelled()
            h.update(b)
            if tick:
                tick(len(b))
    return h.hexdigest()


def version_of(path: Path, pair: Pair, cancel: Optional[threading.Event] = None,
               tick: Optional[Callable[[int], None]] = None) -> Optional[str]:
    """GERMAN, UNCUT, or None (missing, or neither: changed by hand)."""
    try:
        size = path.stat().st_size
    except OSError:
        return None
    fits = [v for v in (GERMAN, UNCUT) if pair.known(v).size == size]
    if not fits:
        return None
    md5 = _md5(path, cancel, tick)
    return next((v for v in fits if pair.known(v).md5 == md5), None)


# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------
@dataclass
class Status:
    own: str                                   # the version the game was built with
    in_use: Dict[str, int] = field(default_factory=dict)    # GERMAN / UNCUT / "changed" / "missing"
    stored: Dict[str, int] = field(default_factory=dict)    # GERMAN / UNCUT: copies waiting in the store
    total: int = 0
    deep: bool = False

    @property
    def censored(self) -> bool:
        """On: every file the swap covers is the German one."""
        return self.total > 0 and self.in_use.get(GERMAN, 0) == self.total

    def ready(self, version: str) -> bool:
        """Every file can be (or already is) `version`."""
        return self.in_use.get(version, 0) + self.stored.get(version, 0) >= self.total

    def summary(self) -> str:
        n = self.total
        g, u = self.in_use.get(GERMAN, 0), self.in_use.get(UNCUT, 0)
        how = "" if self.deep else " (by name and size; --verify checks the MD5s)"
        if g == n:
            text = f"Censored campaign: on ({n} archives with the German release's textures)"
        elif u == n:
            text = f"Censored campaign: off ({n} archives with the uncut textures)"
        else:
            text = f"Censored campaign: mixed ({g} German, {u} uncut of {n})"
        bad = self.in_use.get("changed", 0), self.in_use.get("missing", 0)
        if any(bad):
            text += f"; {bad[0]} changed by hand, {bad[1]} missing"
        other = OTHER[self.own]
        if not self.ready(other) and self.own == UNCUT:
            text += ". Add the German disc's files first (--add <German disc>)"
        elif not self.ready(other):
            text += ". Add the uncut files first (--add <USA/Europe or Fr/Es/It disc>)"
        return text + how + "."


def status(target: Path, deep: bool = False, cancel: Optional[threading.Event] = None) -> Status:
    """Which version each file is. Quick (deep False): by the files' sizes
    and what waits in the store (the version a file isn't, when only one of
    them is in the store); deep: by MD5."""
    assets = assets_folder(target)
    st = Status(own_version(assets), deep=deep)
    store = assets / STORE
    covered, waiting = entries(assets, st.own)
    for rel in waiting:            # a level archive without its copy yet: the game's own, as built
        st.total += 1
        key = st.own if (assets / rel).is_file() else "missing"
        st.in_use[key] = st.in_use.get(key, 0) + 1
    for p in covered:
        st.total += 1
        live = assets / p.rel
        for v in (GERMAN, UNCUT):
            if (store / v / p.rel).is_file():
                st.stored[v] = st.stored.get(v, 0) + 1
        if not live.is_file():
            key = "missing"
        elif deep:
            key = version_of(live, p, cancel) or "changed"
        else:
            size = live.stat().st_size
            fits = [v for v in (GERMAN, UNCUT) if p.known(v).size == size]
            spare = [v for v in (GERMAN, UNCUT) if (store / v / p.rel).is_file()]
            if not fits:
                key = "changed"
            elif len(fits) == 1:
                key = fits[0]
            elif len(spare) == 1:
                key = OTHER[spare[0]]
            else:
                key = st.own
        st.in_use[key] = st.in_use.get(key, 0) + 1
    return st


# ---------------------------------------------------------------------------
# Adding the other version from a disc
# ---------------------------------------------------------------------------
def add(source: Path, target: Path, log: Log, cancel: Optional[threading.Event] = None) -> dict:
    """Copies, from the disc `source` (an image or a folder), the version of
    each file that the game in `target` doesn't use now into assets/censored/.
    Every copy is checked against the disc's MD5 list."""
    cancel = cancel or threading.Event()
    assets = assets_folder(target)
    own = own_version(assets)               # refuses the Japanese release
    _store(assets)
    src = disc_files._open(Path(source))
    try:
        disc = disc_files.disc_by_xex(src.xex_sha1() or "")
        if disc is None:
            raise SetupError(f"{Path(source).name} isn't a Call of Duty 3 disc this tool knows (default.xex).")
        has = GERMAN if disc.key == GERMAN_DISC else UNCUT if disc.key in UNCUT_DISCS else None
        if has is None:
            raise SetupError(f"{disc.name} doesn't have either version of these files (it is a build of its own).")
        on_disc = {rel.lower(): (rel, size) for rel, size in src.files()}
        todo: List[Tuple[Pair, str]] = []
        already = same = 0
        log.info(f"Censored campaign: checking the game's {len(pairs())} archives that the German disc changes...")
        for p in pairs():
            live = assets / p.rel
            v = version_of(live, p, cancel)
            if v == has:
                same += 1           # the game already uses this disc's version of it
                continue
            stored = assets / STORE / has / p.rel
            if stored.is_file() and stored.stat().st_size == p.known(has).size:
                already += 1
                continue
            if p.rel.lower() not in on_disc:
                raise SetupError(f"{p.rel} isn't on {disc.name}: is the image complete?")
            todo.append((p, has))
        if has == own:
            raise SetupError(f"The game already uses the files of {disc.name}: "
                             + ("add the German disc's to switch to the censored campaign." if has == UNCUT else
                                "add the USA/Europe or Fr/Es/It disc's to switch to the uncut campaign."))
        total = sum(p.known(v).size for p, v in todo) or 1
        done = 0
        for p, v in todo:
            dest = assets / STORE / v / p.rel
            tmp = dest.with_name(dest.name + ".part")
            # Checked before any folder is made: a linked folder must not lead out of the store.
            if dest.is_symlink() or not is_within(dest.parent, assets / STORE):
                raise SetupError(f"Unsafe path (a link out of the game folder?): {p.rel}")
            dest.parent.mkdir(parents=True, exist_ok=True)
            if tmp.is_symlink() or tmp.exists():
                tmp.unlink()            # a copy that stopped half way (never followed if it's a link)
            h = hashlib.md5()
            with open(tmp, "xb") as o:  # created new: fails rather than write through a link
                for b in src.chunks(on_disc[p.rel.lower()][0]):
                    if cancel.is_set():
                        o.close()
                        tmp.unlink(missing_ok=True)
                        raise Cancelled()
                    o.write(b)
                    h.update(b)
                    done += len(b)
                    log.progress(done / total, f"censored campaign: {p.rel}")
            if h.hexdigest() != p.known(v).md5:
                tmp.unlink(missing_ok=True)
                raise SetupError(f"{p.rel} on {disc.name} isn't the file it should be (another MD5): "
                                 "the image may be damaged.")
            tmp.replace(dest)
        log.progress(1.0, "censored campaign: copied")
        size = sum(p.known(v).size for p, v in todo)
        log.info(f"Censored campaign: copied {len(todo)} files from {disc.name} ({human(size)}); "
                 f"{already} were already there.")
        made, ready, skipped = _add_levels(src, disc, has, own, assets, on_disc, log, cancel)
        return {"disc": disc.name, "version": has, "copied": len(todo), "bytes": size, "already": already,
                "levels": made, "levels_ready": ready, "levels_skipped": skipped}
    finally:
        src.close()


def _add_levels(src, disc, has: str, own: str, assets: Path, on_disc: Dict[str, Tuple[str, int]], log: Log,
                cancel: threading.Event) -> Tuple[int, int, List[str]]:
    """The level archives' copies: the game's own file with this disc's textures at the same
    items, saved as assets/censored/<has>/<path> and listed in levels.tsv. Returns (made,
    already there, skipped with why)."""
    store = _store(assets)
    rows = _read_levels(store)
    own_lists = [disc_files.known_files(disc_files.disc_by_key(k))
                 for k in (UNCUT_DISCS if own == UNCUT else (GERMAN_DISC,))]
    donor_list = disc_files.known_files(disc_files.disc_by_key(disc.key))
    made = ready = 0
    skipped: List[str] = []
    todo = list(levels().items())
    for n, (rel, names) in enumerate(todo):
        if cancel.is_set():
            raise Cancelled()
        log.progress(n / max(len(todo), 1), f"censored campaign: {rel}")
        live, dest = assets / rel, store / has / rel
        if live.is_symlink() or not live.is_file() or not is_within(live, assets):
            skipped.append(f"{rel}: not in the game (or a link)")
            continue
        md5 = _md5(live, cancel)
        r = rows.get(rel)
        if r is not None and r.own == own and md5 in (r.orig.md5, r.made.md5):
            other, want = (dest, r.made) if md5 == r.orig.md5 else (store / own / rel, r.orig)
            if other.is_file() and not other.is_symlink() and other.stat().st_size == want.size:
                ready += 1
                continue
            if md5 == r.made.md5:
                skipped.append(f"{rel}: the copy is in use and the game's own file is gone from {STORE}/{own}/")
                continue
        if not any(L.get(rel.lower()) is not None and L[rel.lower()].md5 == md5 for L in own_lists):
            skipped.append(f"{rel}: not the disc's file (changed by hand?), left as it is")
            continue
        dk = donor_list.get(rel.lower())
        if dk is None or rel.lower() not in on_disc:
            skipped.append(f"{rel}: not on {disc.name}")
            continue
        donor = b"".join(src.chunks(on_disc[rel.lower()][0]))
        if hashlib.md5(donor).hexdigest() != dk.md5:
            raise SetupError(f"{rel} on {disc.name} isn't the file it should be (another MD5): the image may be "
                             "damaged.")
        mine = live.read_bytes()
        pick = [(u, g) if own == UNCUT else (g, u) for u, g in names]
        try:
            out, copied, problems = copy_textures(mine, donor, pick)
        except cod_archive.CodError as e:
            skipped.append(f"{rel}: {e}")
            continue
        for why in problems:
            log.warn(f"Censored campaign: {rel}: {why}")
        if not copied:
            skipped.append(f"{rel}: none of its textures could be copied")
            continue
        if dest.is_symlink() or not is_within(dest.parent, store):
            raise SetupError(f"Unsafe path (a link out of the game folder?): {rel}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_name(dest.name + ".part")
        if tmp.is_symlink() or tmp.exists():
            tmp.unlink()
        with open(tmp, "xb") as o:
            o.write(out)
        tmp.replace(dest)
        rows[rel] = LevelRow(own, Made(len(mine), md5), Made(len(out), hashlib.md5(out).hexdigest()))
        _write_levels(store, rows)
        made += 1
        log.info(f"Censored campaign: {rel}: {len(copied)} texture(s) from {disc.name} put into a copy of the "
                 "game's own file (its language stays).")
    for why in skipped:
        log.warn(f"Censored campaign: {why}")
    return made, ready, skipped


# ---------------------------------------------------------------------------
# Switching
# ---------------------------------------------------------------------------
def set_censored(target: Path, on: bool, log: Log, cancel: Optional[threading.Event] = None) -> dict:
    """Puts the German (on) or the uncut (off) version of every file in use,
    from the store; the one taken out goes into the store."""
    cancel = cancel or threading.Event()
    assets = assets_folder(target)
    own = own_version(assets)
    want = GERMAN if on else UNCUT
    other = OTHER[want]
    store = _store(assets)
    moved = kept = 0
    lacking: List[str] = []
    changed: List[str] = []
    covered, waiting = entries(assets, own)
    for rel in waiting:            # a level archive without its copy yet: it is the game's own version
        if want == own:
            kept += 1
        else:
            lacking.append(rel)
    total = sum(p.known(want).size for p in covered) or 1
    done = [0]

    def tick(n: int) -> None:
        done[0] += n
        log.progress(min(done[0] / total, 1.0), "censored campaign: checking")

    try:
        for p in covered:
            live = assets / p.rel
            have = store / want / p.rel
            put = store / other / p.rel
            # Only plain files inside this game's folder are ever moved (a link is left alone).
            if (live.is_symlink() or not is_within(live, assets) or have.is_symlink() or put.is_symlink()
                    or not is_within(have, store) or not is_within(put, store)):
                changed.append(p.rel)
                continue
            v = version_of(live, p, cancel, tick) if live.exists() else "missing"
            if v == want:
                kept += 1
                continue
            if v is None:
                changed.append(p.rel)
                continue
            if not have.is_file() or have.stat().st_size != p.known(want).size:
                lacking.append(p.rel)
                continue
            if v == other:
                put.parent.mkdir(parents=True, exist_ok=True)
                os.replace(live, put)
            live.parent.mkdir(parents=True, exist_ok=True)
            os.replace(have, live)
            moved += 1
    except PermissionError as e:
        raise SetupError(f"A game file is in use ({e.filename}): close the game, then try again.")
    log.progress(1.0, "censored campaign: done")
    word = "censored (the German disc's textures)" if on else "uncut"
    if lacking:
        disc = "the German disc" if on else "the USA/Europe or Fr/Es/It disc"
        log.warn(f"Censored campaign: {len(lacking)} files aren't in assets/{STORE}/ yet: add them from {disc} "
                 "first.")
    if changed:
        log.warn(f"Censored campaign: {len(changed)} files are neither version (changed by hand?), left as they "
                 f"are: {', '.join(changed[:5])}{' ...' if len(changed) > 5 else ''}")
    log.info(f"Censored campaign: {moved} files switched, {kept} already {word}.")
    return {"on": on, "moved": moved, "kept": kept, "lacking": lacking, "changed": changed}


# ---------------------------------------------------------------------------
def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="python3 -m c3w.censored", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", help="the game's output folder (with assets/), or assets/ itself")
    ap.add_argument("--add", metavar="DISC", help="copy the other version's files from this disc (.iso or folder)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--on", action="store_true", help="use the German disc's textures in the campaign")
    g.add_argument("--off", action="store_true", help="use the uncut textures")
    ap.add_argument("--status", action="store_true", help="show which version is in use")
    ap.add_argument("--verify", action="store_true", help="with --status: check every file's MD5")
    ap.add_argument("--list", action="store_true", help="list the files this swaps")
    args = ap.parse_args(argv)
    log = Log(sinks=[console_sink])
    try:
        if args.list:
            for p in pairs():
                print(f"{p.uncut.md5}  {p.german.md5}  {p.rel}")
            for rel, names in levels().items():
                print(f"{'level copy':>32}  {len(names):>2} texture(s){'':>18}  {rel}")
            print(f"{len(pairs())} files, {len(levels())} level archives")
            return 0
        if args.add:
            add(Path(args.add), Path(args.game), log)
        if args.on or args.off:
            r = set_censored(Path(args.game), args.on, log)
            if r["lacking"] or r["changed"]:
                print(status(Path(args.game)).summary())
                return 1
        if args.status or args.verify or not (args.add or args.on or args.off):
            print(status(Path(args.game), deep=args.verify).summary())
        return 0
    except (SetupError, Cancelled) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

"""[acct2-decomp] The censored campaign: the German release's textures, on or off.

The German disc of Call of Duty 3 is censored in its campaign archives: 84
of them (characters, vehicles, props of the levels' zones) carry other
textures for the insignia, item for item the same as the USA/Europe disc's
otherwise (the same items, sizes and layout; only their names in the
archive's directory say no_* for of_*). The other campaign archives the
German disc changes also carry its German text and voice lists (sp/global.cod,
sp/frontend.cod and each level's own sp/<level>/<level>.cod), so they are
left as they are: six of those level archives keep a few of their own
insignia textures (oil barrels, crates), which this can't change yet.
The game code and the multiplayer files are not touched.

This swaps those 84 files in a game's assets/ folder with the other
release's, and back:

  - add:  copies the version your game doesn't have from another disc into
          assets/censored/ (the German disc for a game built from the USA,
          Europe or Fr/Es/It disc; the USA/Europe or Fr/Es/It disc for a game
          built from the German one). Each file is checked by its MD5
          (c3w/disc_md5/*.tsv) while it is copied.
  - on / off: swaps them (renames inside assets/, so it's instant); the
          version not in use waits in assets/censored/german/ or
          assets/censored/uncut/.

Each file is checked by its MD5 before it is moved, so a swap that stopped
half way, or a game taken off the disc again, is put right by the next one.
The Japanese release is a build of its own (its archives differ from both),
so it isn't supported. Close the game first (Windows can't rename a file the
game has open).

Safety: only the 84 paths from the lists (relative, under sp/) are touched; a
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

from . import disc_files   # noqa: E402
from .common import Cancelled, Log, SetupError, console_sink, human, is_within, sha1_file   # noqa: E402

STORE = "censored"                      # assets/censored/{german,uncut}/<path as on the disc>
GERMAN, UNCUT = "german", "uncut"
OTHER = {GERMAN: UNCUT, UNCUT: GERMAN}
GERMAN_DISC = "germany"
UNCUT_DISCS = ("usa_europe", "europe_fr_es_it")   # the same 84 files, byte for byte
CHUNK = 8 * 1024 * 1024
EXPECTED = 84                           # the German disc's texture-only archives


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
            text = f"Censored campaign: on ({n} archives are the German disc's)"
        elif u == n:
            text = f"Censored campaign: off ({n} archives are the uncut ones)"
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
    for p in pairs():
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
            waiting = [v for v in (GERMAN, UNCUT) if (store / v / p.rel).is_file()]
            if not fits:
                key = "changed"
            elif len(fits) == 1:
                key = fits[0]
            elif len(waiting) == 1:
                key = OTHER[waiting[0]]
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
    own_version(assets)                     # refuses the Japanese release
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
        if same == len(pairs()):
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
        return {"disc": disc.name, "version": has, "copied": len(todo), "bytes": size, "already": already}
    finally:
        src.close()


# ---------------------------------------------------------------------------
# Switching
# ---------------------------------------------------------------------------
def set_censored(target: Path, on: bool, log: Log, cancel: Optional[threading.Event] = None) -> dict:
    """Puts the German (on) or the uncut (off) version of every file in use,
    from the store; the one taken out goes into the store."""
    cancel = cancel or threading.Event()
    assets = assets_folder(target)
    own_version(assets)
    want = GERMAN if on else UNCUT
    other = OTHER[want]
    store = _store(assets)
    moved = kept = 0
    lacking: List[str] = []
    changed: List[str] = []
    total = sum(p.known(want).size for p in pairs()) or 1
    done = [0]

    def tick(n: int) -> None:
        done[0] += n
        log.progress(min(done[0] / total, 1.0), "censored campaign: checking")

    try:
        for p in pairs():
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
            print(f"{len(pairs())} files")
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

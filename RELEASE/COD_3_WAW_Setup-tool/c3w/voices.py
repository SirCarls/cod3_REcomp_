"""Voices from another Call of Duty 3 disc.

Call of Duty 3's speech comes in sound banks per language, and every
release carries a different set:

  USA, Europe / Japan   English, in whole banks:   sp/<level>/<level>.wbk
  Germany               German, in whole banks:    sp/<level>/<level>.wde
  Europe (Fr, Es, It)   split banks: the sounds without speech in
                        sp/<level>/sound/<level>.wul, the speech in
                        sp/<level>/sound/<level>.wfr / .wes / .wit
                        (the battle chatter banks, sp/bcen_*.w??, are whole)

and the movies' sound tracks are movies/<name>-en/-de/-fr/-es/-it.wma. The
game code is the same on all of them (docs/X360_FAMILY.md), so a bank from
one disc fits a game built from another. This copies another disc's voice
files into a game's assets/ folder next to its own (they never have the same
names, and nothing that is already there is overwritten); the game's "Voices"
setting (cod3_voice_language, game/voice_language.h) then picks the language.

    python3 -m c3w.voices <disc .iso or extracted folder> <game folder> [--language de]
    python3 -m c3w.voices <disc> --list
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional

if __package__ in (None, ""):   # run as a file: python3 c3w/voices.py
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "c3w"

from .common import Cancelled, Log, SetupError, console_sink, human, is_within   # noqa: E402
from .xiso import XisoImage, looks_like_xiso   # noqa: E402

LANGUAGES: Dict[str, str] = {"en": "English", "de": "German", "fr": "French", "es": "Spanish", "it": "Italian"}

# A bank's language by its extension, as the game reads it (the AUDIO lists
# of its archives: .wen .wde .wfr .wes .wit are languages 0-4; anything else,
# .wbk and .wul, belongs to every language).
BANK_LANGUAGE = {".wen": "en", ".wde": "de", ".wfr": "fr", ".wes": "es", ".wit": "it"}
WHOLE_ENGLISH = ".wbk"     # the English discs' banks (speech and sounds together)
NO_SPEECH = ".wul"         # the sounds of a split bank (Fr/Es/It disc)
MOVIE_SOUND = re.compile(r"^movies/[^/]+-(en|de|fr|es|it)\.wma$")

# default.xex of the releases (docs/X360_FAMILY.md): the campaign code of the
# first three is the same byte for byte.
KNOWN_DISCS = {
    "681724c40e250e584e6585dd903e05afa00a8634": ("Call of Duty 3 (USA, Europe)", ("en",)),
    "52447cb4189bc4168e71324255e9e7acf5495eb8": ("Call of Duty 3 (Germany)", ("de",)),
    "6f9247bb74c4006632a8556dcd1d41332139ff83": ("Call of Duty 3 (Europe) (Fr,Es,It)", ("fr", "es", "it")),
    "44cbd1e1d4e6d2e6b3ca1fdb525713cca144a888": ("Call of Duty 3 (Japan)", ("en",)),
}

CHUNK = 8 * 1024 * 1024


def _norm(rel: str) -> str:
    return rel.replace("\\", "/").lstrip("/").lower()


def is_voice_file(rel: str) -> bool:
    """A file another disc can add: a sound bank of the campaign or the
    multiplayer program, or a movie's sound track."""
    r = _norm(rel)
    if MOVIE_SOUND.match(r):
        # the legal screen goes with the game's text language, not the voices
        return not r.startswith("movies/legal-")
    ext = os.path.splitext(r)[1]
    return r.startswith(("sp/", "mp/")) and (ext in BANK_LANGUAGE or ext in (WHOLE_ENGLISH, NO_SPEECH))


def language_of(rel: str) -> Optional[str]:
    """The language a voice file speaks; None for the sounds without speech
    (.wul) and anything that isn't a voice file."""
    r = _norm(rel)
    m = MOVIE_SOUND.match(r)
    if m:
        return m.group(1)
    ext = os.path.splitext(r)[1]
    if ext in BANK_LANGUAGE:
        return BANK_LANGUAGE[ext]
    if ext == WHOLE_ENGLISH and r.startswith(("sp/", "mp/")):
        return "en"
    return None


def languages_in(assets: Path) -> Dict[str, int]:
    """How many campaign speech banks of each language a game's assets/
    folder has (what the Voices setting can choose from)."""
    counts = {code: 0 for code in LANGUAGES}
    sp = Path(assets) / "sp"
    if not sp.is_dir():
        return counts
    for root, _dirs, names in os.walk(sp):
        for n in names:
            code = language_of(Path(root, n).relative_to(assets).as_posix())
            if code:
                counts[code] += 1
    return counts


# ---------------------------------------------------------------------------
# A disc: an image or an extracted folder
# ---------------------------------------------------------------------------
@dataclass
class _File:
    rel: str     # as on the disc, forward slashes
    size: int


class Disc:
    """Read-only access to a Call of Duty 3 disc, as an image or a folder."""

    def __init__(self, path: Path):
        self.path = Path(path)
        self.image: Optional[XisoImage] = None
        if self.path.is_dir():
            root = self.path / "assets" if (self.path / "assets" / "default.xex").is_file() else self.path
            self.root: Optional[Path] = root
        elif self.path.is_file() and looks_like_xiso(self.path):
            self.image = XisoImage(self.path)
            self.root = None
        else:
            raise SetupError(f"{self.path} is neither an Xbox 360 disc image nor a folder.")

    def close(self) -> None:
        if self.image:
            self.image.close()

    def __enter__(self) -> "Disc":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def files(self) -> List[_File]:
        if self.image:
            return [_File(e.path, e.size) for e in self.image.files]
        out = []
        for root, _dirs, names in os.walk(self.root):
            for n in names:
                p = Path(root, n)
                out.append(_File(p.relative_to(self.root).as_posix(), p.stat().st_size))
        return out

    def read_small(self, rel: str) -> bytes:
        if self.image:
            return self.image.read(rel)
        return (self.root / rel).read_bytes()

    def identify(self) -> tuple:
        """(release name, its voice languages); refuses anything that isn't a
        Call of Duty 3 disc."""
        try:
            sha = hashlib.sha1(self.read_small("default.xex")).hexdigest()
        except (SetupError, OSError):
            raise SetupError(f"{self.path.name} has no default.xex: not a Call of Duty 3 disc.")
        if sha not in KNOWN_DISCS:
            raise SetupError(f"{self.path.name} isn't a Call of Duty 3 release this tool knows "
                             f"(default.xex SHA-1 {sha}).")
        return KNOWN_DISCS[sha]

    def copy(self, files: List[_File], assets: Path, log: Log, cancel: threading.Event) -> None:
        """Copies `files` into `assets` at their paths on the disc."""
        if self.image:
            names = {f.rel for f in files}
            self.image.extract(assets, log, cancel, only=lambda rel: rel in names)
            return
        total = sum(f.size for f in files) or 1
        done = 0
        for f in files:
            dest = assets / f.rel
            tmp = dest.with_name(dest.name + ".part")
            dest.parent.mkdir(parents=True, exist_ok=True)
            with open(self.root / f.rel, "rb") as src, open(tmp, "wb") as o:
                while True:
                    if cancel.is_set():
                        raise Cancelled()
                    buf = src.read(CHUNK)
                    if not buf:
                        break
                    o.write(buf)
                    done += len(buf)
                    log.progress(done / total, f"voices: {f.rel}")
            tmp.replace(dest)


def assets_folder(target: Path) -> Path:
    """A game's assets/ folder: the output folder, or assets/ itself."""
    target = Path(target)
    if (target / "assets" / "default.xex").is_file():
        return target / "assets"
    if (target / "default.xex").is_file():
        return target
    raise SetupError(f"{target} has no Call of Duty 3 game files (assets/default.xex): build the game first, "
                     "or choose the output folder.")


def add_voices(source: Path, target: Path, log: Log, cancel: Optional[threading.Event] = None,
               languages: Optional[Iterable[str]] = None, plain_target: bool = False) -> dict:
    """Copies the voice files of the disc `source` that the game in `target`
    doesn't have yet. `languages` limits it (codes from LANGUAGES); the
    sounds of a split bank (.wul) come with any of their languages.
    `plain_target`: `target` is any folder, used as it is (for tests)."""
    cancel = cancel or threading.Event()
    assets = Path(target) if plain_target else assets_folder(target)
    want = set(languages or LANGUAGES)
    unknown = want - set(LANGUAGES)
    if unknown:
        raise SetupError("Unknown language: " + ", ".join(sorted(unknown)) +
                         f" (choose from {', '.join(LANGUAGES)})")
    with Disc(source) as disc:
        name, disc_languages = disc.identify()
        log.info(f"Voices from {name} ({', '.join(LANGUAGES[c] for c in disc_languages)}): {disc.path}")
        picked: List[_File] = []
        already = 0
        for f in disc.files():
            if not is_voice_file(f.rel):
                continue
            code = language_of(f.rel)
            if code is not None and code not in want:
                continue
            if code is None and not want & set(disc_languages):
                continue   # sounds without speech only come with a language they belong to
            dest = assets / f.rel
            if not is_within(dest, assets):
                raise SetupError(f"Unsafe path on the disc: {f.rel}")
            if dest.exists():
                already += 1
                continue
            picked.append(f)
        if picked:
            disc.copy(picked, assets, log, cancel)
    have = languages_in(assets)
    log.progress(1.0, f"voices: done ({human(sum(f.size for f in picked))})")
    if picked:
        log.info(f"Copied {len(picked)} voice files ({human(sum(f.size for f in picked))}); "
                 f"{already} were already there.")
    else:
        log.info(f"Nothing new: the game already has this disc's voices ({already} files).")
    log.info("Voices the game can use now: " +
             ", ".join(f"{LANGUAGES[c]} ({n} banks)" for c, n in have.items() if n) +
             ". Pick one with Voices on the Play page or in F2 > Game > Sound.")
    return {"disc": name, "copied": len(picked), "bytes": sum(f.size for f in picked),
            "already": already, "languages": have}


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="python3 -m c3w.voices", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc", help="a Call of Duty 3 disc image (.iso) or its extracted folder")
    ap.add_argument("game", nargs="?", help="the game's output folder (with assets/), or assets/ itself")
    ap.add_argument("--language", action="append", choices=sorted(LANGUAGES),
                    help="copy only this language (repeatable; default: every language on the disc)")
    ap.add_argument("--list", action="store_true", help="only list the disc's voice files")
    ap.add_argument("--plain-folder", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    log = Log(sinks=[console_sink])
    try:
        if args.list or not args.game:
            with Disc(Path(args.disc)) as disc:
                name, langs = disc.identify()
                files = [f for f in disc.files() if is_voice_file(f.rel)]
                print(f"{name}: {len(files)} voice files, {human(sum(f.size for f in files))}")
                for f in files:
                    print(f"{f.size:>12}  {language_of(f.rel) or '--'}  {f.rel}")
            return 0
        add_voices(Path(args.disc), Path(args.game), log, languages=args.language,
                   plain_target=args.plain_folder)
        return 0
    except (SetupError, Cancelled) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

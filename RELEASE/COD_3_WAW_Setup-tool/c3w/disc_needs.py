"""[acct2-decomp] Which of the setup tool's options need which Call of Duty 3 disc, and where a
game stands with each (owner 2026-11-05: "make sure the setup tool is robust with its options and
exclaiming to the user what options are disc dependent").

The campaign program is the same on the USA/Europe, German and Europe (Fr, Es, It) discs
(docs/X360_FAMILY.md), so any of them builds the game. What the disc decides:

  Build (Set up)                 your disc: USA/Europe, German or Europe (Fr, Es, It); the Japanese
                                 disc is a build of its own and can't be built
  Multiplayer, map packs         only a game built from the USA/Europe disc (the other discs carry
                                 another multiplayer program, which the recomp's hooks don't fit)
  Voices (Play)                  English: the USA/Europe disc; German: the German disc; French,
                                 Spanish, Italian: the Europe (Fr, Es, It) disc. A language the game
                                 doesn't have plays the game's own voices.
  Censored campaign (Play)       the German disc, once (a game built from the German disc: the
                                 USA/Europe or Fr/Es/It disc, to switch it off)
  Console language (Play)        the soldiers' language and the text only on the Fr/Es/It disc; the
                                 films' sound on every disc
  everything else                no other disc: the built-in censor's designs, your own and the HD
                                 textures, the game settings

Everything here only reads: a disc's default.xex (its SHA-1 tells the release:
c3w/disc_files.DISCS), the game's assets/ and assets/censored/.

    python3 cod3_waw_setup.py discs [<output folder>]
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

from . import disc_files
from .common import SetupError, sha1_file
from .xiso import XisoImage, looks_like_xiso

USA, GERMANY, FR_ES_IT, JAPAN = "usa_europe", "germany", "europe_fr_es_it", "japan"


@dataclass(frozen=True)
class Release:
    key: str                    # disc_files.Disc.key
    short: str                  # as the window says it: "the German disc"
    voices: Tuple[str, ...]     # the voice languages on it (c3w/voices.py codes)
    builds: bool                # the recomp builds a game from it
    multiplayer: bool           # its multiplayer program is the one the recomp knows


RELEASES: Dict[str, Release] = {
    USA: Release(USA, "the USA/Europe disc", ("en",), True, True),
    GERMANY: Release(GERMANY, "the German disc", ("de",), True, False),
    FR_ES_IT: Release(FR_ES_IT, "the Europe (Fr, Es, It) disc", ("fr", "es", "it"), True, False),
    JAPAN: Release(JAPAN, "the Japanese disc", ("en",), False, False),
}
LANGUAGE_NAMES = {"en": "English", "de": "German", "fr": "French", "es": "Spanish", "it": "Italian"}
VOICE_DISC = {"en": USA, "de": GERMANY, "fr": FR_ES_IT, "es": FR_ES_IT, "it": FR_ES_IT}

# (option as the window names it, where, what it needs): for the README, the CLI and the tests.
TABLE: Tuple[Tuple[str, str, str], ...] = (
    ("Build", "Set up", "your Call of Duty 3 disc: USA/Europe, German or Europe (Fr, Es, It); not the Japanese one"),
    ("Multiplayer and map packs", "Set up", "a game built from the USA/Europe disc"),
    ("Voices: English", "Play", "the USA/Europe disc (a game built from it has them)"),
    ("Voices: German", "Play", "the German disc"),
    ("Voices: French, Spanish, Italian", "Play", "the Europe (Fr, Es, It) disc"),
    ("Censored campaign", "Play", "the German disc, once (switching a German game off: the USA/Europe or "
                                  "Fr/Es/It disc)"),
    ("Console language", "Play", "soldiers' language and text: only the Europe (Fr, Es, It) disc; films' "
                                 "sound: every disc"),
    ("Built-in censor, your own textures, HD textures, game settings", "Textures, Play", "no other disc"),
)


# What a disc gives when it is the one the game is built from (Set up), and what another disc in the
# input folder is good for (the Play page's options).
GIVES = {
    USA: "Builds the campaign and multiplayer; English voices.",
    GERMANY: "Builds the campaign only (multiplayer needs the USA/Europe disc); German text and voices, the "
             "censored textures.",
    FR_ES_IT: "Builds the campaign only (multiplayer needs the USA/Europe disc); French, Spanish or Italian text "
              "and voices, by Console language on the Play page.",
    JAPAN: "A build of its own, which can't be built: use the USA/Europe, German or Europe (Fr, Es, It) disc.",
}
GOOD_FOR = {
    USA: "English voices",
    GERMANY: "the censored campaign and German voices",
    FR_ES_IT: "French, Spanish and Italian voices",
    JAPAN: "nothing here",
}


def release(disc: Optional[disc_files.Disc]) -> Optional[Release]:
    return RELEASES.get(disc.key) if disc is not None else None


def unknown_name(sha: Optional[str], default: str) -> str:
    """A disc the build doesn't take: its name when this tool knows it (the Japanese disc), else `default`."""
    disc = disc_files.disc_by_xex(sha or "")
    return disc.name if disc is not None else default


def setup_note(sha: Optional[str], others: Dict[str, Path]) -> str:
    """For Set up's disc status: what the chosen disc (by its default.xex SHA-1) gives, and the other
    Call of Duty 3 discs in the input folder (`others`: SHA-1 -> image) and what they are for. Starts with
    a line break; empty when there is nothing to say."""
    disc = disc_files.disc_by_xex(sha or "")
    parts = [GIVES[disc.key]] if disc is not None else []
    more = []
    for other_sha, path in others.items():
        d = disc_files.disc_by_xex(other_sha)
        if d is not None and (disc is None or d.key != disc.key):
            more.append(f"{RELEASES[d.key].short} ({path.name}: {GOOD_FOR[d.key]}, Play page)")
    if more:
        parts.append("Also in the input folder: " + "; ".join(more) + ".")
    return ("\n" + " ".join(parts)) if parts else ""


# ---------------------------------------------------------------------------
# Telling a disc
# ---------------------------------------------------------------------------
_seen: Dict[Tuple[str, int, int], str] = {}   # (path, size, mtime) -> default.xex SHA-1


def _xex_sha1(path: Path) -> str:
    """default.xex's SHA-1 of a disc image, an extracted disc or a game folder."""
    if path.is_dir():
        x = path / "assets" / "default.xex" if (path / "assets" / "default.xex").is_file() else path / "default.xex"
        if not x.is_file():
            raise SetupError(f"{path.name} has no default.xex: it isn't a Call of Duty 3 disc or game folder.")
        return sha1_file(x)
    if not path.is_file():
        raise SetupError(f"{path} isn't there.")
    st = path.stat()
    key = (str(path.resolve()), st.st_size, st.st_mtime_ns)
    if key not in _seen:
        if not looks_like_xiso(path):
            raise SetupError(f"{path.name} isn't an Xbox 360 disc image (.iso).")
        try:
            with XisoImage(path) as img:
                data = img.read("default.xex")
        except (SetupError, OSError, ValueError) as e:
            raise SetupError(f"{path.name} has no readable default.xex: it isn't a Call of Duty 3 disc ({e}).")
        _seen[key] = hashlib.sha1(data).hexdigest()
    return _seen[key]


def identify(path: Path) -> disc_files.Disc:
    """Which Call of Duty 3 release a disc image or folder is (only default.xex is read).
    Anything else raises SetupError saying what it is not."""
    path = Path(path)
    sha = _xex_sha1(path)
    disc = disc_files.disc_by_xex(sha)
    if disc is None:
        raise SetupError(f"{path.name} isn't a Call of Duty 3 disc this tool knows (default.xex SHA-1 {sha}). "
                         "It knows the USA/Europe, German, Europe (Fr, Es, It) and Japanese discs.")
    return disc


def game_disc(out: Optional[Path]) -> Optional[disc_files.Disc]:
    """The disc a built game was made from (its assets/default.xex), or None (not built, or unknown)."""
    if out is None:
        return None
    out = Path(out)
    for x in (out / "assets" / "default.xex", out / "default.xex"):
        if x.is_file():
            try:
                return disc_files.disc_by_xex(sha1_file(x))
            except OSError:
                return None
    return None


def found_discs(input_dir: Optional[Path]) -> Dict[str, Path]:
    """The Call of Duty 3 discs in the input folder (its top level and one folder down), by release key;
    the first of each by name. Nothing that isn't one is reported."""
    found: Dict[str, Path] = {}
    if input_dir is None or not Path(input_dir).is_dir():
        return found
    from . import inputs                         # here: inputs imports map_packs and waw
    for p in inputs.candidates(Path(input_dir)):
        if not p.name.lower().endswith(".iso"):
            continue
        try:
            d = identify(p)
        except (SetupError, OSError):
            continue
        found.setdefault(d.key, p)
    return found


def check_choice(path: Path, wanted: Tuple[str, ...], what: str) -> disc_files.Disc:
    """The disc picked for `what` (an option), when it is one of `wanted` (release keys); otherwise
    SetupError naming the discs that do it."""
    disc = identify(path)
    if disc.key not in wanted:
        names = " or ".join(RELEASES[k].short for k in wanted)
        raise SetupError(f"{Path(path).name} is {RELEASES[disc.key].short} ({disc.name}). {what} needs {names}.")
    return disc


# ---------------------------------------------------------------------------
# Where a game stands
# ---------------------------------------------------------------------------
@dataclass
class Line:
    option: str                 # as the window names it
    text: str                   # what it needs, and where the game stands
    ready: Optional[bool]       # True: works now; False: needs a disc first; None: not with this game


def _join(names: List[str]) -> str:
    return names[0] if len(names) == 1 else ", ".join(names[:-1]) + " and " + names[-1]


def _in_input(key: str, found: Dict[str, Path]) -> str:
    return f" ({found[key].name} is in the input folder)" if key in found else ""


def voice_lines(out: Path, disc: disc_files.Disc, found: Dict[str, Path]) -> Line:
    from . import voices
    have = voices.languages_in(out / "assets" if (out / "assets").is_dir() else out)
    own = set(RELEASES[disc.key].voices)
    present = [LANGUAGE_NAMES[c] + ("" if c in own else " (added)") for c in LANGUAGE_NAMES if have.get(c)]
    missing: Dict[str, List[str]] = {}
    for c in LANGUAGE_NAMES:
        if not have.get(c):
            missing.setdefault(VOICE_DISC[c], []).append(LANGUAGE_NAMES[c])
    parts = [f"in the game: {_join(present) if present else 'none found'}"]
    for key, langs in missing.items():
        parts.append(f"{_join(langs)} need{'s' if len(langs) == 1 else ''} {RELEASES[key].short}"
                     f"{_in_input(key, found)}")
    text = "; ".join(parts) + "."
    if missing:
        text += " Add them with \"Add voices from another disc\"; until then Voices plays the game's own."
    return Line("Voices", text, not missing)


def censored_line(out: Path, disc: disc_files.Disc, found: Dict[str, Path]) -> Line:
    from . import censored
    try:
        st = censored.status(out)
    except (SetupError, OSError) as e:
        return Line("Censored campaign", f"not with this game: {e}", None)
    state = ("on" if st.censored else "off" if st.in_use.get(censored.UNCUT, 0) == st.total else
             "partly on (switch it again to finish)")
    if st.own == censored.UNCUT:
        if st.ready(censored.GERMAN):
            return Line("Censored campaign", f"{state}; the German disc's files are in: it switches at once.", True)
        where = f"; {found[GERMANY].name} is in the input folder" if GERMANY in found else ""
        return Line("Censored campaign", f"{state}; needs the German disc once (about 1.2 GB is copied{where}).",
                    False)
    # a game built from the German disc is censored as it is; off needs an uncut disc
    if st.ready(censored.UNCUT):
        return Line("Censored campaign", f"{state}; both versions are in: it switches at once.", True)
    near = next((k for k in (USA, FR_ES_IT) if k in found), None)
    return Line("Censored campaign", f"{state} (your game is the German release); switching it off needs the "
                "USA/Europe or Europe (Fr, Es, It) disc once" + (_in_input(near, found) if near else "") + ".", False)


def lines(out: Optional[Path], input_dir: Optional[Path] = None,
          found: Optional[Dict[str, Path]] = None) -> List[Line]:
    """Each disc-dependent option and where the game in `out` stands with it. Never raises: an option
    that can't be told says so."""
    if found is None:
        try:
            found = found_discs(input_dir)
        except OSError:
            found = {}
    out = Path(out) if out is not None else None
    disc = game_disc(out)
    others = [RELEASES[k].short for k in found]
    in_input = f" In the input folder: {_join(others)}." if others else ""
    if disc is None:
        return [Line("Your game", "not built yet: build it on Set up from your Call of Duty 3 disc (USA/Europe, "
                     "German or Europe (Fr, Es, It); not the Japanese one)." + in_input, False)]
    rel = RELEASES[disc.key]
    if not rel.builds:
        return [Line("Your game", f"its files are from {rel.short}, which this tool can't build (it is a build "
                     "of its own): build from the USA/Europe, German or Europe (Fr, Es, It) disc." + in_input, None)]
    out_lines = [Line("Your game", f"built from {rel.short} ({disc.name}).{in_input}", True)]
    steps: List[Tuple[str, Callable[[], Line]]] = [
        ("Voices", lambda: voice_lines(out, disc, found)),
        ("Censored campaign", lambda: censored_line(out, disc, found)),
    ]
    for option, make in steps:
        try:
            out_lines.append(make())
        except Exception as e:  # noqa: BLE001 - one option's trouble mustn't hide the others
            out_lines.append(Line(option, f"can't tell ({type(e).__name__}: {e}).", None))
    if rel.multiplayer:
        out_lines.append(Line("Multiplayer and map packs", "with this disc.", True))
    else:
        out_lines.append(Line("Multiplayer and map packs", f"not with {rel.short}: only a game built from the "
                              "USA/Europe disc has multiplayer (its map packs too).", None))
    if disc.key == FR_ES_IT:
        out_lines.append(Line("Console language", "picks the soldiers' language and the text (French, Spanish or "
                              "Italian) on this disc, and the films' sound.", True))
    else:
        out_lines.append(Line("Console language", "changes only the films' sound on this disc.", True))
    out_lines.append(Line("No other disc needed", "the built-in censor and your own textures (Textures), the HD "
                          "textures, the game settings.", True))
    return out_lines


def text(out: Optional[Path], input_dir: Optional[Path] = None,
         found: Optional[Dict[str, Path]] = None) -> str:
    """lines() as the window and the CLI show them: one line each."""
    return "\n".join(f"{ln.option}: {ln.text}" for ln in lines(out, input_dir, found))


def main(argv: Optional[List[str]] = None) -> int:
    import argparse
    from . import common
    ap = argparse.ArgumentParser(prog="python3 cod3_waw_setup.py discs",
                                 description="Which options need which Call of Duty 3 disc.")
    ap.add_argument("target", nargs="?", help="output folder (default: the tool's output folder)")
    ap.add_argument("--table", action="store_true", help="only the table of what needs which disc")
    args = ap.parse_args(argv)
    print("What needs which disc:")
    for option, where, needs in TABLE:
        print(f"  {option} ({where}): {needs}")
    if args.table:
        return 0
    out = Path(args.target) if args.target else common.default_output_dir()
    print(f"\n{out}:")
    for ln in lines(out, common.default_input_dir()):
        mark = {True: "ok  ", False: "need", None: "--  "}[ln.ready]
        print(f"  [{mark}] {ln.option}: {ln.text}")
    return 0


if __name__ == "__main__":       # python3 -m c3w.disc_needs
    import sys
    sys.exit(main())

"""The user texture pack: the player's own pictures for chosen textures.

The owner (2026-10-10): every swastika and other Nazi insignia texture should
be replaceable with a picture the user supplies, with clear format rules, no
confusion in multiplayer and no way to exploit it. lists/insignia_textures.txt
lists the insignia textures found (names, keys, sizes, what they show);
docs/TEXTURE_REPLACEMENT.md has the rules for the pictures.

How it works:
- The player saves PNGs named like the ripped pictures they replace
  (data/cod3/textures/<file>.png) in <output>/data/cod3/user_textures_png/.
- "Build texture pack" (Textures page), or the build's step after the HD
  texture pack, runs `rip cod3-user-pack` (first making or taking back the
  built-in censor's pictures for its saved choice, c3w/censor_builtin.py
  [acct2-decomp]): it checks every PNG (8-bit RGBA,
  RGB or gray, the original's aspect ratio, sides multiples of 4 for the
  compressed formats, at most 8192, never more see-through than the
  original) and writes <output>/data/cod3/user_textures/ (DDS files and
  index.tsv, the HD pack's layout), then checks the result with the game's
  own rules. Pictures that break a rule are skipped with the reason.
- The game reads that pack after the HD pack, so a texture in both comes
  from it (recomp/src/game/hd_textures.h); in a multiplayer match only the
  world insignia on the game's own list are replaced.

Everything is local work with the ripper already built: nothing is
downloaded, so no approval is asked.
"""
from __future__ import annotations

import re
import shutil
import time  # [rexglue-native-settings] 2026-11-11 the built stamp (mark_built)
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional

from .common import Log, Runner, SetupError

# Next to the ripper's "textures" folder, in <output>/data/cod3/.
PNG_DIR_NAME = "user_textures_png"
PACK_NAME = "user_textures"
PACK_INDEX = "index.tsv"
LIST_NAME = "insignia_textures.txt"
# [rexglue-native-settings] 2026-11-11 (owner 07:33 "insignia censoring doesn't work"): the built-in censor's choice
# and the player's own pictures reach the game only through this pack, and only "Build texture pack" or a full build
# made it, so ticking the censor box and pressing Play changed nothing in the game. Play now asks pack_out_of_date()
# and builds the pack first. build() writes this stamp in <output>/data/cod3/ each time it has run, so a build that
# leaves index.tsv as it was (the ripper rewrites it only when it changes) still counts as up to date.
BUILT_STAMP = ".user_textures_built"


def list_path(tool_root: Path) -> Path:
    """The insignia list shipped with the setup tool."""
    return tool_root / "lists" / LIST_NAME


@dataclass
class Insignia:
    file: str
    key: str
    width: int
    height: int
    format: str
    sizes: str
    alpha: str
    multiplayer: bool
    kind: str
    shows: str
    where: str


def parse_list(text: str) -> List[Insignia]:
    """The rows of insignia_textures.txt (tab-separated, # comments).
    Malformed rows are skipped."""
    rows = []
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.rstrip("\r").split("\t")
        if len(cols) < 11 or not re.fullmatch(r"[0-9a-f]{16}", cols[1]):
            continue
        try:
            width, height = int(cols[2]), int(cols[3])
        except ValueError:
            continue
        rows.append(Insignia(cols[0], cols[1], width, height, cols[4], cols[5], cols[6],
                             cols[7].strip().lower() == "yes", cols[8], cols[9], cols[10]))
    return rows


def read_list(tool_root: Path) -> List[Insignia]:
    try:
        return parse_list(list_path(tool_root).read_text(encoding="utf-8"))
    except OSError:
        return []


def png_dir(data_dir: Path) -> Path:
    """<output>/data/cod3/user_textures_png: where the player's PNGs go."""
    return data_dir / PNG_DIR_NAME


def pack_dir(data_dir: Path) -> Path:
    """<output>/data/cod3/user_textures: the pack the game reads."""
    return data_dir / PACK_NAME


def user_pictures(data_dir: Path) -> List[Path]:
    """The PNGs the player has put in the folder (any case of .png)."""
    folder = png_dir(data_dir)
    if not folder.is_dir():
        return []
    return sorted(p for p in folder.iterdir() if p.suffix.lower() == ".png" and p.is_file())


def replaced_names(data_dir: Path) -> set:
    """The picture names the player replaces: the HD pack's upscaler leaves
    those alone (the user pack wins in the game anyway)."""
    from . import censor_builtin  # [acct2-decomp] the built-in censor's pictures are painted from the HD copies
    return {p.stem for p in user_pictures(data_dir)} - censor_builtin.made_names(data_dir)


def prepare_folder(data_dir: Path, tool_root: Path) -> Path:
    """Makes the PNG folder and puts a copy of the insignia list in it, so the
    names to use are at hand. Returns the folder."""
    folder = png_dir(data_dir)
    folder.mkdir(parents=True, exist_ok=True)
    source = list_path(tool_root)
    target = folder / LIST_NAME
    try:
        if source.is_file() and (not target.is_file() or target.read_bytes() != source.read_bytes()):
            shutil.copyfile(source, target)
    except OSError:
        pass
    return folder


def pack_keys(data_dir: Path) -> int:
    """How many texture keys the built pack serves (0: no pack)."""
    try:
        text = (pack_dir(data_dir) / PACK_INDEX).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0
    return sum(1 for line in text.splitlines() if line and not line.startswith("#"))


def mark_built(data_dir: Path) -> None:
    """[rexglue-native-settings] 2026-11-11 Notes that the pack was just built (BUILT_STAMP, the time in it is only for
    people reading the folder), so pack_out_of_date() knows the choices made before now are in it."""
    try:
        (Path(data_dir) / BUILT_STAMP).write_text(time.strftime("%Y-%m-%d %H:%M:%S\n"), encoding="utf-8")
    except OSError:
        pass   # then Play builds the pack again next time: slower, never wrong


def pack_out_of_date(data_dir: Path) -> Optional[str]:
    """[rexglue-native-settings] 2026-11-11 Why the user texture pack should be built before the game starts, in a few
    words for the log, or None when it is up to date or there is nothing to build: the built-in censor's saved choice
    (censor_builtin.json) or the player's pictures (user_textures_png) changed after the last build, or nothing was
    built yet while one of them asks for a pack."""
    data_dir = Path(data_dir)
    from . import censor_builtin  # here: censor_builtin imports this module
    pictures = user_pictures(data_dir)
    try:
        stamp = (data_dir / BUILT_STAMP).stat().st_mtime
    except OSError:
        stamp = None
    if stamp is None:   # never built by this version of the tool
        if censor_builtin.load_settings(data_dir).on:
            return "the insignia censor is on and the texture pack hasn't been built for it yet"
        if pictures:
            return f"{len(pictures)} picture(s) of your own and no texture pack built for them yet"
        return None
    changed = []
    try:
        if censor_builtin.settings_path(data_dir).stat().st_mtime > stamp:
            changed.append("the insignia censor's choice")
    except OSError:
        pass   # no saved choice: nothing of the censor to apply
    try:
        # the folder's own time changes when a picture is added, renamed or removed
        if png_dir(data_dir).stat().st_mtime > stamp or any(p.stat().st_mtime > stamp for p in pictures):
            changed.append("your own pictures")
    except OSError:
        pass
    return (" and ".join(changed) + " changed since the texture pack was built") if changed else None


def _progress(log: Log) -> Callable[[str], None]:
    """`rip cod3-user-pack` output: "n/total" lines become progress, the rest
    (skips with their reasons, what was packed, the summary) goes to the log."""
    fraction = re.compile(r"^(\d+)/(\d+) textures$")

    def handle(line: str) -> None:
        text = line.strip()
        m = fraction.match(text)
        if m and int(m.group(2)) > 0:
            done, total = int(m.group(1)), int(m.group(2))
            log.progress(min(done / total, 1.0), f"packed {done}/{total} user textures")
        elif text:
            log.output(text)
    return handle


def build(rip: Path, data_dir: Path, tool_root: Path, jobs: int, runner: Runner, log: Log) -> int:
    """Builds data/cod3/user_textures from the PNGs in user_textures_png
    (`rip cod3-user-pack`). Returns the number of keys in the pack (0 = no
    pack). Raises SetupError when it can't run."""
    textures = data_dir / "textures"
    if not (textures / "hd_keys.tsv").is_file():
        raise SetupError(f"No ripped textures with hd_keys.tsv in {textures}: build (rip the game) first.")
    folder = prepare_folder(data_dir, tool_root)
    from . import censor_builtin  # [acct2-decomp] the built-in censor's pictures, made or taken back first
    try:
        censor_builtin.refresh(data_dir, tool_root, log, getattr(runner, "cancel", None))
    except (SetupError, OSError) as e:
        log.warn(f"The built-in censor's pictures weren't updated: {e}")
    pictures = user_pictures(data_dir)
    pack = pack_dir(data_dir)
    cmd = [rip, "cod3-user-pack", textures, folder, pack, "--jobs", str(max(1, jobs))]
    if list_path(tool_root).is_file():
        cmd += ["--list", list_path(tool_root)]
    if not pictures and not (pack / PACK_INDEX).exists():
        log.info(f"No pictures in {folder}, so there is no user texture pack. Save PNGs named like the "
                 f"ripped textures there ({LIST_NAME} lists the insignia), then build it.")
        mark_built(data_dir)   # [rexglue-native-settings] 2026-11-11 nothing to pack is up to date too
        return 0
    log.info(f"Building the user texture pack from {len(pictures)} picture(s) in {folder} into {pack}...")
    rc = runner.run(cmd, quiet=True, check=False, line_filter=_progress(log))
    if rc != 0:
        raise SetupError(f"The ripper couldn't build the user texture pack (exit code {rc}; its messages "
                         "are above). If it says the command is unknown, the ripper in tools/ is older: "
                         "build again.")
    keys = pack_keys(data_dir)
    mark_built(data_dir)   # [rexglue-native-settings] 2026-11-11 for pack_out_of_date (Play)
    if keys:
        log.info(f"User texture pack ready: {keys} texture(s) in {pack}. The game uses it from its next start "
                 "(it wins over the HD textures); in a multiplayer match only the world insignia are used.")
    else:
        log.info("No usable pictures, so there is no user texture pack (the reasons are above).")
    return keys


def installed_rip(output: Path, exe: str) -> Optional[Path]:
    """The ripper the build copied into <output>/tools."""
    rip = output / "tools" / exe
    return rip if rip.is_file() else None

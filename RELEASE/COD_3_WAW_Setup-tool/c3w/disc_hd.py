"""HD textures for the original-Xbox ports, Finest Hour and Big Red One, made from the disc's own textures.

[setup-layout] 2026-11-24 (owner 09:15: "Please start looking for logical problems with these HD textures. Then add
this option to the other games."; owner 13:53 "Please continue working").

The ripper reads every texture the port can replace and writes it as a PNG named the way the port looks it up:
`rip fh-hd-textures` reads Finest Hour's COD.PAK, `rip bro-hd-textures` reads Big Red One's DDS files
(docs/formats/fh.md, engine/tools/rip/src/bro_textures.rs). The PNGs and hd_keys.tsv go to <output>/<game>/hd/textures.
waifu2x upscales them into hd/textures_upscaled, and that folder is the pack itself, because the file names are the
keys. No pack step is needed, unlike Call of Duty 3's DDS pack. The game is started with its variable
(FH_HD_TEXTURES or BRO_HD_TEXTURES) set to that folder.

Everything stays under the game's folder in the output folder; the disc's files are only read. Making the pack is
optional, as Call of Duty 3's upscaling is: a failure is reported, the build still succeeds, and the game keeps an
earlier pack or uses its own textures.
"""
from __future__ import annotations

import shutil
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, List, Optional

from . import deps, rippers, upscale
from .common import Install, Log, Runner, SetupError, exe_name

HD_DIR = "hd"                   # <output>/<game>/hd
TEXTURES_DIR = "textures"       # the ripper's PNGs and hd_keys.tsv


@dataclass(frozen=True)
class Game:
    rip_command: str            # the ripper's command for this game
    variable: str               # what the port reads the pack folder from
    source: Callable[[Path], Optional[Path]]   # what the ripper reads, from the game's folder (<output>/<game>)


def _find_name(folder: Path, name: str) -> Optional[Path]:
    """`name` in `folder`, in any letter case (the disc's files keep the case the disc has)."""
    try:
        return next((p for p in folder.iterdir() if p.name.lower() == name.lower()), None)
    except OSError:
        return None


def _fh_source(game_out: Path) -> Optional[Path]:
    """Finest Hour's textures are all in the copied disc's COD.PAK."""
    pak = _find_name(game_out / "game", "COD.PAK")
    return pak if pak and pak.is_file() else None


def _bro_source(game_out: Path) -> Optional[Path]:
    """Big Red One's loose DDS textures: the copied disc's textures folder."""
    folder = _find_name(game_out / "game", "textures")
    return folder if folder and folder.is_dir() else None


GAMES: Dict[str, Game] = {
    "fh": Game("fh-hd-textures", "FH_HD_TEXTURES", _fh_source),
    "bro": Game("bro-hd-textures", "BRO_HD_TEXTURES", _bro_source),
}
VARIABLES = tuple(g.variable for g in GAMES.values())


def hd_dir(game_out: Path) -> Path:
    return Path(game_out) / HD_DIR


def textures_dir(game_out: Path) -> Path:
    return hd_dir(game_out) / TEXTURES_DIR


def pack_dir(game_out: Path) -> Path:
    """The upscaled copies, which are the pack: the port looks each texture up by its file name."""
    return hd_dir(game_out) / upscale.OUTPUT_NAME


def pack_size(game_out: Path) -> int:
    try:
        return sum(1 for _ in pack_dir(game_out).glob("*.png"))
    except OSError:
        return 0


def play_variables(game: str, game_out: Optional[Path], hd_textures: bool) -> Dict[str, str]:
    """The variable that turns the pack on, when it is wanted and there is one. An absolute folder, so the game's
    working folder doesn't matter."""
    if not hd_textures or game_out is None or not pack_size(game_out):
        return {}
    return {GAMES[game].variable: str(pack_dir(game_out).resolve())}


def status(game_out: Optional[Path]) -> str:
    """One line for the Play page."""
    count = pack_size(game_out) if game_out else 0
    if count:
        return f"HD pack: {count} textures."
    return "No HD pack yet: build with \"Make HD textures\" on to make it from the disc's textures."


def downloads(tools_dir: Path) -> List[Install]:
    """What making the pack downloads, for the build's one approval list: waifu2x, when it isn't there yet."""
    key = upscale.portable_key()
    return [] if deps._portable_ok(tools_dir, key) else [deps.tool_install(key, tools_dir)]


def _cargo() -> Optional[Path]:
    """Rust's cargo, if this PC has it (rustup's folder first, then PATH). The ripper is only built when no prebuilt
    one fits."""
    cargo = deps.cargo_bin_dir() / exe_name("cargo")
    if cargo.is_file():
        return cargo
    found = shutil.which("cargo")
    return Path(found) if found else None


def _ripper(output_root: Path, tool_root: Path, target_dir: Path, jobs: int, runner: Runner, log: Log) -> Path:
    """A ripper that writes the current layout: the one a Call of Duty 3 build left in <output>/tools, the
    prebuilt one, or one built now (when the PC has Rust)."""
    kept = output_root / "tools" / exe_name("rip")
    if kept.is_file() and rippers.works(kept, runner):
        return kept
    if rippers.needs_build(tool_root, runner) and _cargo() is None:
        raise SetupError("the ripper must be built and this PC has no Rust (Call of Duty 3's Build installs it)")
    return rippers.install_copy(rippers.get_rip(tool_root, target_dir, _cargo(), jobs, runner, log), output_root)


def build(game: str, game_out: Path, tool_root: Path, tools_dir: Path, scratch: Path, settings: "upscale.Settings",
          jobs: int, runner: Runner, log: Log, cancel: threading.Event) -> Optional[int]:
    """Rips the disc's textures, upscales them, and returns the pack's size (None when nothing was made).
    A failure is a warning, not an error; only Cancel stops the build."""
    g = GAMES[game]
    try:
        source = g.source(game_out)
        if source is None:
            raise SetupError("the disc's textures aren't in the game's folder (copy the disc's files again)")
        rip = _ripper(Path(game_out).parent, tool_root, scratch / "rust-target", jobs, runner, log)
        textures = textures_dir(game_out)
        log.info(f"Reading the disc's textures into {textures}...")
        rc = runner.run([rip, g.rip_command, source, hd_dir(game_out)], quiet=True, check=False)
        # Exit code 1 means some files couldn't be read (the ripper printed which); the others are listed.
        if rc not in (0, 1) or not (textures / upscale.KEYS_FILE).is_file():
            raise SetupError(f"the ripper couldn't read the disc's textures (exit code {rc})")
        if cancel.is_set():
            raise SetupError("canceled")
        tool = deps.fetch_portable(upscale.portable_key(), tools_dir, runner, log, cancel)
        done, missing = upscale.upscale_folder(tool, textures, pack_dir(game_out), scratch / "upscale", settings,
                                               runner, log, cancel)
        if missing:
            log.warn(f"{missing} textures couldn't be upscaled (waifu2x's messages are above); the game draws its "
                     "own for those.")
        log.info(f"Upscaled {done} textures into {pack_dir(game_out)}.")
    except (SetupError, OSError) as e:
        if cancel.is_set():
            raise
        earlier = " The pack from an earlier build stays in use." if pack_size(game_out) else ""
        log.warn(f"The HD textures weren't made: {e}.{earlier} The game is not affected.")
        return None
    count = pack_size(game_out)
    log.info(f"HD texture pack ready: {count} textures in {pack_dir(game_out)}. The game uses them while "
             "\"HD textures\" is on (its Play page).")
    return count


__all__ = ["GAMES", "VARIABLES", "hd_dir", "textures_dir", "pack_dir", "pack_size", "play_variables", "status",
           "downloads", "build"]

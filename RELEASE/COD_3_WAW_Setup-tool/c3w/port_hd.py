"""HD textures for World at War, Call of Duty 2 and Call of Duty 4 (the ports in c3w/x360_ports.py).

[setup-layout] 2026-11-24 (owner 09:15: "Please start looking for logical problems with these HD textures. Then add
this option to the other games."; decision card: "Both, capture first").

Call of Duty 3's HD pack is made from its ripped textures (c3w/upscale.py, `rip cod3-hd-pack`). No ripper reads these
games' Xbox 360 textures yet, so their textures are collected while the game is played instead, and the same steps
make the pack from them:

1. Play with "Save the textures the game uses" on: the SDK's texture capture (`--texture_capture_path`, SDK patch
   0321) saves each texture a pack can replace the first time the game draws with it, under the key the pack uses,
   into <output>/<game>/hd/capture (DDS files and captured.tsv). Later sessions add only new textures.
2. Build the game again with "Upscale textures with waifu2x" on: `rip captured-textures` turns the saved textures
   into hd/textures (PNGs and hd_keys.tsv, as a Call of Duty 3 rip has them), waifu2x upscales them into
   hd/textures_upscaled, and `rip cod3-hd-pack` packs them into hd/hd_textures.
3. Play with "HD textures" on: the game is started with `--texture_replacement_path` at that pack (SDK patch 0016).
   The multiplayer programs also get `--texture_replacement_check_alpha`, so no pack can make walls see-through
   (as Call of Duty 3's matches do).

Only textures the game drew while capture was on are in the pack: a level never played keeps its own textures.
Everything here is under the game's folder in the output folder; nothing in the game's own files changes.

World at War needs no capture (2026-11-24 14:10, owner 13:53 "Please continue working"): `rip waw-hd-textures` reads
every texture straight from the disc's fastfiles copied into <output>/waw/assets (with the disc's highmip/ levels),
giving the same textures/ folder step 2 makes from a capture, for every level whether played or not. Call of Duty 2
and Call of Duty 4 still use the capture: no ripper reads their fastfiles' textures yet.
"""
from __future__ import annotations

import threading
from pathlib import Path
from typing import List, Optional

from . import rippers, upscale
from .common import Log, Runner, SetupError

HD_DIR = "hd"                   # <output>/<game>/hd
CAPTURE_DIR = "capture"         # what the game saves (texture_capture_path)
CAPTURE_LIST = "captured.tsv"   # written by the game next to its DDS files (rip's capture::LIST_FILE)
TEXTURES_DIR = "textures"       # `rip captured-textures` output: PNGs and hd_keys.tsv
# Ports whose textures the ripper reads from the disc files themselves: port id -> its rip command.
FASTFILE_RIP = {"waw": "waw-hd-textures"}


def reads_disc(port_id: str) -> bool:
    """True for a port whose HD pack is made from its disc files, needing no capture first."""
    return port_id in FASTFILE_RIP


def has_source(port_id: str, root: Path) -> bool:
    """True when a build has something to make the HD pack from: the disc files a build copies into
    <output>/<game>/assets (a port that reads them), or saved textures (the others)."""
    return reads_disc(port_id) or captured_count(root) > 0


def hd_dir(root: Path) -> Path:
    """<output>/<game>/hd, from the game's folder."""
    return Path(root) / HD_DIR


def capture_dir(root: Path) -> Path:
    return hd_dir(root) / CAPTURE_DIR


def pack_dir(root: Path) -> Path:
    return hd_dir(root) / upscale.PACK_NAME


def captured_count(root: Path) -> int:
    """How many textures the game has saved (lines of captured.tsv; 0 when there is none). A line the game was
    cut off writing is counted too: the ripper skips it later."""
    try:
        with open(capture_dir(root) / CAPTURE_LIST, "rb") as f:
            return sum(1 for line in f if line.strip() and not line.startswith(b"#"))
    except OSError:
        return 0


def pack_ready(root: Path) -> bool:
    return upscale.pack_ready(pack_dir(root))


def arguments(root: Path, hd_textures: bool, capture: bool, multiplayer: bool) -> List[str]:
    """The SDK settings a start passes for these: absolute folders, so the game's working folder doesn't matter."""
    args: List[str] = []
    if capture:
        args.append(f"--texture_capture_path={capture_dir(root).resolve()}")
    if hd_textures and pack_ready(root):
        args.append(f"--texture_replacement_path={pack_dir(root).resolve()}")
        if multiplayer:
            # Every replacement image is checked against the game's own alpha in a match (SDK 0113): a pack file
            # edited to be see-through isn't used.
            args.append("--texture_replacement_check_alpha=true")
    return args


def status(root: Path, port_id: str = "") -> str:
    """One line for the Play page."""
    saved = captured_count(root)
    pack = pack_dir(root)
    packed = upscale.pack_size(pack) if upscale.pack_ready(pack) else 0
    if reads_disc(port_id):
        # Nothing to save first: the build reads the textures from the disc files.
        if packed:
            return f"HD pack: {packed} textures."
        return "No HD pack yet: build with upscaling on to make it from the disc's textures."
    if not saved and not packed:
        return "No textures saved yet."
    text = f"{saved} textures saved."
    if packed:
        text += f" HD pack: {packed} textures."
    elif saved:
        text += " Build again with upscaling on to make the HD pack from them."
    return text


def _textures_from_disc(rip: Path, port_id: str, root: Path, runner: Runner, log: Log) -> None:
    """hd/textures from the disc files in <output>/<game>/assets (a port in FASTFILE_RIP)."""
    assets = Path(root) / "assets"
    if not any(assets.glob("*.ff")):
        raise SetupError(f"no fastfiles in {assets}: the disc's files step didn't copy them")
    textures = hd_dir(root) / TEXTURES_DIR
    log.info(f"Reading the textures of the disc's fastfiles into {textures}...")
    cmd = [rip, FASTFILE_RIP[port_id], assets, hd_dir(root)]
    highmip = assets / "highmip"
    if highmip.is_dir():
        cmd += ["--highmip", highmip]
    rc = runner.run(cmd, quiet=True, check=False)
    # Exit code 1 means a fastfile couldn't be read (the ripper printed which); the others' textures are listed.
    if rc not in (0, 1) or not (textures / upscale.KEYS_FILE).is_file():
        raise SetupError(f"the ripper couldn't read the disc's textures (exit code {rc})")
    if rc == 1:
        log.warn("Some fastfiles couldn't be read (the ripper's messages are above); the others' textures are "
                 "used.")


def build(rip: Path, root: Path, waifu2x: Path, staging: Path, settings: "upscale.Settings", jobs: int,
          runner: Runner, log: Log, cancel: threading.Event, port_id: str = "") -> Optional[int]:
    """Steps 2's three parts. Returns the pack's texture count, or None when there was nothing to make it from.
    Raises SetupError when a step fails; the game and an earlier pack are left as they are (the ripper only
    replaces files whose content changed, and an earlier pack stays until the new one is written)."""
    textures = hd_dir(root) / TEXTURES_DIR
    if reads_disc(port_id):
        _textures_from_disc(rip, port_id, root, runner, log)
    else:
        saved = captured_count(root)
        if not saved:
            log.info(f"No saved textures in {capture_dir(root)}: play with \"Save the textures the game uses\" on "
                     "first. No HD pack is made.")
            return None
        log.info(f"Turning the {saved} saved textures into pictures in {textures}...")
        rc = runner.run([rip, "captured-textures", capture_dir(root), hd_dir(root)], quiet=True, check=False)
        # Exit code 1 means some files were skipped (the ripper printed which); the rest are usable.
        if rc not in (0, 1) or not (textures / upscale.KEYS_FILE).is_file():
            raise SetupError(f"the ripper couldn't read the saved textures (exit code {rc})")
        if rc == 1:
            log.warn("Some saved textures were skipped (the ripper's messages are above); the others are used.")
    if cancel.is_set():
        raise SetupError("canceled")
    upscaled = upscale.output_dir(hd_dir(root), log)
    done, missing = upscale.upscale_folder(waifu2x, textures, upscaled, staging, settings, runner, log, cancel)
    if missing:
        log.warn(f"{missing} textures couldn't be upscaled (waifu2x's messages are above); the game draws its "
                 "own for those.")
    log.info(f"Upscaled {done} textures into {upscaled}.")
    pack = pack_dir(root)
    rc = rippers.hd_pack(rip, textures, upscaled, pack, jobs, runner, log)
    if rc != 0 or not upscale.pack_ready(pack):
        raise SetupError(f"the HD texture pack wasn't made (the ripper stopped with exit code {rc})")
    return upscale.pack_size(pack)


__all__ = ["HD_DIR", "CAPTURE_DIR", "FASTFILE_RIP", "hd_dir", "capture_dir", "pack_dir", "captured_count",
           "pack_ready", "reads_disc", "has_source", "arguments", "status", "build"]

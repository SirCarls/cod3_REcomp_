"""[input-port] 2026-11-24 (owner: "make the controller module from our cod3 project work in the others", with the
SDL_GameControllerDB link): the controller mappings for the games that don't go through pipeline.py (Big Red One and
Finest Hour). As pipeline._controller_db does for the rexglue games, data/input/gamecontrollerdb.txt and its zlib
LICENSE (as gamecontrollerdb-LICENSE.txt) are copied next to the program, where bro_pad.c and fh_pad.c read them
when SDL's gamepads come up. Never stops the build: without them the game uses SDL's own mappings and the
player's gamecontrollerdb.txt in the folder above bin/."""
from __future__ import annotations

import shutil
from pathlib import Path
from typing import Callable, List, Optional

# COD_3_WAW_Setup-tool/data/input: the database and its license, shipped with the tool (docs/SOURCES.md)
DATA = Path(__file__).resolve().parent.parent / "data" / "input"
FILES = (("gamecontrollerdb.txt", "gamecontrollerdb.txt"), ("LICENSE", "gamecontrollerdb-LICENSE.txt"))


def copy_next_to(bindir: Path, warn: Optional[Callable[[str], None]] = None) -> List[str]:
    """Copies the database and its license into bindir; returns the names copied. A failure is only warned about."""
    copied: List[str] = []
    for name, target in FILES:
        try:
            bindir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(DATA / name, bindir / target)
            copied.append(target)
        except OSError as e:
            if warn is not None:
                warn(f"Controller mappings: couldn't copy {DATA / name} to {bindir} ({e}); the game uses SDL's own "
                     "mappings and your gamecontrollerdb.txt next to bin/")
    return copied

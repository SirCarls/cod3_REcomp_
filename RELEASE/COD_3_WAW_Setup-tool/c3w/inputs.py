"""The input folder: where the user can put their discs and downloads.

Anything dropped into `input/` (next to setup.sh) is found by itself:

- the Call of Duty 3 disc image (recognized by its default.xex);
- the World at War Xbox 360 disc image (fastfiles in its root folder);
- World at War map pack patches (map_packs.looks_like_patch);
- mods and custom maps: installers and archives (.exe, .zip, .rar, .7z).

Files are only read, and only their headers and folder listings: nothing is
unpacked or run here.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, List, Optional

from . import map_packs, waw
from .common import SetupError
from .xiso import XisoImage, looks_like_xiso

DISC_SUFFIXES = (".iso",)
MOD_SUFFIXES = (".exe", ".zip", ".rar", ".7z")


@dataclass
class Inputs:
    cod3_iso: Optional[Path] = None
    waw_xbox360: Optional[Path] = None
    map_packs: List[Path] = field(default_factory=list)
    mods: List[Path] = field(default_factory=list)


def _disc_kind(path: Path, cod3_xex_sha1: Iterable[str]) -> Optional[str]:
    """"cod3", "waw360" or None, from the disc's own files."""
    if not looks_like_xiso(path):
        return None
    try:
        with XisoImage(path) as img:
            names = {e.path.lower() for e in img.files}
            if "default.xex" in names and any(n.startswith("sp/") for n in names):
                if hashlib.sha1(img.read("default.xex")).hexdigest() in cod3_xex_sha1:
                    return "cod3"
            if any(n.endswith(".ff") and "/" not in n for n in names):
                return "waw360"
    except (OSError, SetupError):
        return None
    return None


def scan(folder: Path, cod3_xex_sha1: Iterable[str]) -> Inputs:
    """What the input folder holds (its top level and one folder down)."""
    found = Inputs()
    folder = Path(folder)
    if not folder.is_dir():
        return found
    candidates = [p for p in folder.iterdir() if p.is_file()]
    for sub in folder.iterdir():
        if sub.is_dir():
            candidates += [p for p in sub.iterdir() if p.is_file()]
    known = set(cod3_xex_sha1)
    for p in sorted(candidates):
        low = p.name.lower()
        if low.endswith(DISC_SUFFIXES):
            kind = _disc_kind(p, known)
            if kind == "cod3" and not found.cod3_iso:
                found.cod3_iso = p
            elif kind == "waw360" and not found.waw_xbox360:
                found.waw_xbox360 = p
        elif map_packs.looks_like_patch(p):
            found.map_packs.append(p)
        elif waw.MODS_ENABLED and low.endswith(MOD_SUFFIXES):
            found.mods.append(p)
    return found


__all__ = ["Inputs", "scan"]

"""The input folder: where the user can put their discs and downloads.

Anything dropped into `input/` (next to setup.sh) is found by itself:

- the Call of Duty 3 disc image (recognized by its default.xex); with several,
  the USA/Europe one is used for the build (it is the only one with the
  multiplayer program the recomp knows) and the others are listed in
  cod3_discs (the German disc for the censored campaign, other discs' voices:
  c3w/disc_needs.py) [acct2-decomp];
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
from typing import Dict, Iterable, List, Optional, Tuple

from . import map_packs, waw
from .common import SetupError
from .xiso import XisoImage, looks_like_xiso

DISC_SUFFIXES = (".iso",)
MOD_SUFFIXES = (".exe", ".zip", ".rar", ".7z")


@dataclass
class Inputs:
    cod3_iso: Optional[Path] = None
    cod3_discs: Dict[str, Path] = field(default_factory=dict)   # [acct2-decomp] default.xex SHA-1 -> image
    waw_xbox360: Optional[Path] = None
    map_packs: List[Path] = field(default_factory=list)
    mods: List[Path] = field(default_factory=list)


def _disc_info(path: Path, cod3_xex_sha1: Iterable[str]) -> Tuple[Optional[str], Optional[str]]:
    """("cod3", its default.xex SHA-1), ("waw360", None) or (None, None), from the disc's own files."""
    if not looks_like_xiso(path):
        return None, None
    try:
        with XisoImage(path) as img:
            names = {e.path.lower() for e in img.files}
            if "default.xex" in names and any(n.startswith("sp/") for n in names):
                sha = hashlib.sha1(img.read("default.xex")).hexdigest()
                if sha in cod3_xex_sha1:
                    return "cod3", sha
            if any(n.endswith(".ff") and "/" not in n for n in names):
                return "waw360", None
    except (OSError, SetupError, ValueError):
        return None, None
    return None, None


def _disc_kind(path: Path, cod3_xex_sha1: Iterable[str]) -> Optional[str]:
    """"cod3", "waw360" or None, from the disc's own files."""
    return _disc_info(path, cod3_xex_sha1)[0]


def candidates(folder: Path) -> List[Path]:
    """[acct2-decomp] The files scan() looks at: the input folder's top level and one folder down."""
    folder = Path(folder)
    if not folder.is_dir():
        return []
    found = [p for p in folder.iterdir() if p.is_file()]
    for sub in folder.iterdir():
        if sub.is_dir():
            try:
                found += [p for p in sub.iterdir() if p.is_file()]
            except OSError:
                continue   # a folder that can't be listed is skipped
    return sorted(found)


def scan(folder: Path, cod3_xex_sha1: Iterable[str]) -> Inputs:
    """What the input folder holds (its top level and one folder down). `cod3_xex_sha1`: the
    Call of Duty 3 releases to take, the one to build first (pipeline.KNOWN_XEX: USA/Europe)."""
    found = Inputs()
    order = list(cod3_xex_sha1)
    known = set(order)
    for p in candidates(Path(folder)):
        low = p.name.lower()
        if low.endswith(DISC_SUFFIXES):
            kind, sha = _disc_info(p, known)
            if kind == "cod3" and sha not in found.cod3_discs:
                found.cod3_discs[sha] = p
            elif kind == "waw360" and not found.waw_xbox360:
                found.waw_xbox360 = p
        elif map_packs.looks_like_patch(p):
            found.map_packs.append(p)
        elif waw.MODS_ENABLED and low.endswith(MOD_SUFFIXES):
            found.mods.append(p)
    # [acct2-decomp] with several discs, the first in `order` (the USA/Europe disc) is built
    found.cod3_iso = next((found.cod3_discs[sha] for sha in order if sha in found.cod3_discs), None)
    return found


__all__ = ["Inputs", "candidates", "scan"]

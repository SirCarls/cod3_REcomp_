"""Names for the PC images' functions and globals.

Two sources, ours first:

1. `names/<image>.tsv` and `names/<image>.<area>.tsv` (ours, written by hand): one line per
   name we identified and checked ourselves:
       address <tab> name <tab> how it was identified <tab> evidence
   `how` is a short word (`CoD2x`, `strings`, `shape`, `call`, `export` ...);
   `evidence` says what was seen (an error string, a caller, a match).
2. Optionally, CoD2x's table of CoD2MP_s.exe names, read at run time from a
   CoD2x checkout given by the `COD2X_SRC` environment variable
   (`src/mss32/symbols_cod2mp_s.h`). It is never copied into this repository;
   a name from it that a port uses is verified and goes into our TSV with
   `CoD2x` as its source.
"""
from __future__ import annotations

import os
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAMES_DIR = HERE / "names"
_COD2X_LINE = re.compile(r'\{\s*0x([0-9A-Fa-f]+)u?\s*,\s*0x([0-9A-Fa-f]+)u?\s*,\s*"([^"]+)"\s*\}')


def read_tsv(key: str) -> list[tuple[int, str, str, str]]:
    """`names/<key>.tsv`, then `names/<key>.<area>.tsv` (one per area, so agents
    working on different areas each add to their own file), by file name."""
    paths = [NAMES_DIR / f"{key}.tsv"] + sorted(NAMES_DIR.glob(f"{key}.*.tsv"))
    rows = []
    for path in paths:
        if not path.is_file():
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                raise SystemExit(f"{path}:{number}: expected address<tab>name<tab>how<tab>evidence")
            address = int(parts[0], 16)
            rows.append((address, parts[1].strip(), parts[2].strip() if len(parts) > 2 else "",
                         parts[3].strip() if len(parts) > 3 else ""))
    return rows


def cod2x_names() -> list[tuple[int, int, str]]:
    """(address, length, name) from a CoD2x checkout, if COD2X_SRC points at one."""
    src = os.environ.get("COD2X_SRC")
    if not src:
        return []
    path = Path(src) / "src" / "mss32" / "symbols_cod2mp_s.h"
    if not path.is_file():
        path = Path(src) / "mss32" / "symbols_cod2mp_s.h"
    if not path.is_file():
        return []
    out = []
    for m in _COD2X_LINE.finditer(path.read_text(encoding="utf-8", errors="replace")):
        out.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
    return out


def apply(image) -> None:
    """Adds our names, then CoD2x's (for CoD2MP_s.exe), to an image."""
    ours = set()
    for address, name, _how, _evidence in read_tsv(image.key):
        # Ours win over anything loaded before (exports keep their own entry under their name).
        image.names[address] = name
        image.addresses[name] = address
        ours.add(address)
    if image.key == "cod2mp":
        for address, length, name in cod2x_names():
            if not image.is_code(address):
                continue
            if address in ours:
                image.lengths.setdefault(address, length)  # our name, CoD2x's length
            elif name not in image.addresses:
                image.add_name(address, name, length)

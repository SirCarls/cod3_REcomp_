"""The game's executables as the decomp tools see them: decrypted images, with
their sections, function bounds, imports and the names from the symbol map.

Where the files come from (never from the repository: they are the player's
own game files):

- the folder in the `COD3_DECOMP_INPUT` environment variable, or
- `dev/decomp-input/` in the project folder (private; `dev/` is not published),

holding `default.xex`, `codmp_xenonf.xex` and the level DLLs (`saint_lo.dll`
...), as copied from the disc. Decrypted images are cached in `dev/decomp/`
(or `COD3_DECOMP_CACHE`), next to a hash of the file they came from.
"""
from __future__ import annotations

import bisect
import hashlib
import json
import os
import struct
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

import xex

ROOT = Path(__file__).resolve().parents[2]
SYMBOLS = ROOT / "COD_3_WAW_Setup-tool" / "recomp" / "config" / "symbols.toml"
# The supported release: Call of Duty 3 (USA, Europe), as the setup tool checks it.
DEFAULT_XEX_SHA1 = "681724c40e250e584e6585dd903e05afa00a8634"
LEVELS = ["blkbrn", "chambois", "credits", "crssrds", "falaise", "forest", "fuelplnt", "hostage",
          "island", "laison", "mace2", "mayenne", "nightd", "saint_lo", "stbert"]
FILE_NAMES = {"default": "default.xex", "codmp": "codmp_xenonf.xex"} | {lv: f"{lv}.dll" for lv in LEVELS}
# The two programs. Both load at 0x82000000, so each runs on its own in the
# oracle; a level DLL runs next to the campaign program.
PROGRAMS = ("default", "codmp")
# The multiplayer program's symbol map isn't in the repository (it holds
# names only, but is made from your copy): tools/name_mp_functions.py writes
# it into the cache folder (docs/DECOMP.md, "The multiplayer program").
MP_SYMBOLS_NAME = "symbols_mp.toml"

PE_CODE = 0x20000000  # IMAGE_SCN_MEM_EXECUTE
PE_WRITE = 0x80000000  # IMAGE_SCN_MEM_WRITE


def input_dirs() -> list[Path]:
    dirs = []
    if os.environ.get("COD3_DECOMP_INPUT"):
        dirs.append(Path(os.environ["COD3_DECOMP_INPUT"]))
    dirs.append(ROOT / "dev" / "decomp-input")
    return dirs


def cache_dir() -> Path:
    d = Path(os.environ.get("COD3_DECOMP_CACHE") or ROOT / "dev" / "decomp")
    d.mkdir(parents=True, exist_ok=True)
    return d


def find_input(name: str) -> Path:
    file_name = FILE_NAMES.get(name)
    if file_name is None:
        raise SystemExit(f"Unknown image '{name}'. Known: {', '.join(FILE_NAMES)}")
    for d in input_dirs():
        if (d / file_name).is_file():
            return d / file_name
    where = " or ".join(str(d) for d in input_dirs())
    raise SystemExit(
        f"{file_name} not found in {where}.\n"
        "Copy it from your own Call of Duty 3 disc (the setup tool's output has it under assets/;\n"
        "level DLLs are assets/sp/<level>/<level>.dll). See docs/DECOMP.md.")


@dataclass
class Section:
    name: str
    start: int
    size: int
    flags: int

    @property
    def end(self) -> int:
        return self.start + self.size


@dataclass
class GuestImage:
    name: str
    base: int
    data: bytes
    sections: list[Section] = field(default_factory=list)
    functions: list[tuple[int, int]] = field(default_factory=list)  # (start, end) from .pdata
    imports: dict[int, str] = field(default_factory=dict)  # thunk address -> "library!ordinal"
    names: dict[int, str] = field(default_factory=dict)  # address -> name (functions and globals)
    addresses: dict[str, int] = field(default_factory=dict)  # name -> address

    # --- reading ----------------------------------------------------------
    def contains(self, address: int) -> bool:
        return self.base <= address < self.base + len(self.data)

    def read(self, address: int, size: int) -> bytes:
        o = address - self.base
        return self.data[o:o + size]

    def u32(self, address: int) -> int:
        return struct.unpack(">I", self.read(address, 4))[0]

    def f32(self, address: int) -> float:
        return struct.unpack(">f", self.read(address, 4))[0]

    def cstr(self, address: int, limit: int = 256) -> bytes | None:
        if not self.contains(address):
            return None
        raw = self.read(address, limit)
        end = raw.find(b"\0")
        return raw[:end] if end >= 0 else None

    # --- code -------------------------------------------------------------
    def code_ranges(self) -> list[tuple[int, int]]:
        return [(s.start, s.end) for s in self.sections if s.flags & PE_CODE]

    def is_code(self, address: int) -> bool:
        return any(lo <= address < hi for lo, hi in self.code_ranges())

    def function_at(self, address: int) -> tuple[int, int] | None:
        """The function holding an address: from .pdata, or (for leaf functions without
        unwind data) a named function start up to the next known start."""
        if not hasattr(self, "_starts"):
            starts = {f[0] for f in self.functions}
            starts |= {a for a, n in self.names.items() if self.is_code(a)}
            self._starts = sorted(starts)
            self._pdata = dict(self.functions)
        i = bisect.bisect_right(self._starts, address) - 1
        if i < 0:
            return None
        start = self._starts[i]
        end = self._pdata.get(start)
        if end is None:
            end = self._starts[i + 1] if i + 1 < len(self._starts) else start + 4
        return (start, end) if start <= address < end else None

    def describe(self, address: int) -> str:
        """`Name+0x10` for an address inside a known function, else `sub_X+0x10`."""
        f = self.function_at(address)
        if f is None:
            return self.names.get(address, f"0x{address:08X}")
        name = self.names.get(f[0], f"sub_{f[0]:08X}")
        return name if address == f[0] else f"{name}+0x{address - f[0]:X}"

    def resolve(self, what: str) -> int:
        """A name from the symbol map, `sub_XXXXXXXX`, or a hex address."""
        if what in self.addresses:
            return self.addresses[what]
        text = what[4:] if what.startswith("sub_") else what
        try:
            return int(text, 16)
        except ValueError:
            raise SystemExit(f"'{what}' is neither a known name nor an address") from None


def _parse_pe(image: GuestImage) -> None:
    d = image.data
    pe = struct.unpack("<I", d[0x3C:0x40])[0]
    count = struct.unpack("<H", d[pe + 6:pe + 8])[0]
    optional_size = struct.unpack("<H", d[pe + 20:pe + 22])[0]
    s = pe + 24 + optional_size
    for _ in range(count):
        name = d[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr = struct.unpack("<II", d[s + 8:s + 16])
        flags = struct.unpack("<I", d[s + 36:s + 40])[0]
        image.sections.append(Section(name, image.base + vaddr, vsize, flags))
        s += 40
    for sec in image.sections:
        if sec.name != ".pdata":
            continue
        raw = image.read(sec.start, sec.size)
        for i in range(0, len(raw) - 7, 8):
            begin, info = struct.unpack(">II", raw[i:i + 8])
            if begin:
                image.functions.append((begin, begin + ((info >> 8) & 0x3FFFFF) * 4))
    image.functions.sort()


def symbols_path(name: str) -> Path | None:
    """The symbol map for one of the programs, if there is one."""
    if name == "default":
        return SYMBOLS
    if name == "codmp":
        return cache_dir() / MP_SYMBOLS_NAME
    return None


def _load_symbols(image: GuestImage) -> None:
    path = symbols_path(image.name)
    if path is None or not path.is_file():
        return
    with open(path, "rb") as f:
        table = tomllib.load(f)
    for entry in table.get("function", []) + table.get("global", []):
        image.names.setdefault(entry["address"], entry["name"])
        image.addresses.setdefault(entry["name"], entry["address"])


def load(name: str = "default") -> GuestImage:
    """Decrypts (once; then from the cache) and parses one of the game's images."""
    source = find_input(name)
    raw = source.read_bytes()
    digest = hashlib.sha1(raw).hexdigest()
    if name == "default" and digest != DEFAULT_XEX_SHA1:
        print(f"warning: {source} is not the supported release (SHA-1 {digest}); "
              "addresses in the symbol map won't match.", file=sys.stderr)
    cache = cache_dir()
    pe_path, meta_path = cache / f"{name}.pe", cache / f"{name}.json"
    meta = None
    if pe_path.is_file() and meta_path.is_file():
        meta = json.loads(meta_path.read_text())
        if meta.get("sha1") != digest:
            meta = None
    if meta is None:
        data, base = xex.decrypt(raw)
        thunks = xex.import_thunks(raw, data, base)
        pe_path.write_bytes(data)
        meta = {"sha1": digest, "base": base, "source": source.name,
                "imports": {f"{a:08X}": [lib, ordinal, is_func] for a, (lib, ordinal, is_func) in thunks.items()}}
        meta_path.write_text(json.dumps(meta, indent=1))
    image = GuestImage(name, meta["base"], pe_path.read_bytes())
    for address, (lib, ordinal, is_func) in meta["imports"].items():
        if is_func:
            image.imports[int(address, 16)] = f"{lib}!{ordinal}"
    _parse_pe(image)
    _load_symbols(image)
    for address in image.imports:
        # The symbol map names some thunks (`__imp__XamUserGetName`); use those for messages.
        if address in image.names:
            image.imports[address] = image.names[address]
    return image

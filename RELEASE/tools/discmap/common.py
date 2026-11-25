"""[waw-decomp] 2026-11-16 discmap: the helpers every command shares.

- where the project is and how to reach its existing tools (tools/x360_family, tools/waw_decomp,
  tools/decomp): they are imported or run as they are, never copied;
- the work folder (images, match results, maps, overrides);
- reading and writing the tab-separated tables;
- reading a program off a disc and decrypting it (read only; nothing is ever run or patched).
"""
from __future__ import annotations

import csv
import hashlib
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
IMAGE_BASE = 0x82000000        # where every Call of Duty 3 program image loads
LEVEL_BASE = 0x89000000        # where a level DLL loads

csv.field_size_limit(1 << 30)


# ---------------------------------------------------------------- the project and its tools
def find_project(given: str | None = None) -> Path:
    """The project folder: --project, else $COD3_PROJECT, else the folder above tools/ when this
    tool sits in the project as tools/discmap."""
    for candidate in (given, os.environ.get("COD3_PROJECT"), str(HERE.parent.parent)):
        if candidate and (Path(candidate) / "COD_3_WAW_Setup-tool" / "recomp").is_dir():
            return Path(candidate).resolve()
    sys.exit("error: the project folder was not found (pass --project or set COD3_PROJECT)")


class Project:
    """Paths into the project. Everything here is only read."""

    def __init__(self, root: Path):
        self.root = root
        self.recomp = root / "COD_3_WAW_Setup-tool" / "recomp"
        self.tools = root / "tools"
        self.x360_family = self.tools / "x360_family"
        self.waw_decomp = self.tools / "waw_decomp"
        self.decomp = self.tools / "decomp"
        # The existing tools import each other by their bare module names.
        for folder in (self.decomp, self.waw_decomp, self.x360_family):
            if str(folder) not in sys.path:
                sys.path.insert(0, str(folder))

    def level_names(self) -> list[str]:
        """The levels the campaign has a codegen config for (recomp/config/<level>.toml)."""
        skip = {"default", "names", "symbols"}
        return sorted(p.stem for p in (self.recomp / "config").glob("*.toml") if p.stem not in skip)

    def tool(self, *parts: str) -> Path:
        return self.tools.joinpath(*parts)


def run_tool(script: Path, args: list[str], log: Path, timeout: float | None = None) -> int:
    """Run one of the project's tools as it is (python3 -B script args...), its output into log."""
    cmd = [sys.executable, "-B", str(script)] + [str(a) for a in args]
    with open(log, "a", encoding="utf-8") as out:
        out.write("$ " + " ".join(cmd) + "\n")
        out.flush()
        try:
            return subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            out.write(f"stopped after {timeout} s\n")
            return -1


# ---------------------------------------------------------------- tables
def read_tsv(path: Path) -> list[dict]:
    """A tab-separated table with a header line; lines starting with '#' before it are skipped."""
    with open(path, encoding="utf-8", newline="") as f:
        lines = [line for line in f if not line.startswith("#")]
    return list(csv.DictReader(lines, delimiter="\t", quoting=csv.QUOTE_NONE))


def read_meta(path: Path) -> dict[str, str]:
    """The '# key: value' lines at the top of a table discmap wrote."""
    meta = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("#"):
                break
            if ": " in line:
                key, value = line[1:].strip().split(": ", 1)
                meta[key] = value
    return meta


def write_tsv(path: Path, header: list[str], rows, meta: dict | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as w:
        for key, value in (meta or {}).items():
            w.write(f"# {key}: {value}\n")
        w.write("\t".join(header) + "\n")
        for row in rows:
            w.write("\t".join(str(x).replace("\t", " ").replace("\n", " ") for x in row) + "\n")


def hex8(value: int | None) -> str:
    return "" if value is None else f"{value:08X}"


def parse_hex(text: str) -> int | None:
    text = (text or "").strip()
    if not text:
        return None
    return int(text, 16)


def file_md5(path: Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest()


# ---------------------------------------------------------------- programs on a disc
MAIN_FILES = {"default.xex": "campaign", "codmp_xenonf.xex": "mp"}


def program_files(entries) -> list[tuple[str, object]]:
    """(part, disc entry) for each program file on a disc: default.xex (campaign), codmp_xenonf.xex
    (multiplayer) and every level DLL (sp/<level>/<level>.dll). part is the lower-case path on the
    disc, which is how builds.tsv names it."""
    out = []
    for e in entries:
        path = e.path.replace("\\", "/").lower()
        if path in MAIN_FILES:
            out.append((path, e))
        else:
            bits = path.split("/")
            if len(bits) == 3 and bits[0] == "sp" and bits[2] == bits[1] + ".dll":
                out.append((path, e))
    return sorted(out, key=lambda x: (x[0] not in MAIN_FILES, x[0]))


def part_program(part: str) -> str:
    """campaign | mp | level:<name> for a program file's path on the disc."""
    if part in MAIN_FILES:
        return MAIN_FILES[part]
    return "level:" + part.split("/")[1]


def decrypt_xex(data: bytes) -> tuple[bytes, int]:
    """The memory image of an XEX2 file and its load address: tools/decomp/xex.py (basic compression,
    what Call of Duty 3's discs use), else tools/waw_decomp/xex2pe.py (LZX as well)."""
    import xex  # tools/decomp
    try:
        return xex.decrypt(data)
    except Exception:  # noqa: BLE001  - any other layout: let xex2pe.py try
        pass
    import xex2pe  # tools/waw_decomp
    try:
        image, info = xex2pe.convert(data)
    except SystemExit as e:  # xex2pe.py reports its errors with sys.exit
        raise RuntimeError(f"cannot decrypt: {e}") from None
    return image, info["load_addr"]


# ---------------------------------------------------------------- the work folder
class Work:
    """discmap's own folder: seen.tsv (what identify found where), images/<sha1>.pe (decrypted
    images, game data: they stay on this PC), match/<ref>__<other>/ (the tools' outputs),
    map_<build>.tsv, overrides_<build>.tsv, verify_<build>.txt."""

    SEEN_HEADER = ["build", "program", "part", "sha1_decrypted", "sha1_raw", "load", "source", "known"]

    def __init__(self, folder: Path):
        self.folder = folder
        self.images = folder / "images"
        self.folder.mkdir(parents=True, exist_ok=True)

    def seen(self) -> list[dict]:
        path = self.folder / "seen.tsv"
        return read_tsv(path) if path.exists() else []

    def remember(self, rows: list[list[str]]) -> None:
        """Add identify's rows to seen.tsv (one row per image and source)."""
        old = self.seen()
        keep = {(r["sha1_decrypted"], r["source"], r["part"]): r for r in old}
        for row in rows:
            keep[(row[3], row[6], row[2])] = dict(zip(self.SEEN_HEADER, row))
        rows = sorted(keep.values(), key=lambda r: (r["build"], r["part"]))
        write_tsv(self.folder / "seen.tsv", self.SEEN_HEADER, [[r[h] for h in self.SEEN_HEADER] for r in rows])

    def image_path(self, sha1: str) -> Path:
        return self.images / f"{sha1}.pe"

    def store_image(self, sha1: str, image: bytes) -> Path:
        self.images.mkdir(parents=True, exist_ok=True)
        path = self.image_path(sha1)
        if not path.exists():
            tmp = path.with_suffix(".part")
            tmp.write_bytes(image)
            tmp.replace(path)
        return path

    def map_path(self, build: str) -> Path:
        return self.folder / f"map_{build}.tsv"

    def match_dir(self, ref: str, other: str) -> Path:
        d = self.folder / "match" / f"{ref}__{other}"
        d.mkdir(parents=True, exist_ok=True)
        return d

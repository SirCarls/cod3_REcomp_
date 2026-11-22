"""[waw-decomp] 2026-11-16 When the codegen has to translate everything again, and whether the generated code is still
what it wrote.

The key (kept next to the generated code in generated/c3w-codegen-sdk.json) was the whole SDK build stamp, so any SDK
patch change (graphics, input, a header) made the codegen translate everything again (35-43 s for both programs),
though nothing came out different. The codegen is the installed rexglue program, and its output depends on:
- the codegen tool: rexglue itself (its templates are built into it), the SDK libraries it loads (on Linux
  librexruntime.so, through which it reads the Xbox 360 program) and, to be safe, the app sources the SDK ships next
  to it (share/rexglue/): tool_key();
- the program's inputs: its manifest, the config tomls it and they name (codmp_campaign.toml included) and the
  disc's executables (by SHA-1, as the setup tool identifies them): project_inputs(). The codegen checks these
  itself too, module by module; in the key, a change translates the whole program again.

Two things in the tool's files change with every SDK build without changing what the codegen writes: the SDK's build
timestamp (rex/version.h's REXGLUE_BUILD_TIMESTAMP, set each time the SDK is configured; the runtime library carries
it for the game's overlay) and the GNU build ID (a hash of the whole file, so it changes with the timestamp). They
are blanked before hashing: exactly the timestamp's characters wherever they occur, and the 20 bytes of the build ID
note. Everything else is compared byte for byte. A file whose format this doesn't know is hashed as it is (then any
SDK build translates everything again, as before).

Because the codegen no longer reruns after every SDK change, a generated file changed by hand (or left stale) would
otherwise stay. So after each codegen run the SHA-256 of every generated file is written down
(write_generated_manifest), and before each compile and each codegen run generated_files_match() checks every byte;
the pipeline translates everything again on any difference (rc2-mpsafe's MP safety review, route B2).
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import struct
from pathlib import Path
from typing import Dict, List, Optional

KEY_VERSION = 2
TIMESTAMP = re.compile(rb'#define\s+REXGLUE_BUILD_TIMESTAMP\s+"(\d{8}_\d{4})"')
# An ELF note header for a 20-byte GNU build ID: name size 4, description size 20, type 3 (NT_GNU_BUILD_ID), "GNU".
BUILD_ID_NOTE = struct.pack("<III", 4, 20, 3) + b"GNU\0"
LIBRARY_SUFFIXES = (".so", ".dll", ".dylib")
# A quoted file name in a manifest or config toml: another toml, or a program image the codegen reads.
INPUT_NAME = re.compile(r'"([^"\r\n]+\.(?:toml|xex|dll))"', re.IGNORECASE)
IMAGE_SUFFIXES = (".xex", ".dll")
# What the compile reads of the generated code (the codegen's own bookkeeping, its stamps, depfile and partition
# file, is left out: it changes when the codegen runs without changing any code).
GENERATED_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inl", ".cmake")
GENERATED_MANIFEST = "c3w-codegen-files.json"


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_timestamp(sdk_install: Path) -> Optional[bytes]:
    """The SDK build's timestamp as rex/version.h in the install has it, or None."""
    try:
        m = TIMESTAMP.search((Path(sdk_install) / "include" / "rex" / "version.h").read_bytes())
    except OSError:
        return None
    return m.group(1) if m else None


def elf_needed(data: bytes) -> Optional[List[str]]:
    """The libraries a 64-bit little-endian ELF file names as needed (DT_NEEDED), or None when it isn't one."""
    if data[:6] != b"\x7fELF\x02\x01" or len(data) < 64:
        return None
    try:
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
        sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
        needed = []
        for _name, kind, _flags, _addr, offset, size, link, _info, _align, _entsize in sections:
            if kind != 6:   # SHT_DYNAMIC
                continue
            strings = sections[link][4]
            for pos in range(offset, offset + size, 16):
                tag, value = struct.unpack_from("<qQ", data, pos)
                if tag == 0:
                    break
                if tag == 1:   # DT_NEEDED
                    end = data.index(b"\0", strings + value)
                    needed.append(data[strings + value:end].decode("utf-8", "replace"))
        return needed
    except (struct.error, IndexError, ValueError):
        return None


def _is_library(path: Path) -> bool:
    name = path.name.lower()
    return path.is_file() and (name.endswith(LIBRARY_SUFFIXES) or ".so." in name)


def loaded_libraries(program: Path, sdk_install: Path) -> List[Path]:
    """The SDK install's libraries `program` loads, and the ones those load, and so on. For an ELF file its
    DT_NEEDED list; for any other file every install library whose name appears in it (more than it needs, never
    fewer)."""
    available = {}
    for folder in (Path(sdk_install) / "lib", Path(sdk_install) / "bin"):
        if folder.is_dir():
            for p in sorted(folder.iterdir()):
                if _is_library(p):
                    available.setdefault(p.name.lower(), p)
    found: Dict[str, Path] = {}
    todo = [Path(program)]
    while todo:
        data = todo.pop().read_bytes()
        needed = elf_needed(data)
        if needed is not None:
            names = [n.lower() for n in needed if n.lower() in available]
        else:
            lowered = data.lower()
            names = [n for n in available if n.encode("utf-8") in lowered]
        for n in names:
            if n not in found:
                found[n] = available[n]
                todo.append(available[n])
    return [found[n] for n in sorted(found)]


def normalized_sha256(data: bytes, timestamp: Optional[bytes]) -> str:
    """SHA-256 of `data` with the SDK build's timestamp and the GNU build ID blanked (see the module's description)."""
    buf = bytearray(data)
    if timestamp:
        start = buf.find(timestamp)
        while start >= 0:
            buf[start:start + len(timestamp)] = b"\0" * len(timestamp)
            start = buf.find(timestamp, start + len(timestamp))
    start = buf.find(BUILD_ID_NOTE)
    while start >= 0:
        desc = start + len(BUILD_ID_NOTE)
        buf[desc:desc + 20] = b"\0" * 20
        start = buf.find(BUILD_ID_NOTE, desc + 20)
    return hashlib.sha256(bytes(buf)).hexdigest()


def tool_key(sdk_install: Path, program_name: str) -> Optional[dict]:
    """The codegen tool's part of the key for the SDK installed in `sdk_install` (program_name: "rexglue" or
    "rexglue.exe"), or None when its rexglue isn't there (the caller then falls back to the SDK build stamp)."""
    install = Path(sdk_install)
    program = install / "bin" / program_name
    if not program.is_file():
        return None
    timestamp = build_timestamp(install)
    shipped = install / "share" / "rexglue"
    extra = sorted(p for p in shipped.rglob("*") if p.is_file()) if shipped.is_dir() else []
    files = {}
    for p in [program] + loaded_libraries(program, install) + extra:
        files[p.relative_to(install).as_posix()] = normalized_sha256(p.read_bytes(), timestamp)
    return {"codegen_key": KEY_VERSION, "files": files}


def project_inputs(proj: Path) -> Dict[str, str]:
    """The codegen's inputs in one program's folder: its manifest (*_manifest.toml), every .toml in its config/
    folder, and every toml and program image those name (relative to the naming file's folder or to `proj`).
    Program images by SHA-1, everything else by SHA-256; keys are paths relative to `proj`."""
    proj = Path(proj)
    todo = sorted(proj.glob("*_manifest.toml")) + sorted((proj / "config").rglob("*.toml"))
    seen: Dict[Path, tuple] = {}
    while todo:
        p = todo.pop()
        real = p.resolve()
        if real in seen or not p.is_file():
            continue
        data = p.read_bytes()
        image = p.suffix.lower() in IMAGE_SUFFIXES
        seen[real] = (p, hashlib.sha1(data).hexdigest() if image else hashlib.sha256(data).hexdigest())
        if p.suffix.lower() == ".toml":
            for name in INPUT_NAME.findall(data.decode("utf-8", "replace")):
                for base in (p.parent, proj):
                    if (base / name).is_file():
                        todo.append(base / name)
                        break
    return {os.path.relpath(p, proj).replace(os.sep, "/"): h for p, h in sorted(seen.values())}


def generated_files(proj: Path) -> Dict[str, str]:
    """SHA-256 of every generated file the compile reads: those in each module folder under generated/ (the
    top-level generated/rexglue.cmake comes from the setup tool's template too, which puts it back when it
    differs)."""
    gen = Path(proj) / "generated"
    out = {}
    if gen.is_dir():
        for module in sorted(p for p in gen.iterdir() if p.is_dir()):
            for f in sorted(module.rglob("*")):
                if f.is_file() and f.suffix.lower() in GENERATED_SUFFIXES:
                    out[f.relative_to(gen).as_posix()] = _sha256_file(f)
    return out


def write_generated_manifest(proj: Path) -> None:
    """Right after a codegen run: the SHA-256 of every generated file (generated/c3w-codegen-files.json)."""
    path = Path(proj) / "generated" / GENERATED_MANIFEST
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps({"files": generated_files(proj)}, indent=0), encoding="utf-8")
    tmp.replace(path)


def generated_files_match(proj: Path) -> bool:
    """Whether every generated file has exactly the bytes the manifest says, and there are no others. False with no
    manifest (an older work folder, or a codegen run that didn't finish)."""
    try:
        files = json.loads((Path(proj) / "generated" / GENERATED_MANIFEST).read_text(encoding="utf-8")).get("files")
    except (OSError, ValueError, AttributeError):
        return False
    return isinstance(files, dict) and files == generated_files(proj)


def generated_files_stale(proj: Path) -> bool:
    """Whether `proj` holds generated code that is not exactly what the manifest says (no manifest counts as not
    exact). False when there is no generated code at all: then nothing stale can be compiled."""
    files = generated_files(proj)
    if not files:
        return False
    try:
        recorded = json.loads((Path(proj) / "generated" / GENERATED_MANIFEST).read_text(encoding="utf-8")).get("files")
    except (OSError, ValueError, AttributeError):
        return True
    return recorded != files


__all__ = ["build_timestamp", "elf_needed", "generated_files", "generated_files_match", "generated_files_stale",
           "loaded_libraries", "normalized_sha256", "project_inputs", "tool_key", "write_generated_manifest"]

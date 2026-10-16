"""What the built game needs to start, and gathering it into one folder.

The game is several files: the executable, the level libraries, and the
ReXGlue runtime and GPU plugin. A file that one of them needs but that isn't
packed next to it stops the game before a window opens, with the message
only visible in a terminal. (The first AppImage lacked
libamd_fidelityfx_vk.so and failed exactly like that.)

So packaging reads which libraries each file needs, straight from the file
(ELF on Linux, PE on Windows; no external tools), copies in the ones built
with the game, and stops with a clear message if anything else is missing
from this computer.
"""
from __future__ import annotations

import os
import struct
import subprocess
from pathlib import Path
from typing import Callable, Iterable, List, Optional, Set, Tuple

# ELF: dynamic section tags.
DT_NULL, DT_NEEDED, DT_STRTAB = 0, 1, 5
PT_LOAD, PT_DYNAMIC = 1, 2


# ---------------------------------------------------------------------------
# Reading the needed libraries
# ---------------------------------------------------------------------------
def elf_needed(path: Path) -> List[str]:
    """The DT_NEEDED entries of a 64-bit little-endian ELF file."""
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError(f"{path.name}: not a 64-bit little-endian ELF file")
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    loads: List[Tuple[int, int, int]] = []   # (vaddr, offset, filesz)
    dynamic: Optional[Tuple[int, int]] = None
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", data, phoff + i * phentsize)
        if p_type == PT_LOAD:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == PT_DYNAMIC:
            dynamic = (p_offset, p_filesz)
    if dynamic is None:
        return []   # statically linked

    def file_offset(vaddr: int) -> int:
        for start, offset, size in loads:
            if start <= vaddr < start + size:
                return vaddr - start + offset
        raise ValueError(f"{path.name}: address {vaddr:#x} is outside the file")

    needed_offsets: List[int] = []
    strtab = None
    offset, size = dynamic
    for at in range(offset, offset + size, 16):
        tag, value = struct.unpack_from("<qQ", data, at)
        if tag == DT_NULL:
            break
        if tag == DT_NEEDED:
            needed_offsets.append(value)
        elif tag == DT_STRTAB:
            strtab = file_offset(value)
    if strtab is None:
        return []
    names = []
    for o in needed_offsets:
        end = data.index(b"\0", strtab + o)
        names.append(data[strtab + o:end].decode("utf-8", "replace"))
    return names


def pe_imports(path: Path) -> List[str]:
    """The DLLs a Windows program or DLL imports, including delay-loaded ones."""
    data = Path(path).read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{path.name}: not a Windows program")
    pe, = struct.unpack_from("<I", data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path.name}: not a Windows program")
    sections, = struct.unpack_from("<H", data, pe + 6)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    magic, = struct.unpack_from("<H", data, optional)
    directories = optional + (112 if magic == 0x20B else 96)   # PE32+ or PE32
    table = optional + optional_size
    spans = []
    for i in range(sections):
        s = table + 40 * i
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, s + 8)
        spans.append((vaddr, max(vsize, raw_size), raw_ptr))

    def offset(rva: int) -> int:
        for vaddr, size, raw in spans:
            if vaddr <= rva < vaddr + size:
                return rva - vaddr + raw
        raise ValueError(f"{path.name}: RVA {rva:#x} is outside the file")

    def name_at(rva: int) -> str:
        o = offset(rva)
        return data[o:data.index(b"\0", o)].decode("ascii", "replace")

    names: List[str] = []
    # Directory 1: imports (20-byte descriptors, Name at +12).
    rva, _ = struct.unpack_from("<II", data, directories + 8 * 1)
    if rva:
        at = offset(rva)
        while True:
            entry = data[at:at + 20]
            if len(entry) < 20 or entry == bytes(20):
                break
            names.append(name_at(struct.unpack_from("<I", entry, 12)[0]))
            at += 20
    # Directory 13: delay-load imports (32-byte descriptors, DllNameRVA at +4).
    rva, _ = struct.unpack_from("<II", data, directories + 8 * 13)
    if rva:
        at = offset(rva)
        while True:
            entry = data[at:at + 32]
            if len(entry) < 32 or entry == bytes(32):
                break
            names.append(name_at(struct.unpack_from("<I", entry, 4)[0]))
            at += 32
    return names


def needed(path: Path) -> List[str]:
    """The libraries `path` needs, whichever kind of binary it is."""
    with open(path, "rb") as f:
        head = f.read(4)
    if head == b"\x7fELF":
        return elf_needed(path)
    if head[:2] == b"MZ":
        return pe_imports(path)
    raise ValueError(f"{Path(path).name}: not a program or library")


def elf_section_end(path: Path) -> int:
    """Where an ELF file's section header table ends. For an AppImage this is
    where its runtime ends and the packed file system starts."""
    with open(path, "rb") as f:
        head = f.read(64)
    if head[:4] != b"\x7fELF" or head[4] != 2 or head[5] != 1:
        raise ValueError(f"{Path(path).name}: not a 64-bit little-endian ELF file")
    shoff, = struct.unpack_from("<Q", head, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", head, 0x3A)
    return shoff + shentsize * shnum


# ---------------------------------------------------------------------------
# What the system already has
# ---------------------------------------------------------------------------
def linux_system_libraries() -> Set[str]:
    """Library names the dynamic linker can find on this computer
    (`ldconfig -p`, falling back to the usual library folders)."""
    names: Set[str] = set()
    for ldconfig in ("ldconfig", "/sbin/ldconfig", "/usr/sbin/ldconfig"):
        try:
            out = subprocess.run([ldconfig, "-p"], capture_output=True, text=True, timeout=30).stdout
        except (OSError, subprocess.TimeoutExpired):
            continue
        for line in out.splitlines()[1:]:
            name = line.strip().split(" ", 1)[0]
            if name:
                names.add(name)
        if names:
            return names
    for folder in ("/lib", "/lib64", "/usr/lib", "/usr/lib64", "/lib/x86_64-linux-gnu",
                   "/usr/lib/x86_64-linux-gnu"):
        try:
            names.update(os.listdir(folder))
        except OSError:
            pass
    return names


# The Visual C++ runtime. The build PC has it in System32 (Visual Studio
# installs it), but a PC without the "Visual C++ Redistributable" doesn't, so
# it's packed next to the game (Microsoft allows that "app-local" copy).
VC_RUNTIME_PREFIXES = ("msvcp140", "vcruntime140", "concrt140", "vccorlib140", "vcomp140")


def windows_is_system_dll(name: str) -> bool:
    low = name.lower()
    if low.startswith(VC_RUNTIME_PREFIXES):
        return False
    if low.startswith(("api-ms-win-", "ext-ms-")):
        return True   # API sets, resolved by Windows itself
    root = os.environ.get("SystemRoot") or os.environ.get("windir")
    if not root:
        return False
    return any((Path(root) / sub / name).exists() for sub in ("System32", "SysWOW64"))


def windows_runtime_from_system(missing: List[Tuple[str, str]]) -> Tuple[List[Path], List[Tuple[str, str]]]:
    """The Visual C++ runtime DLLs among `missing` ((file, library) pairs
    from gather), taken from Windows' System32 folder: the copies of the
    Visual C++ Redistributable, which Visual Studio's installer puts there.
    For a Visual Studio without its own redistributable folder. Returns the
    files found and what is still missing."""
    root = os.environ.get("SystemRoot") or os.environ.get("windir")
    system32 = Path(root) / "System32" if root else None
    try:
        # By lower-case name: import tables say VCRUNTIME140.dll.
        listing = {name.lower(): system32 / name for name in os.listdir(system32)} if system32 else {}
    except OSError:
        listing = {}
    found: List[Path] = []
    still: List[Tuple[str, str]] = []
    for by, lib in missing:
        system_copy = listing.get(lib.lower())
        if lib.lower().startswith(VC_RUNTIME_PREFIXES) and system_copy and system_copy.is_file():
            if system_copy not in found:
                found.append(system_copy)
        else:
            still.append((by, lib))
    return found, still


def vc_runtime_dirs(vs_path: Optional[Path]) -> List[Path]:
    """Visual Studio's redistributable copies of the C++ runtime (x64),
    newest first: VC/Redist/MSVC/<version>/x64/Microsoft.VC14x.CRT."""
    if not vs_path:
        return []
    redist = Path(vs_path) / "VC" / "Redist" / "MSVC"
    if not redist.is_dir():
        return []
    dirs = [d for d in redist.glob("*/x64/Microsoft.VC14*.CRT") if d.is_dir()]
    return sorted(dirs, key=lambda d: [int(x) if x.isdigit() else 0 for x in d.parts[-3].split(".")],
                  reverse=True)


# ---------------------------------------------------------------------------
# Gathering
# ---------------------------------------------------------------------------
# The SDK's GPU emulation is a plugin the game loads by name at startup
# (rex::system::LoadGpuPlugin), so no import table mentions it: it is looked
# up explicitly. The file is named after the build that made it.
GPU_PLUGIN = "xenos"


def gpu_plugin_names(name: str, windows: bool) -> List[str]:
    """The file names the SDK's GPU plugin can have: Visual Studio builds
    `rexgpu-<name>.dll`, MinGW (the Windows game built on Linux) adds CMake's
    `lib` prefix, and Linux has `librexgpu-<name>.so`. The game's loader
    accepts both Windows names (SDK patch 0023)."""
    if windows:
        return [f"rexgpu-{name}.dll", f"librexgpu-{name}.dll"]
    return [f"librexgpu-{name}.so"]


def find_gpu_plugin(name: str, search: Iterable[Path], windows: bool) -> Optional[Path]:
    """The GPU plugin's file in the first `search` folder that has it under
    any of its names, or None."""
    for folder in search:
        for file_name in gpu_plugin_names(name, windows):
            candidate = Path(folder) / file_name
            if candidate.is_file():
                return candidate
    return None


def _is_windows_binary(path: Path) -> bool:
    with open(path, "rb") as f:
        return f.read(2) == b"MZ"


def gather(binaries: Iterable[Path], search: Iterable[Path], is_system: Callable[[str], bool]
           ) -> Tuple[List[Path], List[Tuple[str, str]]]:
    """Adds to `binaries` every library they need (directly or through each
    other) that is found in the `search` folders, i.e. was built with the
    game. Returns all the files to ship, and (file, library) pairs for
    libraries found neither there nor on this system.

    Windows file names ignore case (an import table says KERNEL32.dll for
    kernel32.dll), so for Windows programs names are matched regardless of
    case; Linux names are matched exactly."""
    files = [Path(b) for b in binaries]
    folders = [Path(s) for s in search]
    windows = bool(files) and _is_windows_binary(files[0])
    fold = str.lower if windows else str
    have = {fold(f.name) for f in files}
    # Each folder's files, by folded name, so a differently-cased import
    # still finds the file.
    listings = []
    for folder in folders:
        try:
            listings.append({fold(entry): folder / entry for entry in os.listdir(folder)})
        except OSError:
            listings.append({})
    missing: List[Tuple[str, str]] = []
    queue = list(files)
    while queue:
        current = queue.pop()
        for lib in needed(current):
            key = fold(lib)
            if key in have:
                continue
            found = next((entries[key] for entries in listings if key in entries and entries[key].is_file()),
                         None)
            if found:
                files.append(found)
                have.add(key)
                queue.append(found)
            elif not is_system(lib):
                missing.append((current.name, lib))
    return files, missing


__all__ = ["elf_needed", "pe_imports", "needed", "elf_section_end", "linux_system_libraries",
           "windows_is_system_dll", "windows_runtime_from_system", "GPU_PLUGIN", "gpu_plugin_names",
           "find_gpu_plugin", "gather"]

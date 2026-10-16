"""The Rust ripper (engine/tools/rip): getting it and running it.

The ripper turns the user's game files into open formats for the native
engine: textures (DDS + PNG), meshes (glTF), materials (JSON), scripts.

It comes from one of two places:

1. a prebuilt binary shipped in a release download,
   `engine/bin/<platform>/rip[.exe]`;
2. otherwise it is built from `engine/` with cargo. Build files go into the
   work folder, so the project folder stays clean (and may be read-only).
"""
from __future__ import annotations

import os
import re
import shutil
from pathlib import Path
from typing import Callable, List, Optional, Tuple

from .common import (APPROVAL, IS_WINDOWS, Install, Log, Runner, SetupError, exe_name, host_triple, read_json,
                     write_json)

# What `rip version` must print for a prebuilt binary to be used; bumped
# whenever the output layout changes. 2 (2026-10-10): textures/hd_keys.tsv
# and the cod3-hd-pack command, for the HD textures in the playable game.
# 3: cod3-hd-pack takes upscaled PNGs of any scale (1x, 2x, 4x ...), not only 2x.
# 4: hd_keys.tsv names each key's own picture. 5: the cod3-user-pack,
# cod3-check-pack and cod3-locate commands (c3w/user_textures.py). 6:
# hd_keys.tsv's kind column (only color pictures are upscaled), small
# textures read from the right place, 8_8_8_8 channels in the right order.
RIP_OUTPUT_VERSION = 6
# Written next to the ripped files (data/cod3) after a rip: the output layout
# they have, so a check (c3w/repair.py) can tell an older rip.
LAYOUT_FILE = "rip_layout.json"


def engine_dir(tool_root: Path) -> Path:
    """engine/ sits next to the setup tool's folder in the project."""
    return tool_root.parent / "engine"


def crates_install(cargo_home: Optional[Path] = None) -> Install:
    """The approval entry for the libraries cargo downloads to build the
    ripper (miniz_oxide and what it needs, pinned by Cargo.lock)."""
    home = cargo_home or Path(os.environ.get("CARGO_HOME") or Path.home() / ".cargo")
    return Install("crates", "Rust libraries the ripper is built with, pinned by engine/Cargo.lock",
                   str(home / "registry"), "a few MB", "crates.io")


def needs_build(tool_root: Path, runner: Runner) -> bool:
    """True when there's no prebuilt ripper that fits, so get_rip builds one."""
    ready = prebuilt(tool_root)
    return not (ready and _works(ready, runner))


def prebuilt(tool_root: Path) -> Optional[Path]:
    exe = engine_dir(tool_root) / "bin" / host_triple() / exe_name("rip")
    return exe if exe.is_file() else None


def works(rip: Path, runner: Runner) -> bool:
    """True when `rip` runs and writes the current output layout."""
    return _works(rip, runner)


def _works(rip: Path, runner: Runner) -> bool:
    rc, out = runner.capture([rip, "version"], timeout=30)
    m = re.search(r"output layout (\d+)", out)
    return rc == 0 and m is not None and int(m.group(1)) == RIP_OUTPUT_VERSION


def get_rip(tool_root: Path, target_dir: Path, cargo: Optional[Path], jobs: int, runner: Runner,
            log: Log, env: Optional[dict] = None) -> Path:
    """A working ripper: the prebuilt one if it fits, else a fresh build."""
    ready = prebuilt(tool_root)
    if ready and _works(ready, runner):
        log.info(f"Using the prebuilt ripper {ready}")
        return ready
    engine = engine_dir(tool_root)
    if not (engine / "Cargo.toml").is_file():
        raise SetupError(f"The ripper's source is missing ({engine}). Keep the 'engine' folder next to "
                         "the setup tool.")
    if not cargo:
        raise SetupError("Building the ripper needs Rust. Turn on 'Install missing tools' and run again, "
                         "or install it from https://rustup.rs")
    APPROVAL.require([crates_install()], log)
    log.info("Building the ripper (Rust, about a minute the first time)...")
    build_env = dict(env or {})
    build_env["CARGO_TARGET_DIR"] = str(target_dir)
    # cargo lives next to rustc; make sure that folder is on PATH too.
    build_env["PATH"] = str(Path(cargo).parent) + (";" if IS_WINDOWS else ":") + build_env.get("PATH", "")
    runner.run([cargo, "build", "--release", "--locked", "--jobs", str(max(1, jobs)), "-p", "rip"],
               cwd=engine, env=build_env)
    rip = target_dir / "release" / exe_name("rip")
    if not rip.is_file():
        raise SetupError("The ripper build finished but no rip binary was produced; see the log.")
    return rip


WINDOWS_TARGET = "x86_64-pc-windows-gnu"


def build_windows_rip(tool_root: Path, target_dir: Path, cargo: Path, mingw: Optional[Path], jobs: int,
                      runner: Runner, log: Log, env: Optional[dict] = None) -> Path:
    """Builds the Windows ripper (rip.exe) on Linux: Rust's Windows GNU target,
    linked by the MinGW-w64 cross compiler. Returns the .exe."""
    if not mingw:
        raise SetupError("Making Windows .exe files on Linux needs the MinGW-w64 cross compiler "
                         "(x86_64-w64-mingw32-gcc). Turn on 'Install missing tools' and run again.")
    rustup = Path(cargo).with_name("rustup")
    if not rustup.is_file():
        raise SetupError("Making Windows .exe files needs rustup (it adds Rust's Windows target).")
    log.info(f"Adding Rust's Windows target ({WINDOWS_TARGET})...")
    runner.run([rustup, "target", "add", WINDOWS_TARGET], env=env)
    build_env = dict(env or {})
    build_env["CARGO_TARGET_DIR"] = str(target_dir)
    build_env["CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER"] = str(mingw)
    build_env["PATH"] = str(Path(cargo).parent) + ":" + build_env.get("PATH", "")
    log.info("Building the Windows ripper (rip.exe)...")
    runner.run([cargo, "build", "--release", "--locked", "--jobs", str(max(1, jobs)), "-p", "rip",
                "--target", WINDOWS_TARGET], cwd=engine_dir(tool_root), env=build_env)
    exe = target_dir / WINDOWS_TARGET / "release" / "rip.exe"
    if not exe.is_file():
        raise SetupError("The Windows ripper build finished but no rip.exe was produced; see the log.")
    return exe


def install_copy(rip: Path, output: Path) -> Path:
    """Copies the ripper into the output folder (tools/), so the launcher can
    rip custom maps later without the project folder."""
    dest = output / "tools" / (rip.name if rip.suffix.lower() == ".exe" else exe_name("rip"))
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.exists() or rip.resolve() != dest.resolve():
        shutil.copy2(rip, dest)
    return dest


def _archive_progress(log: Log) -> Callable[[str], None]:
    """Turns `rip cod3-all` output into progress: it first prints the archive
    count, then two lines per finished archive."""
    state = {"total": 0, "done": 0}
    count = re.compile(r"^(\d+) archives")

    def handle(line: str) -> None:
        m = count.match(line)
        if m:
            state["total"] = int(m.group(1))
        elif line.endswith("materials") and state["total"]:
            state["done"] += 1
            log.progress(state["done"] / state["total"], f"ripped {state['done']}/{state['total']} archives")
        if line.startswith(("error", "done", "  skipped")) or "not found" in line:
            log.output(line)
    return handle


def rip_cod3(rip: Path, game_sp: Path, out_dir: Path, jobs: int, runner: Runner, log: Log) -> None:
    """Rips every CoD3 archive under the extracted disc's sp/ folder."""
    out_dir.mkdir(parents=True, exist_ok=True)
    runner.run([rip, "cod3-all", game_sp, out_dir, "--jobs", str(max(1, jobs))], quiet=True,
               line_filter=_archive_progress(log))
    record_layout(out_dir)


def record_layout(out_dir: Path) -> None:
    """Notes in out_dir (LAYOUT_FILE) that its rip has the current layout."""
    write_json(out_dir / LAYOUT_FILE, {"output_layout": RIP_OUTPUT_VERSION})


def recorded_layout(out_dir: Path) -> Optional[int]:
    """The output layout LAYOUT_FILE gives for the rip in out_dir, or None
    (no file: a rip made before the file was written, or none at all)."""
    data = read_json(out_dir / LAYOUT_FILE)
    value = data.get("output_layout") if isinstance(data, dict) else None
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def _pack_progress(log: Log) -> Callable[[str], None]:
    """Turns `rip cod3-hd-pack` output into progress. Its exact wording is
    the ripper's business, so this accepts the usual shapes: a "done/total"
    pair anywhere in a line, or a first line giving the texture count
    ("8123 textures ...") followed by one line per written ".dds" file.
    Errors, skips and the summary go to the log; per-texture lines don't (there
    are thousands)."""
    state = {"total": 0, "done": 0}
    fraction = re.compile(r"\b(\d+)\s*/\s*(\d+)\b")
    count = re.compile(r"^(\d+) textures")
    always = ("error", "warning", "done")       # problems and the closing summary
    summary = ("skipped", "packed", "wrote", "scales")   # a summary, unless it names one texture

    def handle(line: str) -> None:
        text = line.strip()
        lower = text.lower()
        m_count = count.match(text)
        m_frac = fraction.search(text)
        per_texture = False
        if m_count and not m_frac:
            state["total"] = int(m_count.group(1))
        elif m_frac and int(m_frac.group(2)) > 0:
            per_texture = True
            done, total = int(m_frac.group(1)), int(m_frac.group(2))
            log.progress(min(done / total, 1.0), f"packed {done}/{total} HD textures")
        elif text.endswith(".dds"):
            per_texture = True
            if state["total"]:
                state["done"] += 1
                log.progress(min(state["done"] / state["total"], 1.0),
                             f"packed {state['done']}/{state['total']} HD textures")
        if lower.startswith(always) or "not found" in lower or \
                (not per_texture and (lower.startswith(summary) or "missing" in lower or "wrong size" in lower)):
            log.output(line)
    return handle


def hd_pack(rip: Path, textures: Path, upscaled: Path, out_dir: Path, jobs: int, runner: Runner,
            log: Log) -> int:
    """`rip cod3-hd-pack`: the waifu2x copies in `upscaled` compressed into the
    game's own texture formats (at the copies' size, whatever the scale; full
    mip chain) in `out_dir`, with
    the index the game's texture replacement reads (textures/hd_keys.tsv says
    which game texture each name is). Returns the exit code; the caller
    decides what a failure means."""
    out_dir.mkdir(parents=True, exist_ok=True)
    return runner.run([rip, "cod3-hd-pack", textures, upscaled, out_dir, "--jobs", str(max(1, jobs))],
                      quiet=True, check=False, line_filter=_pack_progress(log))


def fastfile_folder_name(ff: Path) -> str:
    """The output folder for a fastfile: its name without .ff, but never "."
    or ".." (a file called "...ff" would otherwise rip into the parent)."""
    stem = ff.stem
    return stem if stem.strip(". ") else "fastfile_" + ff.name.replace(".", "_")


def rip_fastfiles(rip: Path, files: List[Path], out_dir: Path, runner: Runner, log: Log
                  ) -> Tuple[int, List[str]]:
    """Rips World at War fastfiles, one output folder each. Returns how many
    worked and the errors of those that didn't."""
    ok, errors = 0, []
    for i, ff in enumerate(files):
        runner.check_cancel()   # capture() doesn't watch Cancel itself
        log.progress(i / max(len(files), 1), f"{ff.name}")
        rc, out = runner.capture([rip, "waw-ff", ff, out_dir / fastfile_folder_name(ff)], timeout=600)
        if rc == 0:
            ok += 1
        else:
            last = out.strip().splitlines()[-1] if out.strip() else f"exit code {rc}"
            errors.append(f"{ff.name}: {last}")
    log.progress(1.0, f"ripped {ok}/{len(files)} fastfiles")
    return ok, errors

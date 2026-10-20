"""The build pipeline.

    CoD3 ISO -> extracted game files -> ripped CoD3 data (Rust ripper)
    World at War (Steam folder / Xbox 360 image / PC disc) -> ripped WaW data
      (+ map packs from the official PC patches, for the discs)
    optional: ReXGlue SDK -> recompiled CoD3 (playable now) -> AppImage / .exe
    -> output folder -> cleanup

Every step can be re-run: finished work is detected and skipped, so a
canceled or failed build continues where it stopped.

Inputs are never modified or deleted: the CoD3 ISO / extracted folder and the
World at War source are opened read-only and are on the protected list that
every delete goes through (common.safe_rmtree). World at War is never run.
"""
from __future__ import annotations

import hashlib
import os
import re
import shutil
import struct
import sys
import threading
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, List, Optional

from . import binaries, cross, deps, map_packs, pc_check, rippers, sdk_shaders, upscale, user_textures, waw
from .common import (APPROVAL, IS_LINUX, IS_WINDOWS, WORK_MARKER, Cancelled, Install, Log, Runner, SetupError,
                     build_path, claim_work_dir, copy_tree, dir_size, owns_work_dir,
                     exe_name, free_space, host_triple, human, is_within, overlaps, read_json, remove_build_link,
                     resolved, safe_rmtree, sha1_file, write_json)
from .xiso import XisoImage

# Bump this with every release that changes the setup tool or what it builds (owner rule, 2026-10-14):
# the last number for fixes, the middle one for new features. repair.py compares it with the installed
# launcher's copy, so a bump also tells existing installs to refresh their launcher.
TOOL_VERSION = "2.10.18"  # 2.10.18: keys call the game's own actions (both programs): fire, aim, use and reload apart, grenades, stance, lean, weapons ... each on a key of its own, the controller unchanged (docs/KEYBINDS.md); 2.10.17: sounds no longer play twice: the LFE slot's copy of the rear channels dropped on 6-channel outputs (cod3_audio_lfe), the ends of XMA sounds no longer replayed and the game's audio callback paced like the console's hardware (SDK 0061); 2.10.16: multiplayer: the keyboard's grenade keys follow its controller layout (SDK 0056), the campaign's controls kept over the profile's saved ones at a switch, one crosshair choice for both programs; 2.10.15: readback_resolve full without the GPU thread waiting at each resolve (SDK 0050: +6% to +22% fps in full, level with fast); 2.10.14: the GPU emulation no longer waits for the global lock behind /proc/self/maps reads in the write-watch fault handler (SDK 0052, Linux 6.11+: +21% to +71% fps in the levels that waited most); profiler: a sampling thread instead of the CPU-time timer, the render job and Direct3D wait reasons, a verdict for the lock; 2.10.13: CoD4 bounces only for a jump falling onto a steep slope (no lift walking up an uneven incline); the sound output follows a switch of the system's default while playing (SDK 0051); 2.10.12: the CoD4 movement style moves in CoD4 PC's 125 fps steps (its air strafing and jump height) at any frame rate; 2.10.11: sprint in the campaign (hold/toggle) and the optional CoD4 movement style, binoculars on a key of their own (B); 2.10.10: HDR10 output (SDK 0044: BT.2020, PQ, subtle highlights; F2 > Video > Colors); 2.10.9: aspect ratio: custom shapes from 4:3, "Match the render resolution", F2 dropdowns fixed, Letterbox off planned as stretched; 2.10.8: the campaign's port features in multiplayer (cod3mp: frame limit, FOV, crosshair, F2, console, cheat guard, screenshots; tools/mp_aliases.py), clean launch path back to the campaign (SDK 0043); 2.10.7: the game is collected when it ends (no "ghost" left by the launcher); 2.10.6: pause menu BRIGHTNESS, clickable menu arrows and gauges; 2.10.5: readback copies only what the CPU reads (SDK 0049), Direct3D waits by reason; 2.10.4: HD textures on/off screenshot pair (cod3_hd_compare); 2.10.3: debug screenshots of the game's own picture; 2.10.2: texture descriptor sets reused (SDK 0048), profiler: what runs under the global lock; 2.10.1: window paint pipeline kept (SDK 0046), register writes (0047), profiler callers; 2.10.0: settings: the last change wins (SDK 0042, Play page); 2.9.1: the game's job wait sleeps, global lock spin (0045), automatic profiles; 2.9.0: QTE spinner, decomp trace version (--trace-build); 2.8.0: regional discs, console language, F2 picture colors; [voice] 2.7.0: voices from another disc
SDK_REPO = "https://github.com/rexglue/rexglue-sdk.git"
SDK_COMMIT = "c94f5ebdcb3c9d1a460ca48e04f9758448f8d518"   # ReXGlue v0.10.0

# default.xex of "Call of Duty 3 (USA, Europe)". The recomp's hooks are tied
# to this exact build, so other versions would compile into a broken game.
# The campaign's default.xex of each disc the recomp builds. The German and the
# French/Spanish/Italian discs carry the same program (the decrypted image and
# the 15 level DLLs are byte for byte the USA/Europe disc's; only the XEX
# wrapper differs: docs/X360_FAMILY.md), so every hook matches them too.
KNOWN_XEX = {"681724c40e250e584e6585dd903e05afa00a8634": "Call of Duty 3 (USA, Europe)",
             "52447cb4189bc4168e71324255e9e7acf5495eb8": "Call of Duty 3 (Germany)",
             "6f9247bb74c4006632a8556dcd1d41332139ff83": "Call of Duty 3 (Europe: French, Spanish, Italian)"}
# The French/Spanish/Italian disc has no English speech: the game takes its
# language from the console language (Game settings > Console language).
XEX_PICKS_LANGUAGE = {"6f9247bb74c4006632a8556dcd1d41332139ff83"}
LEVELS = ["blkbrn", "chambois", "credits", "crssrds", "falaise", "forest", "fuelplnt", "hostage", "island",
          "laison", "mace2", "mayenne", "nightd", "saint_lo", "stbert"]
RECOMP_FILES = ["default.xex"] + [f"sp/{lvl}/{lvl}.dll" for lvl in LEVELS]
# The multiplayer program, recompiled next to the campaign as cod3mp
# (recomp/mp/, docs/MULTIPLAYER.md). Its hooks are tied to this exact build
# too; with another (or none) the campaign is built alone.
MP_XEX = "codmp_xenonf.xex"
KNOWN_MP_XEX = {"907985e2f1bdea255b52226ef39acf627c2d7ffc": "Call of Duty 3 (USA, Europe) multiplayer"}

WORK_NEEDED_RECOMP = 7 * 1024 ** 3   # SDK checkout + build, generated code, objects
WORK_NEEDED_RIPPER = 1 * 1024 ** 3   # the ripper's build, unpacked WaW discs
# The decomp trace version (docs/DECOMP_TRACE.md): its own build folder next
# to the normal one, packed into <output>/bin-trace/.
TRACE_BUILD_DIR = "trace"
TRACE_BIN_DIR = "bin-trace"
WORK_NEEDED_TRACE = 2 * 1024 ** 3    # its objects (the translated code is shared)
BINARIES_SIZE = 600 * 1024 ** 2      # the recompiled game in the output
RIPPED_SIZE = 4 * 1024 ** 3          # ripped CoD3 + WaW data (whole disc: textures as DDS + PNG)
# With the waifu2x option: data/cod3/textures_upscaled (the ripped PNGs at the
# chosen scale), plus data/cod3/hd_textures, the same textures compressed for
# the game. Both grow with the square of the scale; these are the sizes at 1x.
# The pack is an estimate, about a third of the PNGs: DXT1 / DXT5 / ATI2
# store 0.5-1 byte per texel where the PNGs take roughly 2-4, the mip levels
# add a third, and the few uncompressed 32-bit textures take more.
UPSCALED_PNG_SIZE_1X = 768 * 1024 ** 2
HD_PACK_SIZE_1X = 256 * 1024 ** 2


def upscaled_png_size(scale: int) -> int:
    return UPSCALED_PNG_SIZE_1X * scale * scale


def hd_pack_size(scale: int) -> int:
    return HD_PACK_SIZE_1X * scale * scale


def upscaled_size(scale: int) -> int:
    """Disk space the upscaled copies and the HD pack take at `scale`."""
    return upscaled_png_size(scale) + hd_pack_size(scale)

# Output folder layout.
COD3_DATA = Path("data") / "cod3"
LOGS = Path("logs")             # setup logs, kept after the build
KEEP_LOGS = 10

# AMD FidelityFX (FSR / CAS scaling) is built on Windows only. Its build
# compiles shaders with AMD's FidelityFX_SC.exe, a Windows program; on Linux
# the SDK runs it through Wine, and this project runs nothing through Wine.
# (Found 2026-10-07: the owner's Linux build had used Wine for it.)
FIDELITYFX_PLATFORMS = ("win",)


def fidelityfx_available() -> bool:
    return host_triple().split("-")[0] in FIDELITYFX_PLATFORMS


@dataclass
class BuildOptions:
    cod3_source: str = ""          # Call of Duty 3 .iso, or a folder already extracted from it
    # World at War: a Steam folder, when given, is the only source used.
    # Otherwise `waw_alternative` picks the Xbox 360 image or the PC disc.
    waw_steam: str = ""
    waw_xbox360: str = ""
    waw_pc_disc: str = ""
    waw_alternative: str = waw.XBOX360
    # Official PC patch downloads with map packs (DLC), for the discs, which
    # lack them. Not used with a Steam folder, which has every map pack.
    waw_map_packs: List[str] = field(default_factory=list)
    output_dir: str = ""
    work_dir: str = ""
    jobs: int = 2
    build_recomp: bool = True      # the playable recompiled CoD3 (needs a C++ toolchain, 30-120 min)
    # Which versions of the game to build, whatever system this is: on Linux
    # the Windows one is cross-compiled (llvm-mingw); on Windows the Linux one
    # is built inside WSL 2 (c3w/wsl.py). By default, this system's own.
    build_linux: bool = IS_LINUX   # Linux version: CoD3-x86_64.AppImage
    build_windows: bool = IS_WINDOWS   # Windows version: cod3.exe (+ the Windows ripper on Linux)
    appimage: bool = True          # the Linux version as an AppImage (else a bin/ folder)
    binaries_only: bool = False    # internal (the build inside WSL): only the game, no rips
    cleanup: bool = True
    keep_sdk: bool = False         # keep the built SDK + downloaded tools for faster rebuilds
    install_deps: bool = True
    fidelityfx: bool = True        # AMD FSR / CAS output scaling in the game (Windows builds only)
    # Optional: the ripped CoD3 textures upscaled with waifu2x (1x, 2x or 4x)
    # into data/cod3/textures_upscaled (c3w/upscale.py), then packed for the
    # playable game into data/cod3/hd_textures. Uses the graphics card.
    upscale_textures: bool = False
    upscale_model: str = upscale.DEFAULT_MODEL
    upscale_noise: int = upscale.DEFAULT_NOISE
    upscale_scale: int = upscale.DEFAULT_SCALE
    force_unknown_iso: bool = False
    skip_dep_check: bool = False   # advanced: build even if detection reports something missing
    # Also the decomp trace version of this system's game (docs/DECOMP_TRACE.md):
    # notes which game functions run and who calls them, for mapping unnamed
    # ones; built in its own folder into <output>/bin-trace/ (the normal game is
    # unchanged). About doubles the compile time.
    trace_build: bool = False
    # A repair (c3w/repair.py): only the steps named here (Step.key) run,
    # plus the steps they need (STEP_NEEDS) and those in ALWAYS_STEPS. Empty
    # (a normal build): every step runs.
    only_steps: List[str] = field(default_factory=list)


@dataclass
class Step:
    title: str
    func: Callable[[], None]
    key: str = ""   # a short name for BuildOptions.only_steps


# What each step needs done first in the same run (for a repair, which runs
# only some steps): the ripper for anything that rips or packs, the game
# files for a rip or a compile, the translated code for the game builds.
STEP_NEEDS = {
    "rip_cod3": ("ripper", "assets"), "upscale": (), "hd_pack": ("ripper",), "user_pack": ("ripper",),
    "waw": ("ripper",), "recompile": ("sdk", "assets"), "package": ("recompile",),
    "windows": ("recompile",), "linux_wsl": ("assets",), "trace": ("recompile",),
}
# Steps every run has (the tools step only when another step uses tools).
ALWAYS_STEPS = ("inputs", "finish", "cleanup")
NO_TOOLS_STEPS = ("inputs", "assets", "finish", "cleanup")


class Pipeline:
    def __init__(self, opts: BuildOptions, log: Log, cancel: threading.Event,
                 on_step: Optional[Callable[[int, int, str], None]] = None):
        self.o = opts
        self.log = log
        self.cancel = cancel
        self.runner = Runner(log, cancel)
        # A failed command's error says what to do when the cause is known.
        self.runner.failure_hint = pc_check.failure_hint
        self.on_step = on_step or (lambda i, n, t: None)
        self.tool_root = Path(__file__).resolve().parent.parent
        self.template = self.tool_root / "recomp"
        self.patches = self.tool_root / "patches"
        self.out = resolved(Path(opts.output_dir))
        self.work_real = resolved(Path(opts.work_dir))
        self.work = self.work_real   # becomes a space-free link to it in run(), when needed
        self.tools_dir = self.work / "tools"
        self.cod3_src = resolved(Path(opts.cod3_source)) if opts.cod3_source else None
        self.waw, self.waw_notes = waw.choose_source(opts.waw_steam, opts.waw_xbox360, opts.waw_pc_disc,
                                                     opts.waw_alternative)
        if self.waw.path:
            self.waw.path = resolved(self.waw.path)
        self.map_packs = [resolved(Path(p)) for p in opts.waw_map_packs if p.strip()]
        self.assets = self.out / "assets"
        self.tc: Optional[deps.Toolchain] = None
        self.rip: Optional[Path] = None
        self.exe_rel: Optional[str] = None           # this system's version (bin/ or the AppImage)
        self.windows_exe_rel: Optional[str] = None   # cross-built on Linux
        self.trace_exe_rel: Optional[str] = None     # the decomp trace version (bin-trace/)
        self.linux_exe_rel: Optional[str] = None     # built in WSL on Windows
        self.used_wsl = False
        self.waw_ripped = 0
        self._reusable: Optional[tuple] = None   # see _reusable_rip
        self._prebuilt_ok: Optional[bool] = None   # see _prebuilt_rip_works
        # The game's files each version was packed with this run, relative to
        # the output folder ("linux" / "windows"), for the record (finish).
        self.packed: dict = {}

    # ------------------------------------------------------------------
    @property
    def protected(self) -> List[Path]:
        return [p for p in (self.cod3_src, self.waw.path, self.out, *self.map_packs) if p]

    @property
    def native(self) -> bool:
        """This system's own version is wanted."""
        return self.o.build_recomp and (self.o.build_linux if IS_LINUX else self.o.build_windows)

    @property
    def cross_windows(self) -> bool:
        """The Windows version, cross-compiled on Linux."""
        return self.o.build_recomp and IS_LINUX and self.o.build_windows

    @property
    def wsl_linux(self) -> bool:
        """The Linux version, built inside WSL on Windows."""
        return self.o.build_recomp and IS_WINDOWS and self.o.build_linux

    @property
    def repairing(self) -> bool:
        """Only some steps run (BuildOptions.only_steps; c3w/repair.py)."""
        return bool(self.o.only_steps)

    def _wanted_steps(self) -> set:
        """The step keys a repair runs: the ones asked for and what they need."""
        wanted = set(self.o.only_steps) | set(ALWAYS_STEPS)
        todo = list(wanted)
        while todo:
            for need in STEP_NEEDS.get(todo.pop(), ()):
                if need not in wanted:
                    wanted.add(need)
                    todo.append(need)
        if wanted - set(NO_TOOLS_STEPS):
            wanted.add("tools")
        return wanted

    def _runs(self, key: str) -> bool:
        """Whether this run includes the step `key` (always, in a normal build)."""
        return not self.repairing or key in self._wanted_steps()

    def steps(self) -> List[Step]:
        steps = [Step("Checking inputs", self.preflight, "inputs"), Step("Tools", self.dependencies, "tools"),
                 Step("Call of Duty 3 game files", self.cod3_assets, "assets")]
        if not self.o.binaries_only:
            steps += [
                Step("Ripper", self.ripper, "ripper"),
                Step("Ripping Call of Duty 3", self.rip_cod3, "rip_cod3"),
            ]
            if self.o.upscale_textures:
                steps.append(Step("Upscaling textures (waifu2x)", self.upscale_textures, "upscale"))
            # A repair can pack the textures again without upscaling.
            if self.o.upscale_textures or "hd_pack" in self.o.only_steps:
                steps.append(Step("HD texture pack", self.hd_texture_pack, "hd_pack"))
            steps.append(Step("User texture pack", self.user_texture_pack, "user_pack"))
            steps.append(Step("World at War", self.rip_waw, "waw"))
        # The Windows cross build runs the Linux SDK's code translator, so it
        # needs the SDK and the translated code even without the Linux game.
        if self.native or self.cross_windows:
            steps += [
                Step("ReXGlue SDK", self.sdk, "sdk"),
                Step("Recompiling Call of Duty 3" if self.native else "Translating Call of Duty 3's code",
                     self.recompile, "recompile"),
            ]
        if self.native:
            steps.append(Step("Packaging", self.package, "package"))
            if self.o.trace_build:
                steps.append(Step("Decomp trace version", self.trace_game, "trace"))
        if self.cross_windows:
            steps.append(Step("Windows version (cod3.exe)", self.windows_game, "windows"))
        if self.wsl_linux:
            steps.append(Step("Linux version (AppImage, built in WSL)", self.linux_in_wsl, "linux_wsl"))
        if not self.o.binaries_only:
            steps.append(Step("Finishing", self.finish, "finish"))
        steps.append(Step("Cleaning up", self.cleanup, "cleanup"))
        if self.repairing:
            wanted = self._wanted_steps()
            steps = [s for s in steps if s.key in wanted]
        return steps

    def run(self) -> None:
        # Only a folder the tool can call its own: the cleanup empties it.
        claim_work_dir(self.work_real)
        self.work_real.mkdir(parents=True, exist_ok=True)
        self.work = build_path(self.work_real)
        self.tools_dir = self.work / "tools"
        self.log.file = self.work_real / "setup.log"
        # The setup tool's and the PC's versions first, so a log alone tells
        # what the build ran on (bug reports scrub user and computer names).
        self.log.info("\n".join(pc_check.system_lines()))
        if self.work != self.work_real:
            self.log.info(f"Work folder {self.work_real} has spaces or is long; the build reaches it through "
                          f"the link {self.work}.")
        steps = self.steps()
        t0 = time.time()
        for i, step in enumerate(steps):
            if self.cancel.is_set():
                raise Cancelled()
            self.on_step(i, len(steps), step.title)
            self.log.step(f"[{i + 1}/{len(steps)}] {step.title}")
            step.func()
            if i == 0:
                self._keep_log_in_output()
        mins = (time.time() - t0) / 60
        self.log.step(f"Done in {mins:.0f} min. Everything is in {self.out}")
        if self.log.file:
            self.log.info(f"This log: {self.log.file}")

    def _keep_log_in_output(self) -> None:
        """After the inputs are checked, the log moves to <output>/logs/, so it
        survives the cleanup (the last few are kept)."""
        logs = self.out / LOGS
        try:
            logs.mkdir(parents=True, exist_ok=True)
            target = logs / time.strftime("setup-%Y%m%d-%H%M%S.log")
            if self.log.file and self.log.file.exists():
                shutil.copyfile(self.log.file, target)
                self.log.file.unlink()
            self.log.file = target
            for old in sorted(logs.glob("setup-*.log"))[:-KEEP_LOGS]:
                old.unlink()
        except OSError as e:
            self.log.warn(f"Couldn't keep the log in {logs} ({e}); it stays in the work folder.")

    # ------------------------------------------------------------------
    # 1. Inputs
    # ------------------------------------------------------------------
    def preflight(self) -> None:
        if self.o.build_recomp and not self.template.joinpath("CMakeLists.txt").exists():
            raise SetupError(f"The recomp template is missing ({self.template}). Keep the 'recomp' and 'patches' "
                             "folders next to the setup tool.")
        # The disc is read only when its files are taken out (always, except
        # in a repair that doesn't need them).
        needs_disc = self._runs("assets")
        if needs_disc and (not self.cod3_src or not self.cod3_src.exists()):
            raise SetupError("Choose the Call of Duty 3 .iso (or a folder extracted from it).")
        if not self.o.output_dir:
            raise SetupError("Choose an output folder.")
        if host_triple() == "win-arm64":
            raise SetupError("This Python is the ARM64 one. The game is built for 64-bit x86 (x64), which Windows "
                             "on ARM runs through its emulation: start the setup tool with the x64 Python from "
                             f"{pc_check.PYTHON_ORG} instead (setup.bat installs that one). Not tested on ARM yet.")
        if host_triple() not in ("linux-amd64", "win-amd64"):
            raise SetupError(f"Unsupported system {host_triple()}: the build supports 64-bit x86 Linux and Windows.")
        if IS_WINDOWS and self.native and self._runs("package"):
            # Windows can't replace a program, or a DLL, that is running.
            from .game_settings import game_running
            if game_running(self.out):
                raise SetupError("Call of Duty 3 is running. Close it first: Windows can't replace the game's "
                                 "files while it runs. Then press Build again.")

        # Where things go must not collide with the inputs.
        if self.cod3_src and self.cod3_src.is_dir() and self.cod3_src != self.assets \
                and overlaps(self.cod3_src, self.out) and not is_within(self.cod3_src, self.out):
            raise SetupError("The output folder can't be inside the extracted CoD3 folder.")
        for p in self.protected:
            if overlaps(self.work, p):
                raise SetupError(f"The work folder {self.work} overlaps {p}. Pick a separate work folder "
                                 "(it is deleted at the end).")
        if self.waw.path and self.waw.path.is_dir() and overlaps(self.out, self.waw.path):
            raise SetupError("The output folder and the World at War folder can't contain each other.")

        if needs_disc:
            self._check_cod3_source()

        if self.o.build_recomp and not (self.o.build_linux or self.o.build_windows):
            raise SetupError("Tick at least one version of the game to build (Linux and/or Windows), or untick "
                             "'Also build the playable Call of Duty 3'.")
        if self.o.binaries_only:
            self.out.mkdir(parents=True, exist_ok=True)
            return
        if self.wsl_linux and not self.native:
            # The Linux version is the only one asked for: say now if WSL isn't
            # ready, not after the rips.
            from . import wsl as wsl_mod
            wsl_mod.Wsl(self.runner, self.log, self.cancel).check()

        # World at War, by the source rule (only when it is ripped this time).
        if self._runs("waw"):
            for note in self.waw_notes:
                self.log.info(note)
            ok, msg = waw.check_source(self.waw)
            if not ok:
                raise SetupError(f"World at War ({waw.KIND_LABELS[self.waw.kind]}): {msg}")
            self.log.info(f"World at War source: {self.waw.label}. {msg}")
            if self.waw.kind not in (waw.STEAM, waw.NONE):   # only the discs use them
                for p in self.map_packs:
                    if not p.is_file():
                        raise SetupError(f"Map pack download not found: {p}")

        # Disk space (a repair counts only the steps it runs).
        need_out = (RIPPED_SIZE if self._runs("rip_cod3") else 0) + (BINARIES_SIZE if self.o.build_recomp else 0)
        if self.o.upscale_textures and not self.o.binaries_only and self._runs("upscale"):
            need_out += upscaled_size(upscale.Settings(scale=self.o.upscale_scale).validated().scale)
        if needs_disc and not (self.assets.exists() and (self.assets / "default.xex").exists()):
            need_out += self._cod3_size()
        # One SDK + game build per version made (the Windows cross build and
        # the WSL build each add their own, plus their toolchain / Ubuntu).
        builds = int(self.native or self.cross_windows) + int(self.cross_windows) + int(self.wsl_linux)
        need_work = (WORK_NEEDED_RIPPER if self._runs("ripper") else 0) + WORK_NEEDED_RECOMP * builds
        need_out += BINARIES_SIZE * max(0, int(self.cross_windows) + int(self.wsl_linux))
        if self.native and self.o.trace_build and self._runs("trace"):
            need_work += WORK_NEEDED_TRACE
            need_out += BINARIES_SIZE
        free_out, free_work = free_space(self.out), free_space(self.work)
        self.log.info(f"Output: {self.out}  (needs ~{human(need_out)}, {human(free_out)} free)")
        self.log.info(f"Work:   {self.work}  (needs ~{human(need_work)}, {human(free_work)} free)")
        same_disk = _same_device(self.out, self.work)
        if same_disk and free_out < need_out + need_work:
            raise SetupError(f"Not enough disk space: need ~{human(need_out + need_work)}, "
                             f"{human(free_out)} free.")
        if free_out < need_out:
            raise SetupError(f"Not enough space in the output folder: need ~{human(need_out)}.")
        if free_work < need_work:
            raise SetupError(f"Not enough space in the work folder: need ~{human(need_work)}.")
        if " " in str(self.work):
            self.log.warn("The work folder path contains spaces; some build tools dislike that.")
        self.out.mkdir(parents=True, exist_ok=True)

    def _check_cod3_source(self) -> None:
        """Which CoD3 build the source is, and that it has the files needed."""
        xex_sha1 = self._cod3_xex_sha1()
        name = KNOWN_XEX.get(xex_sha1)
        if name:
            self.log.info(f"Game: {name} (default.xex {xex_sha1[:12]}...)")
            if xex_sha1 in XEX_PICKS_LANGUAGE:
                self.log.info("This disc speaks French, Spanish or Italian, whichever the console language is: "
                              "pick it on the Play page (Game settings > Console language). With English the "
                              "soldiers most likely have no voices, since the disc has no English speech.")
        elif self.o.force_unknown_iso:
            self.log.warn(f"Unknown default.xex ({xex_sha1}); continuing because 'force' is on. "
                          "Expect build errors or crashes.")
        else:
            raise SetupError("This doesn't look like a Call of Duty 3 Xbox 360 disc the recomp knows "
                             f"(default.xex SHA-1 {xex_sha1}). Its code hooks match these releases: "
                             + ", ".join(KNOWN_XEX.values()) + ".")
        # The recomp needs the executables; ripping needs the sp/ archives.
        missing = self._cod3_missing_files() if self.o.build_recomp else []
        if missing:
            raise SetupError("Game files missing from the CoD3 source: " + ", ".join(missing))

    def _cod3_xex_sha1(self) -> str:
        assert self.cod3_src
        if self.cod3_src.is_dir():
            xex = self.cod3_src / "default.xex"
            if not xex.exists():
                raise SetupError(f"No default.xex in {self.cod3_src}.")
            return sha1_file(xex)
        with XisoImage(self.cod3_src) as img:
            return hashlib.sha1(img.read("default.xex")).hexdigest()

    def _cod3_missing_files(self) -> List[str]:
        assert self.cod3_src
        if self.cod3_src.is_dir():
            return [f for f in RECOMP_FILES if not (self.cod3_src / f).exists()]
        with XisoImage(self.cod3_src) as img:
            return [f for f in RECOMP_FILES if not img.find(f)]

    def _cod3_size(self) -> int:
        assert self.cod3_src
        if self.cod3_src.is_dir():
            return dir_size(self.cod3_src)
        with XisoImage(self.cod3_src) as img:
            return img.total_size

    # ------------------------------------------------------------------
    # 2. Tools
    # ------------------------------------------------------------------
    def install_plan(self, recomp: bool) -> List[Install]:
        """Everything this build will download or install, with where it goes,
        for the one approval at the start (owner, 2026-10-10: "things will
        require approval before downloading", "the user knows where everything
        is installed")."""
        tools_shown = self.work_real / "tools"
        tc = deps.detect(self.tools_dir, self.runner)
        items = deps.plan(tc, self.tools_dir, self.o.install_deps, recomp=recomp,
                          windows_exe=self.cross_windows, rust=self._needs_rust, shown_dir=tools_shown)

        def want(key: str) -> None:
            if not deps._portable_ok(self.tools_dir, key):
                items.append(deps.tool_install(key, tools_shown))
        if self._needs_rust and rippers.needs_build(self.tool_root, self.runner):
            local_rust = deps.rust_dir(self.tools_dir).exists() or \
                any(i.key == "rust" and i.where == str(deps.rust_dir(tools_shown)) for i in items)
            items.append(rippers.crates_install(deps.rust_dir(tools_shown) / "cargo" if local_rust else None))
        if (self.native or self.cross_windows) and not (self.sdk_src / ".git").exists():
            items.append(self._sdk_install())
        if self.native and self._fidelityfx and self._runs("sdk") and not self._sdk_built(tc.cxx) \
                and not self._fidelityfx_fetched():
            items.append(self._fidelityfx_install())
        if self.cross_windows:
            want("llvm-mingw-linux")
        if self.native and IS_LINUX and self.o.appimage:
            want("appimagetool")
        if self.o.upscale_textures and not self.o.binaries_only and self._runs("upscale"):
            want(upscale.portable_key())
        if self.wsl_linux:
            items.append(deps.wsl_install(self.work_real))
        return items

    def _sdk_install(self) -> Install:
        return Install("git:rexglue-sdk", "ReXGlue SDK source at a pinned commit, with its submodules (git)",
                       str(self.work_real / "rexglue-sdk"), "about 1.5 GB", "github.com")

    def _fidelityfx_install(self) -> Install:
        """AMD's FidelityFX SDK, which the SDK's own CMake setup downloads
        (git, FetchContent) when FSR / CAS scaling is on (Windows)."""
        where = self.work_real / "rexglue-sdk" / "out" / "build" / self.sdk_preset / "_deps"
        return Install("git:fidelityfx", "AMD FidelityFX SDK source (FSR / CAS scaling in the game), at the "
                       "commit the ReXGlue SDK pins; downloaded by the SDK's CMake setup. Optional: untick AMD "
                       "FidelityFX under \"Show more options\" to leave it out", str(where),
                       "several hundred MB, up to about 1 GB", "github.com")

    def _fidelityfx_fetched(self) -> bool:
        return (self.sdk_src / "out" / "build" / self.sdk_preset / "_deps" / "fidelityfx-src" / ".git").exists()

    def dependencies(self) -> None:
        # Building only in WSL needs nothing on Windows but the ripper's tools.
        recomp = self.native or self.cross_windows
        if self.o.install_deps:
            APPROVAL.require(self.install_plan(recomp), self.log)
        self.tc = deps.ensure(self.tools_dir, self.runner, self.log, self.cancel, self.o.install_deps,
                              recomp=recomp, windows_exe=self.cross_windows,
                              rust=self._needs_rust)
        problems = self.tc.problems(recomp=recomp)
        if not self._needs_rust:   # no ripper to build here, so no Rust
            problems = [p for p in problems if p not in self.tc.rust_problems()]
        if IS_WINDOWS:
            # What about this PC can make a Windows build fail (OneDrive, the
            # Store's Python, a proxy ...), in the log for anyone reading it.
            for note in pc_check.system_notes(self.work_real, self.out):
                (self.log.info if note.level == pc_check.INFO else self.log.warn)(note.text())
        if problems and self.o.skip_dep_check:
            self.log.warn("Continuing despite: " + "; ".join(problems))
        elif problems:
            how = ("Turn on 'Install missing tools' (or drop --no-install-deps) and run again, or install them "
                   "yourself." if not self.o.install_deps else
                   "Install them with your package manager and run the build again." if IS_LINUX else
                   "If an installer just finished, close this tool and start it again.")
            fixes = [n.text() for n in pc_check.tool_notes(self.tc, recomp=recomp, rust=self._needs_rust)] \
                if IS_WINDOWS else []
            raise SetupError("Still missing:\n  - " + "\n  - ".join(problems) + "\n" + how +
                             ("\n\n" + "\n\n".join(fixes) if fixes else ""))

    # ------------------------------------------------------------------
    # 3. Game files
    # ------------------------------------------------------------------
    def cod3_assets(self) -> None:
        assert self.cod3_src
        if self.cod3_src.is_dir():
            if resolved(self.cod3_src) == resolved(self.assets):
                self.log.info("Game files are already in the output folder.")
                return
            self.log.info(f"Copying the extracted game files from {self.cod3_src}")
            copy_tree(self.cod3_src, self.assets, self.log, self.cancel, "assets", hardlink=True)
            return
        self.log.info(f"Extracting {self.cod3_src.name} (read-only) into {self.assets}")
        with XisoImage(self.cod3_src) as img:
            img.extract(self.assets, self.log, self.cancel)

    # ------------------------------------------------------------------
    # 4. The ripper and the rips
    # ------------------------------------------------------------------
    @property
    def _needs_rust(self) -> bool:
        """Rust is needed to build the ripper: in every full build (unless
        only the game is made), not in a repair that doesn't rip or can use
        the ripper already in the output folder."""
        if IS_WINDOWS and self._prebuilt_rip_works():
            return False   # a release download's engine/bin/win-amd64/rip.exe: no Rust needed at all
        return not self.o.binaries_only and self._runs("ripper") and not self._reusable_rip()

    def _prebuilt_rip_works(self) -> bool:
        """The prebuilt ripper a release download ships runs and writes the
        current output layout (checked once)."""
        if self._prebuilt_ok is None:
            self._prebuilt_ok = not rippers.needs_build(self.tool_root, self.runner)
        return self._prebuilt_ok

    def _reusable_rip(self) -> Optional[Path]:
        """In a repair: the ripper an earlier build copied into <output>/tools,
        if it still runs and writes the current output layout (so it isn't
        built again). None in a normal build."""
        if not self.repairing:
            return None
        if self._reusable is None:
            rip = self.out / "tools" / exe_name("rip")
            self._reusable = (rip if rip.is_file() and rippers.works(rip, self.runner) else False,)
        return self._reusable[0] or None

    def ripper(self) -> None:
        reuse = self._reusable_rip()
        if reuse:
            self.log.info(f"Using the ripper in the output folder: {reuse}")
            self.rip = reuse
            return
        assert self.tc
        rip = rippers.get_rip(self.tool_root, self.work / "rust-target", self.tc.cargo, self.o.jobs,
                              self.runner, self.log, env=self.tc.env)
        self.rip = rippers.install_copy(rip, self.out)
        if self.cross_windows:
            # Optional: a failure here is reported but doesn't stop the build.
            try:
                exe = rippers.build_windows_rip(self.tool_root, self.work / "rust-target", self.tc.cargo,
                                                self.tc.mingw, self.o.jobs, self.runner, self.log, env=self.tc.env)
                self.log.info(f"Windows ripper: {rippers.install_copy(exe, self.out)}")
            except SetupError as e:
                self.log.warn(f"The Windows ripper wasn't made: {e}")

    def rip_cod3(self) -> None:
        assert self.rip
        target = self.out / COD3_DATA
        self.log.info(f"Ripping textures, meshes and materials into {target}...")
        rippers.rip_cod3(self.rip, self.assets / "sp", target, self.o.jobs, self.runner, self.log)
        # A pack from an earlier build may give textures the wrong picture;
        # repaired here so it's safe even if this build doesn't pack again.
        upscale.repair_old_pack(target / upscale.PACK_NAME, self.log)

    def upscale_textures(self) -> None:
        """Optional, so a failure is reported and the build goes on; only
        Cancel stops it."""
        textures = self.out / COD3_DATA / "textures"
        if not textures.is_dir():
            self.log.warn(f"No ripped textures in {textures}; nothing to upscale.")
            return
        settings = upscale.Settings(self.o.upscale_model, self.o.upscale_noise, self.o.upscale_scale)
        try:
            out = upscale.output_dir(textures.parent, self.log)
            tool = deps.fetch_portable(upscale.portable_key(), self.tools_dir, self.runner, self.log, self.cancel)
            done, missing = upscale.upscale_folder(tool, textures, out, self.work / "upscale", settings,
                                                   self.runner, self.log, self.cancel)
        except (SetupError, OSError) as e:
            self.log.warn(f"The textures weren't upscaled: {e}\nwaifu2x needs a graphics card with a Vulkan "
                          "driver. The game and the original textures are not affected.")
            return
        if missing:
            self.log.warn(f"{missing} textures couldn't be upscaled (waifu2x's messages are above); the "
                          "originals are used for those.")
        self.log.info(f"Upscaled {done} textures into {out}.")

    def hd_texture_pack(self) -> None:
        """The upscaled copies packed for the playable game: data/cod3/
        hd_textures, the same compressed formats as the game's own textures
        at the copies' size, whatever the scale (`rip cod3-hd-pack`;
        c3w/upscale.py). Optional like
        the upscaling: a failure is reported, the build goes on and the game
        uses its own textures. Runs even when this build's upscaling failed,
        so copies from an earlier build still get packed."""
        assert self.rip
        textures = self.out / COD3_DATA / "textures"
        upscaled = upscale.output_dir(textures.parent, self.log)
        pack = textures.parent / upscale.PACK_NAME
        if not upscaled.is_dir() or not any(upscaled.glob("*.png")):
            self.log.warn(f"No upscaled textures in {upscaled}, so there is no HD texture pack for the game; "
                          "it uses its own textures.")
            return
        if not (textures / upscale.KEYS_FILE).is_file():
            # Only a ripper older than output layout 2 leaves it out.
            self.log.warn(f"The rip in {textures} has no {upscale.KEYS_FILE}, which says which game texture "
                          "each file is, so the HD textures can't be packed for the game. Build again to rip "
                          "with the current ripper.")
            return
        self.log.info(f"Packing the upscaled textures for the game into {pack} (at their upscaled size, in the "
                      "same compressed formats as the game's own)...")
        try:
            rc = rippers.hd_pack(self.rip, textures, upscaled, pack, self.o.jobs, self.runner, self.log)
            failure = f"the ripper stopped with exit code {rc} (its messages are above)" if rc != 0 else ""
        except (SetupError, OSError) as e:
            failure = str(e)
        if not failure and not upscale.pack_ready(pack):
            failure = f"the ripper finished without writing {upscale.PACK_INDEX}"
        if failure:
            earlier = (f" The pack from an earlier build in {pack} stays in use." if upscale.pack_ready(pack)
                       else " The game uses its own textures.")
            self.log.warn(f"The HD texture pack wasn't made: {failure}.{earlier}")
            return
        self.log.info(f"HD texture pack ready: {upscale.pack_size(pack)} textures in {pack}. The game uses them "
                      "while F2 > Video > \"HD textures (waifu2x)\" is on (the default).")

    def user_texture_pack(self) -> None:
        """The player's own pictures (data/cod3/user_textures_png, for example
        for the insignia in lists/insignia_textures.txt) packed for the game
        into data/cod3/user_textures (c3w/user_textures.py, `rip
        cod3-user-pack`). Local work, nothing downloaded. Optional: a failure
        is reported and the build goes on; the game then uses the pack from an
        earlier build, if any, or its own textures."""
        assert self.rip
        data = self.out / COD3_DATA
        folder = user_textures.prepare_folder(data, self.tool_root)
        if not user_textures.user_pictures(data) and not user_textures.pack_keys(data):
            self.log.info(f"No pictures of your own in {folder} (optional: to replace the insignia or other "
                          "textures, see docs/TEXTURE_REPLACEMENT.md).")
            return
        try:
            user_textures.build(self.rip, data, self.tool_root, self.o.jobs, self.runner, self.log)
        except (SetupError, OSError) as e:
            self.log.warn(f"The user texture pack wasn't made: {e}")

    def rip_waw(self) -> None:
        assert self.rip and self.tc
        if self.waw.kind == waw.NONE or not self.waw.path:
            # Left out (or not found): an earlier World at War rip in the output
            # stays as it is. (Before 2026-10-10 it was removed here first.)
            self.log.info("World at War isn't used this time; skipping. "
                          "An earlier World at War rip in the output folder is kept.")
            return
        data = self.out / waw.OUTPUT_DATA
        # The new source is unpacked and checked first; only then does the new
        # rip replace the last one (which may come from another source), so a
        # source that fails or a Cancel leaves the earlier rip in place.
        # Added custom maps always stay.
        folder = waw.prepare(self.waw, self.work, self.tc.sevenzip, self.tc.unshield, self.runner, self.log,
                             self.cancel)
        if not folder:
            self.log.info("No World at War source chosen; skipping.")
            return
        files = waw.fastfiles(folder)
        if not files:
            raise SetupError(f"No fastfiles (zone/*.ff) found in {folder}.")
        self.runner.check_cancel()
        if data.is_dir():
            for old in data.iterdir():
                if old.is_dir() and old.name != waw.CUSTOM_MAPS.name:
                    safe_rmtree(old, data, [p for p in self.protected if p != self.out], self.log)
        files += self._map_pack_fastfiles(files)
        self.log.info(f"Ripping {len(files)} World at War fastfiles from the {waw.KIND_LABELS[self.waw.kind]}"
                      f"{' and map packs' if self.map_packs and self.waw.kind != waw.STEAM else ''}...")
        ok, errors = rippers.rip_fastfiles(self.rip, files, data, self.runner, self.log)
        self.waw_ripped = ok
        if not ok:   # World at War is optional: say so, but build Call of Duty 3 anyway
            self.log.warn(f"None of the {len(files)} World at War fastfiles could be ripped; continuing "
                          "without World at War.")
        for e in errors[:20]:
            self.log.warn(e)
        if errors:
            self.log.warn(f"{len(errors)} of {len(files)} fastfiles couldn't be ripped (listed above).")

    def _map_pack_fastfiles(self, source_files: List[Path]) -> List[Path]:
        """The map pack fastfiles the source lacks, taken out of the patch
        downloads into data/waw/map_packs/ (kept: their .iwd files hold the
        maps' textures and sounds)."""
        if not self.map_packs:
            return []
        if self.waw.kind == waw.STEAM:
            self.log.info("The Steam version already has every map pack, so the map pack downloads aren't used.")
            return []
        assert self.tc
        unpackers = map_packs.Unpackers(self.tc.sevenzip, self.tc.unshield, self.tc.innoextract)
        dest = self.out / map_packs.OUTPUT
        extra: List[Path] = []
        for patch in self.map_packs:
            self.log.info(f"Taking the map packs out of {patch.name} (its installer is not run)...")
            extra += map_packs.extract(patch, dest, self.work, unpackers, self.runner, self.log, self.cancel)
        new, already = map_packs.missing_from(source_files, sorted(set(extra)))
        if already:
            self.log.info(f"Already in the {waw.KIND_LABELS[self.waw.kind]}, so not taken from the patches: "
                          + ", ".join(already))
        self.log.info(f"Map pack fastfiles added: {', '.join(p.stem for p in new) or 'none'}")
        return new

    # ------------------------------------------------------------------
    # 4. SDK
    # ------------------------------------------------------------------
    @property
    def sdk_src(self) -> Path:
        return self.work / "rexglue-sdk"

    @property
    def sdk_preset(self) -> str:
        return "win-amd64" if IS_WINDOWS else "linux-amd64"

    @property
    def sdk_install(self) -> Path:
        return self.sdk_src / "out" / "install" / self.sdk_preset

    def _git(self, *args: str, cwd: Optional[Path] = None, check: bool = True) -> int:
        assert self.tc and self.tc.git
        return self.runner.run([self.tc.git, *self._git_config(), *args], cwd=cwd or self.sdk_src,
                               env=self.tc.env, check=check)

    def _clean_checkout(self) -> None:
        """The pinned commit with nothing else on it, so exactly the current
        patches go in: an edited or removed patch, or the Windows-only ones a
        cross build added, don't carry over. Build and install folders (out/)
        and the submodules are kept."""
        self._git("checkout", "-q", "--force", SDK_COMMIT)
        self._git("clean", "-fdq", "-e", "out/")

    def _apply_patches(self, patches: List[Path]) -> None:
        for p in patches:
            p = lf_patch(p, self.work / "patches-lf")
            if self._git("apply", "--check", str(p), check=False) == 0:
                self.log.info(f"Applying {p.name}")
                self._git("apply", str(p))
            elif self._git("apply", "--reverse", "--check", str(p), check=False) == 0:
                self.log.info(f"{p.name} already applied")
            else:
                raise SetupError(f"Patch {p.name} doesn't apply to the SDK checkout. Delete {self.sdk_src} "
                                 "and run again.")
        # The precompiled Vulkan resolve shaders, for the 4096-tile EDRAM of
        # patch 0010 (edited word by word; see c3w/sdk_shaders.py).
        try:
            n = sdk_shaders.edit_sdk(self.sdk_src)
        except sdk_shaders.ShaderEditError as e:
            raise SetupError(f"Couldn't adapt the SDK's resolve shaders to the larger EDRAM: {e}") from None
        if n:
            self.log.info(f"Adapted {n} Vulkan resolve shaders to the 4096-tile EDRAM")

    # What git prints about a submodule it couldn't set up; the quoted part is
    # the submodule's path (relative, or absolute for "clone of ... into").
    _SUBMODULE_ERROR = re.compile(r"(?:submodule(?: path)?|[Ff]ailed to clone) '([^']+)'")

    def _fetch_submodules(self) -> None:
        """git submodule update, with repair. A clone that was cut off (network
        drop, antivirus, Windows' path length limit, the tool closed midway)
        leaves a broken copy in .git/modules, and from then on git only says
        "could not get a repository handle for submodule". Such copies are
        removed (they hold nothing but downloaded SDK sources, inside the work
        folder) and the download is tried again: shallow, shallow after the
        repair, then with full history."""
        self.log.info("Downloading SDK dependencies (git submodules)...")
        self._git("submodule", "sync", "--recursive", check=False)
        attempts = [("--depth", "1"), ("--depth", "1"), ()]
        for n, depth in enumerate(attempts):
            failed: List[str] = []

            def watch(line: str) -> None:
                failed.extend(self._SUBMODULE_ERROR.findall(line))

            last = n == len(attempts) - 1
            rc = self.runner.run([self.tc.git, *self._git_config(), "submodule", "update", "--init",
                                  "--recursive", "--jobs", "4", *depth],
                                 cwd=self.sdk_src, env=self.tc.env, check=last, line_filter=watch)
            if rc == 0:
                return
            repaired = self._repair_submodules(failed)
            if n == 0:
                self.log.warn("Submodule download failed" + (f" ({', '.join(repaired)} repaired)" if repaired
                                                             else "") + "; trying again.")
            else:
                if not repaired:
                    # Nothing named in the errors: start every submodule over.
                    self._git("submodule", "deinit", "--all", "--force", check=False)
                    safe_rmtree(self.sdk_src / ".git" / "modules", self.sdk_src, self.protected, self.log)
                self.log.warn("Submodule download failed again; retrying with full history.")

    def _repair_submodules(self, paths: List[str]) -> List[str]:
        """Removes the broken copies of the named submodules (git's own clone in
        .git/modules and the checkout). Returns the paths it repaired."""
        repaired = []
        root = self.sdk_src.resolve()
        for raw in dict.fromkeys(paths):
            p = Path(raw)
            p = (p if p.is_absolute() else root / p).resolve()
            if p == root or not is_within(p, root):
                continue
            rel = p.relative_to(root)
            for target in (root / ".git" / "modules" / rel, p):
                safe_rmtree(target, root, self.protected, self.log)
            repaired.append(rel.as_posix())
        return repaired

    def _git_config(self) -> List[str]:
        return ["-c", "core.longpaths=true"] if IS_WINDOWS else []

    @property
    def _fidelityfx(self) -> bool:
        return self.o.fidelityfx and fidelityfx_available()

    def _sdk_stamp_data(self, cxx: Optional[Path]) -> dict:
        """What the built SDK depends on; it is built again when any of it
        changes (sdk.stamp.json in the work folder)."""
        patches = sorted(self.patches.glob("*.patch"))
        return {"commit": SDK_COMMIT, "fidelityfx": self._fidelityfx, "compiler": str(cxx),
                "patches": [hashlib.sha1(p.read_bytes()).hexdigest() for p in patches],
                "shaders": sdk_shaders.version(), "backends": "d3d12+vulkan" if IS_WINDOWS else "vulkan"}

    def _sdk_built(self, cxx: Optional[Path]) -> bool:
        return read_json(self.work / "sdk.stamp.json") == self._sdk_stamp_data(cxx) and \
            (self.sdk_install / "lib" / "cmake" / "rexglue").exists()

    def sdk(self) -> None:
        assert self.tc
        patches = sorted(self.patches.glob("*.patch"))
        stamp_data = self._sdk_stamp_data(self.tc.cxx)
        stamp = self.work / "sdk.stamp.json"
        if self._sdk_built(self.tc.cxx):
            self.log.info("ReXGlue SDK already built.")
            return

        # Fetch the pinned commit and its submodules.
        if not (self.sdk_src / ".git").exists():
            self.sdk_src.mkdir(parents=True, exist_ok=True)
            self._git("init", "-q")
            self._git("remote", "add", "origin", SDK_REPO)
        head = self.runner.capture([self.tc.git, "rev-parse", "HEAD"], cwd=self.sdk_src)[1].strip()
        if head != SDK_COMMIT:
            APPROVAL.require([self._sdk_install()], self.log)
            self.log.info(f"Downloading the ReXGlue SDK source into {self.work_real / 'rexglue-sdk'}...")
            if self._git("fetch", "--depth", "1", "origin", SDK_COMMIT, check=False) != 0:
                self._git("fetch", "origin")
        self._clean_checkout()
        self._fetch_submodules()

        self._apply_patches(patches)

        fidelityfx = self._fidelityfx
        if self.o.fidelityfx and not fidelityfx:
            self.log.info("AMD FidelityFX (FSR / CAS scaling) is left out on this system: its build runs AMD's "
                          "Windows-only shader compiler, which would need Wine. The game uses its standard "
                          "scaling instead.")
        if fidelityfx and not self._fidelityfx_fetched():
            # The SDK's CMake setup downloads it (git) before anything else.
            try:
                APPROVAL.require([self._fidelityfx_install()], self.log)
                self.log.info("The SDK's setup first downloads AMD's FidelityFX SDK with git (no progress is shown; "
                              "it can take several minutes).")
            except SetupError:
                self.log.warn("AMD FidelityFX wasn't approved for download, so the SDK is built without it (no "
                              "FSR / CAS scaling in the game).")
                fidelityfx = False
        failed: List[str] = []   # ninja's "FAILED: <file>" lines
        try:
            self._build_sdk(fidelityfx, failed)
        except SetupError:
            # Only a failure in FidelityFX's own part is worth a second try
            # without it; any other file would fail the same way again.
            if not fidelityfx or (failed and not any(re.search(r"fidelityfx|\bffx", f, re.I) for f in failed)):
                raise
            self.log.warn("SDK build with AMD FidelityFX failed; retrying without it (no FSR/CAS in game).")
            safe_rmtree(self.sdk_src / "out" / "build" / self.sdk_preset, self.work, self.protected, self.log)
            self._build_sdk(False)
        write_json(stamp, stamp_data)

    def _forget_other_compiler(self, build_dir: Path) -> None:
        """A build folder configured with another compiler (say clang 18
        before the switch to 20) can't be reused: CMake refuses to change
        compilers in place. Start that folder over."""
        assert self.tc
        cache = build_dir / "CMakeCache.txt"
        try:
            text = cache.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return
        m = re.search(r"^CMAKE_CXX_COMPILER:\w+=(.*)$", text, re.MULTILINE)
        if m and resolved(Path(m.group(1).strip())) != resolved(Path(str(self.tc.cxx))):
            self.log.info(f"{build_dir.name}: configured with {m.group(1).strip()}, now using {self.tc.cxx}; "
                          "starting that build folder over.")
            safe_rmtree(build_dir, self.work, self.protected, self.log)

    def _build_sdk(self, fidelityfx: bool, failed: Optional[List[str]] = None) -> None:
        """Configure and build the SDK; ninja's FAILED lines go into failed."""
        assert self.tc
        build_dir = self.sdk_src / "out" / "build" / self.sdk_preset
        self._forget_other_compiler(build_dir)
        self.log.info(f"Configuring the SDK (FidelityFX {'on' if fidelityfx else 'off'})...")
        self.runner.run([self.tc.cmake, "--preset", self.sdk_preset,
                         f"-DCMAKE_C_COMPILER={self.tc.cc}", f"-DCMAKE_CXX_COMPILER={self.tc.cxx}",
                         f"-DCMAKE_MAKE_PROGRAM={self.tc.ninja}",
                         "-DREXGLUE_BUILD_TESTS=OFF", "-DBUILD_TESTING=OFF",
                         f"-DREXGLUE_ENABLE_FIDELITYFX={'ON' if fidelityfx else 'OFF'}",
                         # Windows: Vulkan next to Direct3D 12, chosen in F2 (Linux is Vulkan only).
                         *(["-DREXGLUE_USE_VULKAN=ON"] if self.sdk_preset.startswith("win") else [])],
                        cwd=self.sdk_src, env=self.tc.env)
        self.log.info(f"Building the SDK with {self.o.jobs} threads (this takes a while)...")
        self.runner.run([self.tc.cmake, "--build", build_dir, "--config", "Release", "--target", "install",
                         "--parallel", str(self.o.jobs)],
                        cwd=self.sdk_src, env=self.tc.env, quiet=True,
                        line_filter=self._note_failed(self._ninja_progress("SDK"), failed))

    @staticmethod
    def _note_failed(handle: Callable[[str], None], failed: Optional[List[str]]) -> Callable[[str], None]:
        """handle, also keeping ninja's "FAILED: <file>" lines in failed."""
        if failed is None:
            return handle

        def note(line: str) -> None:
            if line.startswith("FAILED:"):
                failed.append(line)
            handle(line)
        return note

    def _relay_inner_log(self, label: str) -> Callable[[str], None]:
        """Output of this tool running elsewhere (inside WSL): its progress line
        (redrawn with \\r) drives the progress bar, the rest goes to the log."""
        pat = re.compile(r"^\s*(\d+(?:\.\d+)?)% (.*)$")

        def handle(line: str) -> None:
            parts = [p for p in line.split("\r") if p.strip()]
            if not parts:
                return
            m = pat.match(parts[-1])
            if m:
                self.log.progress(float(m.group(1)) / 100, f"{label}: {m.group(2).strip()[:90]}")
            elif len(parts) == 1:
                self.log.output(parts[0])
        return handle

    def _ninja_progress(self, label: str) -> Callable[[str], None]:
        """Turn ninja's "[n/total] ..." lines into the progress bar; anything
        else (compiler diagnostics, failures) goes to the log."""
        pat = re.compile(r"^\[(\d+)/(\d+)\]\s*(.*)")

        def handle(line: str) -> None:
            m = pat.match(line)
            if m:
                done, total = int(m.group(1)), int(m.group(2))
                self.log.progress(done / max(total, 1), f"{label} {done}/{total}  {m.group(3)[-90:]}")
            elif line.strip():
                self.log.output(line)
        return handle

    # ------------------------------------------------------------------
    # 5. Recompile
    # ------------------------------------------------------------------
    @property
    def proj(self) -> Path:
        return self.work / "cod3"

    @property
    def proj_build(self) -> Path:
        return self.proj / "out" / "build" / "release"

    @property
    def trace_build_dir(self) -> Path:
        """The decomp trace version's own build folder (same project and translated code)."""
        return self.proj / "out" / "build" / TRACE_BUILD_DIR

    def _game_configure(self, build: Path, *extra: str) -> list:
        """The CMake configure command of the game in `build` (as recompile's)."""
        assert self.tc
        return [self.tc.cmake, "-S", self.proj, "-B", build, "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={self.tc.cc}",
                f"-DCMAKE_CXX_COMPILER={self.tc.cxx}", f"-DCMAKE_MAKE_PROGRAM={self.tc.ninja}",
                f"-DCMAKE_PREFIX_PATH={self.sdk_install}", *extra]

    def recompile(self) -> None:
        assert self.tc
        # Template -> work tree. Only changed files are rewritten so ninja
        # rebuilds incrementally on a re-run.
        for src in self.template.rglob("*"):
            if src.is_dir() or "__pycache__" in src.parts:
                continue
            dst = self.proj / src.relative_to(self.template)
            if not dst.exists() or dst.read_bytes() != src.read_bytes():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dst)
        # The recompiler only needs the executables, not the 6 GB of data.
        for rel in RECOMP_FILES:
            s, d = self.assets / rel, self.proj / "assets" / rel
            if not d.exists() or d.stat().st_size != s.stat().st_size:
                d.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(s, d)
        build_mp = self._copy_mp_xex()

        self._forget_other_compiler(self.proj_build)
        configure = [self.tc.cmake, "-S", self.proj, "-B", self.proj_build, "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={self.tc.cc}",
                     f"-DCMAKE_CXX_COMPILER={self.tc.cxx}", f"-DCMAKE_MAKE_PROGRAM={self.tc.ninja}",
                     f"-DCMAKE_PREFIX_PATH={self.sdk_install}"]
        self.log.info("Configuring the CoD3 project...")
        self.runner.run(configure, cwd=self.proj, env=self.tc.env)
        # Always run the translator target and configure again: both do
        # nothing when everything is current, and a translation that was cut
        # off (Cancel, a crash) would otherwise leave the project configured
        # without its 15 level libraries.
        # The codegen skips modules whose inputs (the disc's code, config/)
        # haven't changed, but an SDK patch can change what it writes (0027
        # did: every module's _pch.h). So after the SDK changed, every module
        # is translated again; files that come out the same keep their dates,
        # so only what really changed is compiled again.
        sdk_key = read_json(self.work / "sdk.stamp.json")
        if codegen_sdk_changed(self.proj, sdk_key):
            if forget_codegen(self.proj):
                self.log.info("The SDK changed since the code was last translated, so all of it is translated "
                              "again (a few minutes).")
        self.log.info("Translating the Xbox 360 PowerPC code to C++ (rexglue codegen; quick when up to date)...")
        self.runner.run([self.tc.cmake, "--build", self.proj_build, "--target", "cod3_codegen"],
                        cwd=self.proj, env=self.tc.env)
        write_json(self.proj / "generated" / CODEGEN_SDK_STAMP, sdk_key)
        if build_mp:
            # The multiplayer program has its own codegen (recomp/mp/).
            mp_proj = self.proj / "mp"
            if codegen_sdk_changed(mp_proj, sdk_key):
                forget_codegen(mp_proj)
            self.log.info("Translating the multiplayer program (codmp_xenonf.xex) to C++...")
            self.runner.run([self.tc.cmake, "--build", self.proj_build, "--target", "cod3mp_codegen"],
                            cwd=self.proj, env=self.tc.env)
            write_json(mp_proj / "generated" / CODEGEN_SDK_STAMP, sdk_key)
        # Pick up the generated source lists and the 15 level DLL targets.
        self.runner.run(configure, cwd=self.proj, env=self.tc.env)
        if not self.native:
            self.log.info("Translated; this system's own version isn't wanted, so it isn't compiled.")
            return
        self.log.info(f"Compiling the recompiled game with {self.o.jobs} threads. This is the long part "
                      "(roughly 20-90 minutes depending on the CPU).")
        self.runner.run([self.tc.cmake, "--build", self.proj_build, "--parallel", str(self.o.jobs)],
                        cwd=self.proj, env=self.tc.env, quiet=True, line_filter=self._ninja_progress("CoD3"))
        if not (self.proj_build / exe_name("cod3")).exists():
            raise SetupError("The build finished but cod3 was not produced; see the log.")
        if build_mp and not (self.proj_build / exe_name("cod3mp")).exists():
            raise SetupError("The build finished but cod3mp (multiplayer) was not produced; see the log.")
        missing = missing_levels(self.proj_build)
        if missing:
            raise SetupError("The build finished without the level libraries for: " + ", ".join(missing) +
                             f". Delete {self.proj / 'generated'} and build again.")

    def _copy_mp_xex(self) -> bool:
        """Puts the multiplayer program's executable into the project when it
        is the known build, so the project builds cod3mp next to cod3
        (recomp/CMakeLists.txt adds recomp/mp/ when assets/codmp_xenonf.xex is
        there). Returns whether cod3mp will be built."""
        s, d = self.assets / MP_XEX, self.proj / "assets" / MP_XEX
        if not s.is_file():
            self.log.warn(f"No {MP_XEX} in the game files: building the campaign only (no multiplayer).")
            d.unlink(missing_ok=True)
            return False
        sha = sha1_file(s)
        if sha not in KNOWN_MP_XEX:
            self.log.warn(f"{MP_XEX} isn't the known build (SHA-1 {sha}); building the campaign only "
                          "(the multiplayer program's hooks are tied to that build).")
            d.unlink(missing_ok=True)
            return False
        if not d.exists() or d.stat().st_size != s.stat().st_size:
            d.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(s, d)
        return True

    # ------------------------------------------------------------------
    # 6. Package
    # ------------------------------------------------------------------
    def _binaries(self, build: Optional[Path] = None) -> List[Path]:
        """The game's files: the executable, the libraries built next to it, and
        anything else they need from the SDK. Stops if a library is missing
        from this computer, since the game couldn't start without it.
        `build`: another build folder of the game (the decomp trace version)."""
        b = build or self.proj_build
        if IS_WINDOWS:
            start = [b / "cod3.exe"] + sorted(b.glob("*.dll"))
            is_system = binaries.windows_is_system_dll
        else:
            start = [b / "cod3"] + sorted(b.glob("*.so"))
            system = binaries.linux_system_libraries()
            is_system = system.__contains__
        # The multiplayer program, when it was built (recompile).
        mp = b / exe_name("cod3mp")
        if mp.exists():
            start.insert(1, mp)
        search = [b, self.sdk_install / "lib", self.sdk_install / "bin"]
        if IS_WINDOWS:
            search += binaries.vc_runtime_dirs(self.tc.vs_path if self.tc else None)
        start = self._with_gpu_plugin(start, search, IS_WINDOWS)
        files, missing = binaries.gather(start, search, is_system)
        if IS_WINDOWS and missing:
            # A Visual Studio without its "C++ Redistributable Update" part has
            # no copy of the C++ runtime to ship; Windows' own copies are the same files.
            extra, missing = binaries.windows_runtime_from_system(missing)
            files += extra
        added = [f.name for f in files[len(start):]]
        if added:
            self.log.info("Also packing, needed by the game: " + ", ".join(added))
        if missing:
            raise SetupError("The game needs libraries that aren't on this computer or in the build:\n" +
                             "\n".join(f"  {lib} (needed by {by})" for by, lib in missing) +
                             "\nInstall them (or run the build with 'Install missing tools' on) and build again.")
        return files

    def _with_gpu_plugin(self, start: List[Path], search: List[Path], windows: bool) -> List[Path]:
        """`start` plus the SDK's GPU plugin. The game loads it by name at
        startup, so no import table names it: the build copies it next to the
        executable, and if that copy is missing (an SDK-only rebuild) it is
        taken from the SDK's install folder. Without it the game stops at
        startup ("Failed to load GPU plugin")."""
        plugin = binaries.find_gpu_plugin(binaries.GPU_PLUGIN, search, windows)
        if not plugin:
            names = " or ".join(binaries.gpu_plugin_names(binaries.GPU_PLUGIN, windows))
            raise SetupError(f"The GPU plugin ({names}) is in neither the build nor the SDK's install folder "
                             f"({', '.join(str(s) for s in search)}); the game can't start without it.")
        if plugin.name in {s.name for s in start}:
            return start
        return start[:1] + [plugin] + start[1:]

    def package(self) -> None:
        bins = self._binaries()
        if IS_LINUX and self.o.appimage and self._appimage(bins):
            return
        bindir = self.out / "bin"
        bindir.mkdir(parents=True, exist_ok=True)
        for f in bins:
            try:
                shutil.copy2(f, bindir / f.name)
            except OSError as e:
                if not IS_WINDOWS:
                    raise
                raise SetupError(f"Couldn't replace {bindir / f.name} ({e}). Windows keeps a running program's "
                                 "files locked: close Call of Duty 3 (and anything else using that folder, an "
                                 "antivirus scan or OneDrive included), then press Build again.") from None
        self.exe_rel = f"bin/{exe_name('cod3')}"
        self.packed["windows" if IS_WINDOWS else "linux"] = [f"bin/{f.name}" for f in bins]
        mp = (bindir / exe_name("cod3mp")).exists()
        if IS_WINDOWS:
            self._windows_shortcut(bindir / "cod3.exe", self.out / "Call of Duty 3.lnk")
            if mp:
                self._windows_shortcut(bindir / "cod3mp.exe", self.out / "Call of Duty 3 Multiplayer.lnk")
        else:
            launcher = self.out / "Call of Duty 3.sh"
            launcher.write_text('#!/bin/sh\ncd "$(dirname "$0")" && exec ./bin/cod3 "$@"\n', encoding="utf-8")
            launcher.chmod(0o755)
            if mp:
                launcher = self.out / "Call of Duty 3 Multiplayer.sh"
                launcher.write_text('#!/bin/sh\ncd "$(dirname "$0")" && exec ./bin/cod3mp "$@"\n',
                                    encoding="utf-8")
                launcher.chmod(0o755)
        self.log.info(f"Game executable: {self.out / self.exe_rel}")

    def _appimage(self, bins: List[Path]) -> bool:
        tool = deps.find_appimagetool(self.tools_dir, self.runner, self.log, self.cancel)
        if not tool:
            self.log.warn("No appimagetool; installing as a plain folder instead.")
            return False
        appdir = self.work / "AppDir"
        if appdir.exists():
            safe_rmtree(appdir, self.work, self.protected, self.log)
        bindir = appdir / "usr" / "bin"
        bindir.mkdir(parents=True)
        for f in bins:
            shutil.copy2(f, bindir / f.name)
        # COD3_PROGRAM picks the program: the campaign, or multiplayer (cod3mp)
        # when one starts the other (recomp/src/game/relaunch.cpp sets it).
        (appdir / "AppRun").write_text('#!/bin/sh\nHERE="$(dirname "$(readlink -f "$0")")"\n'
                                       'if [ "$COD3_PROGRAM" = cod3mp ] && [ -x "$HERE/usr/bin/cod3mp" ]; then\n'
                                       '  exec "$HERE/usr/bin/cod3mp" "$@"\nfi\n'
                                       'exec "$HERE/usr/bin/cod3" "$@"\n', encoding="utf-8")
        (appdir / "AppRun").chmod(0o755)
        (appdir / "cod3.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Call of Duty 3 (recomp)\nExec=cod3\nIcon=cod3\n"
            "Categories=Game;\nTerminal=false\n", encoding="utf-8")
        (appdir / "cod3.png").write_bytes(make_icon_png())
        target = self.out / "CoD3-x86_64.AppImage"
        env = dict(self.tc.env if self.tc else os.environ)
        env.update({"ARCH": "x86_64", "APPIMAGE_EXTRACT_AND_RUN": "1"})
        # The AppImage starts with the same runtime as appimagetool itself (a
        # pinned, checksummed download), so nothing else is fetched here.
        runtime = self.work / "appimage-runtime"
        with open(tool, "rb") as f:
            runtime.write_bytes(f.read(binaries.elf_section_end(tool)))
        rc = self.runner.run([tool, "--no-appstream", "--runtime-file", runtime, appdir, target], cwd=self.work,
                             env=env, check=False)
        if rc != 0 or not target.exists():
            self.log.warn("appimagetool failed; installing as a plain folder instead.")
            return False
        target.chmod(0o755)
        if (bindir / "cod3mp").exists():
            launcher = self.out / "Call of Duty 3 Multiplayer.sh"
            launcher.write_text('#!/bin/sh\ncd "$(dirname "$0")" && COD3_PROGRAM=cod3mp exec ./' + target.name +
                                ' "$@"\n', encoding="utf-8")
            launcher.chmod(0o755)
        self.exe_rel = target.name
        self.packed["linux"] = [target.name]
        self.log.info(f"Game executable: {target}")
        return True

    def _windows_shortcut(self, target: Path, link: Path) -> None:
        ps = ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut($env:C3W_LINK);"
              "$s.TargetPath=$env:C3W_TARGET;$s.WorkingDirectory=$env:C3W_DIR;$s.Save()")
        env = dict(os.environ, C3W_LINK=str(link), C3W_TARGET=str(target), C3W_DIR=str(target.parent))
        self.runner.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], env=env, check=False,
                        quiet=True)

    # ------------------------------------------------------------------
    # 7a. The decomp trace version (optional; docs/DECOMP_TRACE.md)
    # ------------------------------------------------------------------
    def trace_game(self) -> None:
        """This system's game once more with COD3_DECOMP_TRACE (every game
        function notes when it first runs and who calls it), in a build folder
        of its own from the same translated code, into <output>/bin-trace/. It
        uses the normal game's files and userdata; the normal game is
        unchanged. Optional: if it fails, the normal game is unaffected and
        the reason is logged."""
        try:
            self._trace_game()
        except Cancelled:
            raise
        except Exception as e:  # noqa: BLE001 - a compile or copy failure, reported the same way
            self.log.warn(f"The decomp trace version wasn't made: {e}")

    def _trace_game(self) -> None:
        assert self.tc
        build = self.trace_build_dir
        self._forget_other_compiler(build)
        self.log.info("Configuring the decomp trace version (its own build folder)...")
        # Only the campaign program is traced; the multiplayer program is not built here.
        self.runner.run(self._game_configure(build, "-DCOD3_DECOMP_TRACE=ON", "-DCOD3_BUILD_MP=OFF"),
                        cwd=self.proj, env=self.tc.env)
        self.log.info(f"Compiling the decomp trace version with {self.o.jobs} threads. It compiles the whole "
                      "game once more, so it takes about as long as the normal game did.")
        self.runner.run([self.tc.cmake, "--build", build, "--parallel", str(self.o.jobs)],
                        cwd=self.proj, env=self.tc.env, quiet=True, line_filter=self._ninja_progress("Trace"))
        if not (build / exe_name("cod3")).exists():
            raise SetupError("its build finished but cod3 was not produced; see the log")
        missing = missing_levels(build)
        if missing:
            raise SetupError("its build finished without the level libraries for: " + ", ".join(missing))
        bins = self._binaries(build)
        bindir = self.out / TRACE_BIN_DIR
        bindir.mkdir(parents=True, exist_ok=True)
        for f in bins:
            try:
                shutil.copy2(f, bindir / f.name)
            except OSError as e:
                raise SetupError(f"couldn't replace {bindir / f.name} ({e}); close the trace version of the game "
                                 "and build again") from None
        self.trace_exe_rel = f"{TRACE_BIN_DIR}/{exe_name('cod3')}"
        self.packed["trace"] = [f"{TRACE_BIN_DIR}/{f.name}" for f in bins]
        if IS_WINDOWS:
            self._windows_shortcut(bindir / "cod3.exe", self.out / "Call of Duty 3 (decomp trace).lnk")
        else:
            launcher = self.out / "Call of Duty 3 (decomp trace).sh"
            launcher.write_text(f'#!/bin/sh\ncd "$(dirname "$0")" && exec ./{TRACE_BIN_DIR}/cod3 "$@"\n',
                                encoding="utf-8")
            launcher.chmod(0o755)
        self.log.info(f"Decomp trace version: {self.out / self.trace_exe_rel} (same game files and settings; "
                      "its traces go to userdata/decomp_trace/, see docs/DECOMP_TRACE.md)")

    # ------------------------------------------------------------------
    # 7b. The Windows version, built on Linux (optional)
    # ------------------------------------------------------------------
    def windows_game(self) -> None:
        """Builds cod3.exe with llvm-mingw into <output>/bin-windows/. Optional:
        if it fails, the Linux game is unaffected and the reason is logged."""
        try:
            self._windows_game()
        except Cancelled:
            raise
        except Exception as e:  # noqa: BLE001 - network, unpacking, links: all reported the same way
            if not self.native:   # it was the only version asked for
                raise SetupError(f"The Windows version wasn't made: {e}") from e
            self.log.warn(f"The Windows version wasn't made: {e}")

    def _windows_game(self) -> None:
        assert self.tc
        toolchain_bin = deps.fetch_portable("llvm-mingw-linux", self.tools_dir, self.runner, self.log, self.cancel)
        safe_rmtree(self.work / "mingw" / "include", self.work, self.protected, self.log)
        toolchain = cross.prepare(self.work, toolchain_bin, self.tool_root / "cross" / "mingw" / "include",
                                  self.sdk_src)
        env = self.tc.env
        ninja = [f"-DCMAKE_MAKE_PROGRAM={self.tc.ninja}"]

        # The SDK, for Windows. Rebuilt only when what goes into it changes.
        sdk_build = self.sdk_src / "out" / "build" / "mingw"
        sdk_install = self.sdk_src / "out" / "install" / "mingw"
        # Patches only the Windows build needs (patches/mingw). They are kept
        # out of the main list so they never touch the Linux SDK's headers,
        # which would make the Linux game rebuild from scratch.
        cross_patches = sorted((self.patches / "mingw").glob("*.patch"))
        patches = sorted(self.patches.glob("*.patch")) + cross_patches
        stamp_data = {"commit": SDK_COMMIT, "toolchain": deps.PORTABLE["llvm-mingw-linux"][2],
                      "patches": [hashlib.sha1(p.read_bytes()).hexdigest() for p in patches],
                      "shaders": sdk_shaders.version(), "backends": "d3d12+vulkan"}
        stamp = self.work / "sdk-mingw.stamp.json"
        if read_json(stamp) == stamp_data and (sdk_install / "lib" / "cmake" / "rexglue").exists():
            self.log.info("ReXGlue SDK for Windows already built.")
        else:
            # Same checkout as the Linux SDK (already built and installed by
            # now): start from the pinned commit with every patch, mingw ones
            # last. The next Linux SDK build starts clean again.
            self._clean_checkout()
            self._apply_patches(patches)
            self.log.info("Configuring the SDK for Windows (llvm-mingw)...")
            self.runner.run([self.tc.cmake, "-S", self.sdk_src, "-B", sdk_build, "-G", "Ninja", *ninja,
                             *cross.sdk_configure_args(toolchain, sdk_install)], cwd=self.sdk_src, env=env)
            self.log.info(f"Building the SDK for Windows with {self.o.jobs} threads...")
            self.runner.run([self.tc.cmake, "--build", sdk_build, "--target", "install", "--parallel",
                             str(self.o.jobs)], cwd=self.sdk_src, env=env, quiet=True,
                            line_filter=self._ninja_progress("SDK (Windows)"))
            write_json(stamp, stamp_data)

        # The game, for Windows. The Linux rexglue runs the codegen (same output).
        build = self.proj / "out" / "build" / "mingw"
        self.log.info("Configuring the Windows version of the game...")
        self.runner.run([self.tc.cmake, "-S", self.proj, "-B", build, "-G", "Ninja", *ninja,
                         *cross.game_configure_args(toolchain, sdk_install, self.sdk_install / "bin" / "rexglue")],
                        cwd=self.proj, env=env)
        self.log.info(f"Compiling the Windows version with {self.o.jobs} threads (about as long as the Linux one)...")
        self.runner.run([self.tc.cmake, "--build", build, "--parallel", str(self.o.jobs)], cwd=self.proj, env=env,
                        quiet=True, line_filter=self._ninja_progress("CoD3 (Windows)"))
        exe = build / "cod3.exe"
        if not exe.exists():
            raise SetupError("the build finished but cod3.exe was not produced; see the log.")

        # Pack it: the game's DLLs, the GPU plugin, the SDK's runtime DLL and
        # llvm-mingw's C++ runtime (libc++, libunwind), following every import
        # table. Direct3D 12, DXGI and the Vulkan loader are Windows' own
        # (under Wine/Proton: vkd3d-proton and winevulkan), so they aren't
        # packed; the DLLs that are packed are listed in the log.
        search = [build, sdk_install / "bin", cross.runtime_dll_folder(toolchain_bin)]
        mp_exe = build / "cod3mp.exe"   # the multiplayer program, when it was built
        start = self._with_gpu_plugin([exe] + ([mp_exe] if mp_exe.exists() else []) + sorted(build.glob("*.dll")),
                                      search, windows=True)
        files, missing = binaries.gather(start, search, cross.is_windows_system_dll)
        if missing:
            raise SetupError("cod3.exe needs DLLs that are neither built with it nor part of Windows: " +
                             ", ".join(sorted({lib for _, lib in missing})))
        bindir = self.out / "bin-windows"
        if bindir.exists():
            safe_rmtree(bindir, self.out, [p for p in self.protected if p != self.out], self.log)
        bindir.mkdir(parents=True)
        for f in files:
            shutil.copy2(f, bindir / f.name)
        self.windows_exe_rel = "bin-windows/cod3.exe"
        self.packed["windows"] = [f"bin-windows/{f.name}" for f in files]
        self.log.info("Windows files: " + ", ".join(f.name for f in files))
        self.log.info(f"Windows version: {bindir / 'cod3.exe'} ({len(files)} files). Copy the whole output folder "
                      "to a Windows PC and start bin-windows\\cod3.exe; it uses the same assets/ and userdata/.")

    # ------------------------------------------------------------------
    # 7c. The Linux version, built on Windows inside WSL 2 (optional)
    # ------------------------------------------------------------------
    def linux_in_wsl(self) -> None:
        """Builds the AppImage in a private Ubuntu under WSL 2 (c3w/wsl.py),
        reading the game files extracted above and writing into the same
        output folder. Optional: if WSL isn't ready or the build fails, the
        reason is logged and the rest is unaffected."""
        try:
            self._linux_in_wsl()
        except Cancelled:
            raise
        except Exception as e:  # noqa: BLE001
            if not self.native:   # it was the only version asked for
                raise SetupError(f"The Linux version wasn't made: {e}") from e
            self.log.warn(f"The Linux version wasn't made: {e}")

    def _linux_in_wsl(self) -> None:
        from . import wsl as wsl_mod
        w = wsl_mod.Wsl(self.runner, self.log, self.cancel)
        w.check()
        self.used_wsl = True   # from here on the build Ubuntu may exist; cleanup removes it
        w.ensure_distro(self.work / "wsl", self.tools_dir / "downloads")
        # Python inside the build Ubuntu (Canonical's image has it; this is a fallback).
        w.run_root(["sh", "-c", "command -v python3 >/dev/null || "
                                "(apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y python3)"])
        tool = wsl_mod.to_wsl_path(self.tool_root)
        args = ["python3", "-u", "cod3_waw_setup.py", "build", "--binaries-only", "--linux",
                "--iso", wsl_mod.to_wsl_path(self.assets), "--output", wsl_mod.to_wsl_path(self.out),
                "--work", wsl_mod.WORK_INSIDE, "--jobs", str(self.o.jobs), "--no-waw",
                "--yes"]   # approved here, with the build Ubuntu (deps.wsl_install)
        if not self.o.appimage:
            args.append("--no-appimage")
        if self.o.keep_sdk or not self.o.cleanup:
            args.append("--keep-sdk")
        if not self.o.install_deps:
            args.append("--no-install-deps")
        if self.o.force_unknown_iso:
            args.append("--force")
        if self.o.skip_dep_check:
            args.append("--skip-dep-check")
        self.log.info("Building the Linux version inside WSL (its own SDK and compile; about as long as the "
                      "Windows one)...")
        started = time.time() - 2
        rc = w.run_root(args, cwd_linux=tool, quiet=True, line_filter=self._relay_inner_log("Linux (WSL)"))
        # Only what this run wrote counts (an AppImage from an earlier build
        # may still be there).
        fresh = [p for p in (self.out / "CoD3-x86_64.AppImage", self.out / "bin" / "cod3")
                 if p.exists() and p.stat().st_mtime >= started]
        if rc != 0 or not fresh:
            raise SetupError(f"the build inside WSL stopped (exit code {rc}); see the log above.")
        self.used_wsl = True
        self.linux_exe_rel = fresh[0].relative_to(self.out).as_posix()
        self.log.info(f"Linux version: {self.out / self.linux_exe_rel}. On Linux, start it from the same "
                      "output folder (it uses the same assets/ and userdata/).")

    # ------------------------------------------------------------------
    # 8. Finish
    # ------------------------------------------------------------------
    def finish(self) -> None:
        (self.out / "userdata").mkdir(exist_ok=True)
        manifest_path = self.out / "cod3_waw.json"
        manifest = read_json(manifest_path)
        # Each version's executable, relative to the output folder. "cod3_exe"
        # (older launchers) is the version for the system that built it.
        linux_exe = (self.exe_rel if IS_LINUX else self.linux_exe_rel) or manifest.get("cod3_exe_linux")
        windows_exe = (self.exe_rel if IS_WINDOWS else self.windows_exe_rel) or manifest.get("cod3_exe_windows")

        def packed(system: str, built_elsewhere: Optional[str]) -> Optional[List[str]]:
            # Each version's files, for the check in c3w/repair.py: this run's
            # list, else the last one, unless that version was rebuilt
            # without a list (inside WSL).
            if system in self.packed:
                return self.packed[system]
            return None if built_elsewhere else manifest.get(f"cod3_files_{system}")
        manifest.update({
            "tool_version": TOOL_VERSION,
            "built": time.strftime("%Y-%m-%d %H:%M:%S"),
            "platform": host_triple(),
            "cod3_exe": self.exe_rel or manifest.get("cod3_exe"),
            "cod3_exe_linux": linux_exe,
            "cod3_exe_windows": windows_exe,
            "cod3_files_linux": packed("linux", self.linux_exe_rel),
            "cod3_files_windows": packed("windows", None),
            # The decomp trace version (bin-trace/), when this or an earlier build made it.
            "cod3_trace_exe": self.trace_exe_rel or (manifest.get("cod3_trace_exe")
                                                     if (self.out / TRACE_BIN_DIR).is_dir() else None),
            "cod3_files_trace": packed("trace", None) if (self.out / TRACE_BIN_DIR).is_dir() else None,
            "cod3_data": COD3_DATA.as_posix(),
            # The upscaled PNGs (any scale), under their old name if a
            # textures_2x folder from an earlier build wasn't renamed.
            "cod3_textures_upscaled": next(
                ((COD3_DATA / name).as_posix() for name in (upscale.OUTPUT_NAME, upscale.LEGACY_OUTPUT_NAME)
                 if (self.out / COD3_DATA / name).is_dir()), None),
            # The HD texture pack the game reads (found by the game itself in
            # data/cod3/hd_textures; recorded here for the launcher and tools).
            "cod3_hd_textures": ((COD3_DATA / upscale.PACK_NAME).as_posix()
                                 if upscale.pack_ready(self.out / COD3_DATA / upscale.PACK_NAME) else None),
            # The player's own texture pack (c3w/user_textures.py), found by
            # the game in data/cod3/user_textures.
            "cod3_user_textures": ((COD3_DATA / user_textures.PACK_NAME).as_posix()
                                   if user_textures.pack_keys(self.out / COD3_DATA) else None),
            # (A repair that doesn't rip World at War keeps what the last rip used.)
            "waw_source": ({"kind": self.waw.kind, "path": str(self.waw.path) if self.waw.path else None}
                           if self._runs("waw") or "waw_source" not in manifest else manifest["waw_source"]),
            "waw_data": waw.OUTPUT_DATA.as_posix() if self.waw_ripped else manifest.get("waw_data"),
            "ripper": f"tools/{exe_name('rip')}",
            "sdk_commit": SDK_COMMIT if self.o.build_recomp else manifest.get("sdk_commit"),
        })
        manifest.pop("waw_original", None)   # from tool 1.x
        manifest.pop("waw_copy", None)
        write_json(manifest_path, manifest)

        # A copy of this tool, so the output folder has its own launcher /
        # map installer even after the setup tool folder is gone.
        launcher = self.out / "launcher"
        # data/: the insignia list the launcher's "Build user texture pack" uses.
        for item in ("cod3_waw_setup.py", "c3w", "data"):
            src = self.tool_root / item
            dst = launcher / item
            if not src.exists():
                continue
            if src.is_dir():
                shutil.copytree(src, dst, dirs_exist_ok=True, ignore=shutil.ignore_patterns("__pycache__"))
            else:
                launcher.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
        # Both launchers: the output folder may hold both versions and be
        # copied to the other system.
        builder = Path(sys.executable).with_name("pythonw.exe") if IS_WINDOWS and sys.executable else None
        try:
            # cmd reads a .bat in the console's (OEM) code page.
            bat = launcher_bat(builder).encode("oem" if IS_WINDOWS else "utf-8")
        except (UnicodeEncodeError, LookupError):
            bat = launcher_bat(None).encode("utf-8")   # a path it can't spell: PATH's Python only
        (self.out / "Launcher.bat").write_bytes(bat)
        sh = self.out / "Launcher.sh"
        sh.write_text('#!/bin/sh\ncd "$(dirname "$0")" && exec python3 launcher/cod3_waw_setup.py --launcher "$@"\n',
                      encoding="utf-8")
        if not IS_WINDOWS:
            sh.chmod(0o755)
        built = [f"Linux: {linux_exe}" if linux_exe else "", f"Windows: {windows_exe}" if windows_exe else ""]
        if any(built):
            self.log.info("Game versions in the output folder: " + "; ".join(b for b in built if b))
        self.log.info(f"Launcher: {self.out / ('Launcher.bat' if IS_WINDOWS else 'Launcher.sh')}")

    # ------------------------------------------------------------------
    # 9. Cleanup
    # ------------------------------------------------------------------
    def cleanup(self) -> None:
        if not self.o.cleanup:
            self.log.info(f"Keeping the work folder ({self.work_real}) as requested.")
            return
        if self.log.file and is_within(self.log.file, self.work_real):
            self.log.file = None  # the log couldn't move to the output folder; it goes with the work folder
        if IS_WINDOWS and (self.used_wsl or (self.work / "wsl").exists()):
            # The build Ubuntu's disk is in work/wsl; WSL has to let go of it first.
            if self.o.keep_sdk:
                self.log.info("Keeping the build Ubuntu in WSL for faster rebuilds.")
            else:
                from . import wsl as wsl_mod
                try:
                    wsl_mod.Wsl(self.runner, self.log, self.cancel).unregister()
                except SetupError as e:
                    self.log.warn(f"Couldn't remove the build Ubuntu from WSL ({e}); remove it with "
                                  f"'wsl --unregister {wsl_mod.DISTRO}'.")
        # tools/ always stays: the downloaded tools, Rust and (Windows) Python
        # live there, so the next build needn't download them again.
        keep = {"tools", WORK_MARKER} | ({"rexglue-sdk", "sdk.stamp.json", "sdk-mingw.stamp.json", "rust-target",
                                          "wsl"} if self.o.keep_sdk else set())
        if not owns_work_dir(self.work_real):
            self.log.warn(f"{self.work_real} isn't marked as this tool's work folder; nothing in it was deleted.")
            return
        for child in list(self.work.iterdir()):
            if child.name in keep:
                continue
            if child.is_dir():
                safe_rmtree(child, self.work, self.protected, self.log)
            else:
                child.unlink(missing_ok=True)
        if self.o.keep_sdk:
            build = self.sdk_src / "out" / "build"
            if build.exists():
                safe_rmtree(build, self.work, self.protected, self.log)  # the install tree is enough
        else:
            remove_build_link(self.work, self.work_real)
        self.log.info("Temporary files removed. The CoD3 ISO and the World at War files were not touched. "
                      f"The downloaded tools stay in {self.work_real / 'tools'} for the next build.")


def launcher_bat(python: Optional[Path] = None) -> str:
    """Launcher.bat: starts the launcher copy in the output folder with
    Python's windowless pythonw. It tries the Python that built this folder
    first (setup.bat's own Python in work\\tools\\python is on no PATH), then
    the py launcher and pythonw on PATH, and says what to do when there is
    none instead of failing without a word."""
    lines = ["@echo off", "rem Starts the COD3 launcher (the setup tool's Play page).", 'cd /d "%~dp0"', 'set "PYW="']
    if python:
        quoted = str(python).replace("%", "%%")
        lines.append(f'if exist "{quoted}" set PYW="{quoted}"')
    lines += ["if not defined PYW where pyw >nul 2>&1 && set PYW=pyw -3",
              "if not defined PYW where pythonw >nul 2>&1 && set PYW=pythonw",
              "if not defined PYW goto no_python",
              'start "" %PYW% "launcher\\cod3_waw_setup.py" --launcher',
              "exit /b",
              ":no_python",
              "echo Python 3 with Tkinter is needed for the launcher, and none was found.",
              "echo Install it from https://www.python.org/downloads/ (tick \"tcl/tk and IDLE\"), or run",
              "echo setup.bat in the project folder, which installs one. The game itself starts without it:",
              "echo bin\\cod3.exe (or bin-windows\\cod3.exe) in this folder.",
              "pause"]
    return "\r\n".join(lines) + "\r\n"


# The SDK (sdk.stamp.json) the generated code was last made with, in the
# project's generated/ folder (so deleting that folder forgets it too).
CODEGEN_SDK_STAMP = "c3w-codegen-sdk.json"


def codegen_sdk_changed(proj: Path, sdk_key: dict) -> bool:
    """Whether the generated code in `proj` was made with another SDK (or
    with one this tool didn't record)."""
    return read_json(Path(proj) / "generated" / CODEGEN_SDK_STAMP) != sdk_key


def forget_codegen(proj: Path) -> bool:
    """Removes the codegen's own up-to-date stamps (the build's
    generated/default/codegen.build.stamp and each module's codegen.stamp),
    so the next codegen run translates every module. The generated code
    itself stays. Returns whether there was anything to forget."""
    generated = Path(proj) / "generated"
    stamps = list(generated.glob("*/codegen.stamp")) + list(generated.glob("*/codegen.build.stamp"))
    for f in stamps:
        f.unlink(missing_ok=True)
    return bool(stamps)


def lf_patch(patch: Path, scratch: Path) -> Path:
    """`patch` itself, or a copy with LF line endings in `scratch` when every
    line of it ends in CRLF: a copy whose line endings an editor, a zip tool
    or a sync on Windows changed. git apply refuses that one, since the SDK's
    files end their lines with LF; none of the patches changes CRLF lines."""
    data = patch.read_bytes()
    lines = data.split(b"\n")[:-1]   # every line that has a line break after it
    if not lines or not all(line.endswith(b"\r") for line in lines):
        return patch
    scratch.mkdir(parents=True, exist_ok=True)
    copy = scratch / patch.name
    copy.write_bytes(data.replace(b"\r\n", b"\n"))
    return copy


def missing_levels(build_dir: Path) -> List[str]:
    """Levels whose recompiled library (libcod3_<level>.so / cod3_<level>.dll)
    isn't in a build folder."""
    found = {p.name for p in build_dir.glob("*cod3_*.*") if p.suffix.lower() in (".so", ".dll")}
    return [lvl for lvl in LEVELS
            if not any(n.endswith(f"cod3_{lvl}.so") or n.endswith(f"cod3_{lvl}.dll") for n in found)]


def _same_device(a: Path, b: Path) -> bool:
    def existing(p: Path) -> Path:
        while not p.exists() and p.parent != p:
            p = p.parent
        return p
    try:
        return os.stat(existing(a)).st_dev == os.stat(existing(b)).st_dev
    except OSError:
        return False


def make_icon_png(size: int = 256) -> bytes:
    """A simple original icon (olive disc, white '3' bars, red dot) as PNG."""
    px = bytearray()
    c = size / 2
    for y in range(size):
        px.append(0)
        for x in range(size):
            dx, dy = x - c + 0.5, y - c + 0.5
            r = (dx * dx + dy * dy) ** 0.5
            rgba = (0, 0, 0, 0)
            if r < size * 0.48:
                rgba = (74, 83, 52, 255)
                if r > size * 0.44:
                    rgba = (40, 44, 28, 255)
                # a blocky "3": three horizontal bars joined on the right
                u, v = x / size, y / size
                bar = any(abs(v - yy) < 0.045 for yy in (0.30, 0.50, 0.70)) and 0.36 < u < 0.64
                stem = 0.56 < u < 0.64 and 0.255 < v < 0.745
                if bar or stem:
                    rgba = (235, 230, 215, 255)
                if (u - 0.70) ** 2 + (v - 0.30) ** 2 < 0.0012:
                    rgba = (200, 40, 30, 255)
            px.extend(rgba)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(px), 9)) + chunk(b"IEND", b""))

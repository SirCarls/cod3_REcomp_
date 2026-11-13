"""Call of Duty 2, World at War and Call of Duty 4 (Xbox 360) recompiled for PC from your own disc, the same way
Call of Duty 3 is: the same ReXGlue SDK, patches and toolchain, Linux and Windows.
[waw-decomp] (owner 2026-11-08 13:02: "Please include cod2 WAW and cod4 within this release"; 2026-11-07 11:49:
"the setup tool says rust, but it should be the same as cod3's SDK / Vulkan and Dx12").

Each game's port is a folder next to Call of Duty 3's recomp/ (recomp_cod2/, recomp_waw/, recomp_cod4/), made by
the second account's chats acct2-cod2, acct2-waw and acct2-cod4. A build here:

1. Checks the disc: an Xbox 360 disc image (.iso) or a folder copied from the disc, whose programs (default.xex,
   default_mp.xex) are the ones the port's tables were made for (their SHA-1s below).
2. Copies the disc's files into <output>/<game>/assets: the game reads them from there.
3. Builds the ReXGlue SDK, exactly as Call of Duty 3's build does (Pipeline.sdk; one SDK serves every game, and a
   work folder kept from Call of Duty 3's build is reused as it is).
4. Copies the port into <work>/<game>, the programs into its assets/, translates their PowerPC code (the codegen)
   and compiles it with CMake against the SDK.
5. Packs each program with the libraries it needs into <output>/<game>/bin (and bin-windows for the Windows version
   made on Linux), with launchers and <game>.json.

Graphics are the SDK's Xenos GPU emulation for now (Vulkan; Direct3D 12 or Vulkan on Windows), as for Call of Duty
3 before its native renderer; each game's native renderer comes later from its own chat. The disc is only read; what
runs is the recompiled code.

Plain logic, no Tk, so the tests run without Tkinter.
"""
from __future__ import annotations

import hashlib
import shutil
import threading
import time
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from . import binaries, cross, deps, game_settings
from .common import (IS_LINUX, IS_WINDOWS, Log, SetupError, copy_tree, dir_size, exe_name, free_space,
                     host_triple, human, is_within, overlaps, read_json, resolved, safe_rmtree, sha1_file,
                     write_json)
from .pipeline import (BINARIES_SIZE, TOOL_VERSION, WORK_NEEDED_RECOMP, BuildOptions, Pipeline, Step,
                       linux_launcher)
from .xiso import XisoImage


@dataclass(frozen=True)
class Program:
    """One program of a game: the campaign or the multiplayer one."""
    name: str            # the executable's name: cod2, cod2mp ...
    xex: str             # its program on the disc
    sha1: str            # the disc program's SHA-1 the port was made for
    label: str           # "Campaign", "Multiplayer"
    project: str = ""    # its CMake project, relative to the port's folder ("" = the port's own; "mp" for cod4mp)
    # Where the codegen reads the program, relative to the port's folder.
    asset_dir: str = "assets"

    @property
    def codegen_target(self) -> str:
        return f"{self.name}_codegen"


@dataclass(frozen=True)
class Port:
    id: str
    name: str                     # "Call of Duty 2"
    template: str                 # the port's folder next to the setup tool
    programs: Tuple[Program, ...]
    disc_size: int                # about how big the disc's files are
    settings_prefix: str          # the port's own settings (cod2_..., waw_..., cod4_...)
    skip_films: str = ""          # its "skip the start-up films" setting, if it has one
    fov: str = ""                 # its field of view setting, if it has one
    fov_whole: bool = False       # [f2-menus] that setting takes whole degrees (cod2_fov is an integer)
    max_fps: str = ""             # its frame limit setting, if it has one
    # [f2-menus] 2026-11-10 That setting's own range and what its 0 means, as each port defines it (cod2_max_fps:
    # 0 = none, up to 1000; waw_max_fps: 0 = the game's own limit, up to 1000; cod4_max_fps: the same, up to 250).
    max_fps_top: int = 1000
    max_fps_zero: str = "none"
    max_fps_default: int = 0      # what the port starts with when its file has no line for it
    # Projects that build another folder's sources from ../recomp/src (Call of Duty 3's test pad).
    uses_cod3_src: bool = False

    @property
    def subdir(self) -> str:      # <output>/<id> and <work>/<id>
        return self.id

    @property
    def manifest(self) -> str:
        return f"{self.id}.json"

    def program(self, name: str) -> Program:
        for p in self.programs:
            if p.name == name:
                return p
        raise KeyError(name)

    @property
    def projects(self) -> List[str]:
        """Its CMake projects, in build order: the port's folder first."""
        out: List[str] = []
        for p in self.programs:
            if p.project not in out:
                out.append(p.project)
        return out


COD2 = Port("cod2", "Call of Duty 2", "recomp_cod2", (
    Program("cod2", "default.xex", "16da73ad198bea9f8203063ddb51172508064fb0", "Campaign"),
    # cod2mp is built from mp/ as a subfolder of the port's own project (its manifest reads ../assets).
    Program("cod2mp", "default_mp.xex", "5c9672b2f3a9178192a6fab533ecd0d513ec3280", "Multiplayer"),
), disc_size=5_600_000_000, settings_prefix="cod2", skip_films="cod2_skip_intro_movies", max_fps="cod2_max_fps",
    fov="cod2_fov", fov_whole=True,   # [f2-menus] 2026-11-10 the port has cod2_fov; the Play page had no row for it
    max_fps_default=60)

WAW = Port("waw", "Call of Duty: World at War", "recomp_waw", (
    Program("waw", "default.xex", "9972cdb7c1bf2f54c6ef7d58c9eb3b6d4a724708", "Campaign"),
), disc_size=7_300_000_000, settings_prefix="waw", fov="waw_fov", max_fps="waw_max_fps",
    max_fps_zero="the game's own limit", uses_cod3_src=True)

COD4 = Port("cod4", "Call of Duty 4: Modern Warfare", "recomp_cod4", (
    Program("cod4", "default.xex", "2f3148ca7e4c17140e0d44714e204f570197af51", "Campaign"),
    # cod4mp is a project of its own in mp/, with its own assets/ for the codegen.
    Program("cod4mp", "default_mp.xex", "9d2f0236a318f36e31930d01cbe15df641af2f54", "Multiplayer", project="mp",
            asset_dir="mp/assets"),
), disc_size=7_000_000_000, settings_prefix="cod4", skip_films="cod4_skip_intro_movies", fov="cod4_fov",
    max_fps="cod4_max_fps",   # [f2-menus] 2026-11-10 the port has cod4_max_fps; the Play page had no row for it
    max_fps_top=250, max_fps_zero="the game's own limit",
    uses_cod3_src=True)

PORTS: Dict[str, Port] = {p.id: p for p in (COD2, WAW, COD4)}
PORT_ORDER = ("cod2", "waw", "cod4")

# What a port's build adds to the work folder besides the SDK (the copy, the translated code and the objects).
WORK_NEEDED_PORT = 6 * 1024 ** 3


def port(port_id: str) -> Port:
    try:
        return PORTS[port_id]
    except KeyError:
        raise SetupError(f"Unknown game {port_id!r}: one of {', '.join(PORT_ORDER)}.") from None


# ---------------------------------------------------------------------- the disc
@dataclass
class Disc:
    path: Path
    is_image: bool
    sha1: Dict[str, str]       # program name -> its SHA-1 on this disc ("" = not there)
    size: int

    def has(self, program: Program) -> bool:
        return bool(self.sha1.get(program.name))


def read_disc(p: Port, text: str) -> Disc:
    """The disc at `text` (an image or a folder): its programs' SHA-1s. Reads only."""
    if not text.strip():
        raise SetupError(f"Choose the {p.name} Xbox 360 disc image (.iso), or a folder copied from the disc.")
    path = resolved(Path(text.strip()).expanduser())
    if not path.exists():
        raise SetupError(f"{path} doesn't exist.")
    sums: Dict[str, str] = {}
    if path.is_dir():
        for prog in p.programs:
            f = path / prog.xex
            sums[prog.name] = sha1_file(f) if f.is_file() else ""
        return Disc(path, False, sums, dir_size(path))
    with XisoImage(path) as img:
        for prog in p.programs:
            sums[prog.name] = hashlib.sha1(img.read(prog.xex)).hexdigest() if img.find(prog.xex) else ""
        return Disc(path, True, sums, img.total_size)


def check_disc(p: Port, text: str, force_unknown: bool = False) -> Tuple[Optional[Disc], str]:
    """(the disc, one line for the Set up page). A disc whose campaign program isn't the one the port knows is
    refused (the port's tables name addresses in that program), unless `force_unknown`. A missing or unknown
    multiplayer program only leaves multiplayer out."""
    try:
        disc = read_disc(p, text)
    except SetupError as e:
        return None, str(e)
    except (OSError, ValueError) as e:
        return None, f"Couldn't read {text}: {e}"
    campaign = p.programs[0]
    got = disc.sha1.get(campaign.name, "")
    if not got:
        return None, (f"No {campaign.xex} in {disc.path.name}: this isn't a {p.name} Xbox 360 disc "
                      "(or the folder isn't the disc's top folder).")
    if got != campaign.sha1 and not force_unknown:
        return None, (f"This {campaign.xex} (SHA-1 {got[:12]}...) isn't the one the port was made for "
                      f"({campaign.sha1[:12]}...): a different release or region of {p.name}.")
    notes = []
    for prog in p.programs[1:]:
        s = disc.sha1.get(prog.name, "")
        if not s:
            notes.append(f"no {prog.xex}, so no {prog.label.lower()}")
        elif s != prog.sha1:
            notes.append(f"{prog.xex} isn't the known one, so no {prog.label.lower()}")
    line = f"{p.name}: the disc the port knows ({human(disc.size)})"
    if got != campaign.sha1:
        line = f"{p.name}: an unknown {campaign.xex}, built anyway (expect errors or crashes)"
    return disc, line + ("; " + "; ".join(notes) if notes else "") + "."


def buildable(p: Port, disc: Disc, force_unknown: bool = False) -> List[Program]:
    """The programs this disc can build: the campaign, and each other program whose SHA-1 is the known one."""
    out = []
    for i, prog in enumerate(p.programs):
        s = disc.sha1.get(prog.name, "")
        if s and (s == prog.sha1 or (i == 0 and force_unknown)):
            out.append(prog)
    return out


# ---------------------------------------------------------------------- the build
class PortPipeline(Pipeline):
    """Call of Duty 3's pipeline, with its steps for one port instead: the checks, the tools, the disc's files,
    the SDK (shared, unchanged), the game's translation and compile, the Windows version, the finish and the
    cleanup."""

    def __init__(self, p: Port, source: str, opts: BuildOptions, log: Log, cancel: threading.Event,
                 on_step: Optional[Callable[[int, int, str], None]] = None):
        # Nothing of Call of Duty 3's own: no CoD3 disc, no World at War rip, no AppImage, no texture work.
        super().__init__(replace(opts, cod3_source="", waw_steam="", waw_xbox360="", waw_pc_disc="",
                                 waw_map_packs=[], appimage=False, upscale_textures=False, trace_build=False,
                                 binaries_only=False, build_recomp=True, only_steps=[]),
                         log, cancel, on_step)
        self.port = p
        self.port_src = resolved(Path(source.strip()).expanduser()) if source.strip() else None
        self.root = self.out / p.subdir           # <output>/<game>
        self.port_assets = self.root / "assets"
        self.disc: Optional[Disc] = None
        self.programs: List[Program] = []
        self.exe_rels: Dict[str, Dict[str, str]] = {}   # "linux"/"windows" -> program -> path in <output>/<game>
        self.files_rel: Dict[str, List[str]] = {}

    # Call of Duty 3's pipeline asks these.
    @property
    def protected(self) -> List[Path]:
        return [q for q in (self.port_src, self.out) if q]

    @property
    def _needs_rust(self) -> bool:
        return False

    @property
    def wsl_linux(self) -> bool:
        return False   # a port's Linux version is built on Linux

    @property
    def port_proj(self) -> Path:
        return self.work / self.port.subdir

    def steps(self) -> List[Step]:
        name = self.port.name
        steps = [Step("Checking inputs", self.preflight, "inputs"), Step("Tools", self.dependencies, "tools"),
                 Step(f"{name}: the disc's files", self.port_files, "assets"),
                 Step("ReXGlue SDK", self.sdk, "sdk")]
        if self.native:
            steps.append(Step(f"Recompiling {name}", self.port_build, "recompile"))
        if self.cross_windows:
            steps.append(Step(f"{name}: the Windows version", self.port_windows, "windows"))
        steps += [Step("Finishing", self.port_finish, "finish"), Step("Cleaning up", self.cleanup, "cleanup")]
        return steps

    # ------------------------------------------------------------------ 1. inputs
    def preflight(self) -> None:
        p = self.port
        if not (self.tool_root / p.template / "CMakeLists.txt").exists():
            raise SetupError(f"The port's folder is missing ({self.tool_root / p.template}). Keep {p.template}, "
                             "recomp and patches next to the setup tool.")
        if not self.port_src or not self.port_src.exists():
            raise SetupError(f"Choose the {p.name} Xbox 360 disc image (.iso), or a folder copied from it.")
        if not self.o.output_dir:
            raise SetupError("Choose an output folder.")
        if host_triple() not in ("linux-amd64", "win-amd64"):
            raise SetupError(f"Unsupported system {host_triple()}: the build supports 64-bit x86 Linux and Windows.")
        if IS_WINDOWS and self.o.build_linux and not self.o.build_windows:
            raise SetupError(f"{p.name}'s Linux version is built on Linux. Tick the Windows version here.")
        if not (self.native or self.cross_windows):
            raise SetupError("Tick at least one version of the game to build (Linux and/or Windows).")
        if IS_WINDOWS and self.o.build_linux:
            self.log.info(f"{p.name}: only the Windows version is made on Windows; build the Linux one on Linux.")
        for q in self.protected:
            if overlaps(self.work, q):
                raise SetupError(f"The work folder {self.work} overlaps {q}. Pick a separate work folder "
                                 "(it is deleted at the end).")
        if self.port_src.is_dir() and overlaps(self.port_src, self.root):
            raise SetupError(f"The output folder and the {p.name} disc folder can't contain each other.")

        disc, line = check_disc(p, str(self.port_src), self.o.force_unknown_iso)
        if not disc:
            raise SetupError(line)
        self.log.info(line)
        self.disc = disc
        self.programs = buildable(p, disc, self.o.force_unknown_iso)
        self.log.info("Building: " + ", ".join(f"{q.name} ({q.label.lower()})" for q in self.programs))

        builds = int(self.native) + int(self.cross_windows)
        need_out = BINARIES_SIZE * builds
        if not self._files_in_place():
            need_out += disc.size
        sdk_there = (self.sdk_install / "lib" / "cmake" / "rexglue").exists()
        need_work = (0 if sdk_there else WORK_NEEDED_RECOMP) + WORK_NEEDED_PORT * builds
        free_out, free_work = free_space(self.out), free_space(self.work)
        self.log.info(f"Output: {self.root}  (needs ~{human(need_out)}, {human(free_out)} free)")
        self.log.info(f"Work:   {self.work}  (needs ~{human(need_work)}, {human(free_work)} free)")
        if free_out < need_out:
            raise SetupError(f"Not enough space in the output folder: need ~{human(need_out)}.")
        if free_work < need_work:
            raise SetupError(f"Not enough space in the work folder: need ~{human(need_work)}.")
        self.root.mkdir(parents=True, exist_ok=True)

    def _files_in_place(self) -> bool:
        """The disc's files are in <output>/<game>/assets already (a rebuild), by the campaign program's SHA-1."""
        f = self.port_assets / self.port.programs[0].xex
        return f.is_file() and self.disc is not None and \
            sha1_file(f) == self.disc.sha1.get(self.port.programs[0].name)

    # ------------------------------------------------------------------ 2. the disc's files
    def port_files(self) -> None:
        assert self.disc and self.port_src
        if self._files_in_place() and dir_size(self.port_assets) >= self.disc.size:
            self.log.info(f"{self.port.name}'s files are in {self.port_assets} already.")
            return
        if self.port_src.is_dir():
            if resolved(self.port_src) == resolved(self.port_assets):
                return
            self.log.info(f"Copying {self.port.name}'s files from {self.port_src}")
            copy_tree(self.port_src, self.port_assets, self.log, self.cancel, "assets", hardlink=True)
            return
        self.log.info(f"Extracting {self.port_src.name} (read only) into {self.port_assets}")
        with XisoImage(self.port_src) as img:
            img.extract(self.port_assets, self.log, self.cancel)

    # ------------------------------------------------------------------ 3. translate and compile
    def _copy_port(self) -> None:
        """The port's folder -> <work>/<game>; only changed files are written, so ninja rebuilds only what they
        touch. Then each program into the assets/ folder its codegen reads."""
        src = self.tool_root / self.port.template
        n = 0
        for f in src.rglob("*"):
            rel = f.relative_to(src)
            if f.is_dir() or "__pycache__" in rel.parts or rel.parts[:1] in (("out",), ("assets",)):
                continue
            d = self.port_proj / rel
            if not d.exists() or d.read_bytes() != f.read_bytes():
                d.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(f, d)
                n += 1
        self.log.info(f"{self.port.template}: {n} file(s) copied into {self.port_proj}")
        for prog in self.programs:
            have = self.port_assets / prog.xex
            dest = self.port_proj / prog.asset_dir / prog.xex
            if not dest.exists() or sha1_file(dest) != sha1_file(have):
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(have, dest)
        # A program the disc lacks must not be built from an older copy.
        for prog in self.port.programs:
            if prog not in self.programs:
                stale = self.port_proj / prog.asset_dir / prog.xex
                if stale.exists():
                    stale.unlink()

    def _project_dir(self, project: str) -> Path:
        return self.port_proj / project if project else self.port_proj

    def _extra_args(self) -> List[str]:
        """Call of Duty 3's recomp/src for the ports that build its test pad from there."""
        return [f"-DCOD3_RECOMP_SRC={(self.tool_root / 'recomp' / 'src').as_posix()}"] if self.port.uses_cod3_src \
            else []

    def _projects(self) -> List[Tuple[str, List[Program]]]:
        """(project, its programs) for the programs this disc builds; cod2mp builds inside the port's own."""
        out: List[Tuple[str, List[Program]]] = []
        for project in self.port.projects:
            progs = [q for q in self.programs if q.project == project]
            if progs:
                out.append((project, progs))
        return out

    def _configure_native(self, src: Path, build: Path) -> list:
        assert self.tc
        return [self.tc.cmake, "-S", src, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_C_COMPILER={self.tc.cc}", f"-DCMAKE_CXX_COMPILER={self.tc.cxx}",
                f"-DCMAKE_MAKE_PROGRAM={self.tc.ninja}", f"-DCMAKE_PREFIX_PATH={self.sdk_install}",
                *self._extra_args()]

    def _translate_and_compile(self, configure: Callable[[Path, Path], list], build_name: str, label: str) -> \
            Dict[str, Path]:
        """Configure, codegen (each program), configure again (the generated source list), compile; for each
        project. Returns each program's executable."""
        assert self.tc
        env = self.tc.env
        exes: Dict[str, Path] = {}
        for project, progs in self._projects():
            src = self._project_dir(project)
            build = src / "out" / "build" / build_name
            if build_name == "release":
                self._forget_other_compiler(build)   # a folder configured with another compiler starts over
            self.runner.run(configure(src, build), cwd=src, env=env)
            for prog in progs:
                self.log.info(f"Translating {prog.name} ({prog.xex}) to C++...")
                self.runner.run([self.tc.cmake, "--build", build, "--target", prog.codegen_target], cwd=src, env=env,
                                quiet=True, line_filter=self._ninja_progress(f"{prog.name} codegen"))
            self.runner.run(configure(src, build), cwd=src, env=env)
            self.log.info(f"Compiling {', '.join(q.name for q in progs)} ({label}) with {self.o.jobs} threads...")
            self.runner.run([self.tc.cmake, "--build", build, "--parallel", str(self.o.jobs)], cwd=src, env=env,
                            quiet=True, line_filter=self._ninja_progress(f"{self.port.id} ({label})"))
            for prog in progs:
                windows = build_name == "mingw" or IS_WINDOWS
                name = prog.name + (".exe" if windows else "")
                found = [c for c in (build / name, build / "mp" / name) if c.is_file()]
                if not found:
                    raise SetupError(f"The build finished but {name} was not made; see the log.")
                exes[prog.name] = found[0]
        return exes

    def port_build(self) -> None:
        self._copy_port()
        exes = self._translate_and_compile(self._configure_native, "release", "Windows" if IS_WINDOWS else "Linux")
        bindir = self.root / "bin"
        if IS_WINDOWS:
            search = sorted({e.parent for e in exes.values()}) + [self.sdk_install / "lib", self.sdk_install / "bin"]
            search += binaries.vc_runtime_dirs(self.tc.vs_path if self.tc else None)
            start = list(exes.values()) + sorted(d for e in exes.values() for d in e.parent.glob("*.dll"))
            is_system = binaries.windows_is_system_dll
        else:
            search = sorted({e.parent for e in exes.values()}) + [self.sdk_install / "lib", self.sdk_install / "bin"]
            start = list(exes.values()) + sorted(d for e in exes.values() for d in e.parent.glob("*.so"))
            is_system = binaries.linux_system_libraries().__contains__
        start = self._with_gpu_plugin(_unique(start), search, IS_WINDOWS)
        files, missing = binaries.gather(start, search, is_system)
        if IS_WINDOWS and missing:
            extra, missing = binaries.windows_runtime_from_system(missing)
            files += extra
        if missing:
            raise SetupError(f"{self.port.name} needs libraries that aren't on this computer or in the build:\n" +
                             "\n".join(f"  {lib} (needed by {by})" for by, lib in missing))
        system = "windows" if IS_WINDOWS else "linux"
        self._pack(files, bindir, system, exes)

    def port_windows(self) -> None:
        """The Windows version, made on Linux with llvm-mingw against the SDK's Windows build, as cod3.exe is."""
        assert self.tc
        toolchain, sdk_win, toolchain_bin = self._windows_sdk()
        if not self.native:
            self._copy_port()   # the Linux version didn't copy it this time

        def configure(src: Path, build: Path) -> list:
            return [self.tc.cmake, "-S", src, "-B", build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={self.tc.ninja}",
                    *cross.game_configure_args(toolchain, sdk_win, self.sdk_install / "bin" / "rexglue"),
                    *self._extra_args()]
        exes = self._translate_and_compile(configure, "mingw", "Windows")
        search = sorted({e.parent for e in exes.values()}) + [sdk_win / "bin", cross.runtime_dll_folder(toolchain_bin)]
        start = list(exes.values()) + sorted(d for e in exes.values() for d in e.parent.glob("*.dll"))
        start = self._with_gpu_plugin(_unique(start), search, windows=True)
        files, missing = binaries.gather(start, search, cross.is_windows_system_dll)
        if missing:
            raise SetupError(f"{self.port.name} for Windows needs DLLs that are neither built with it nor part of "
                             "Windows: " + ", ".join(sorted({lib for _, lib in missing})))
        self._pack(files, self.root / "bin-windows", "windows", exes)

    def _pack(self, files: Sequence[Path], bindir: Path, system: str, exes: Dict[str, Path]) -> None:
        if bindir.exists():
            safe_rmtree(bindir, self.root, [q for q in self.protected if q != self.out], self.log)
        bindir.mkdir(parents=True)
        for f in files:
            try:
                shutil.copy2(f, bindir / f.name)
            except OSError as e:
                raise SetupError(f"Couldn't write {bindir / f.name} ({e}). Close {self.port.name} if it is "
                                 "running, then press Build again.") from None
        rel = bindir.relative_to(self.root).as_posix()
        self.exe_rels[system] = {name: f"{rel}/{path.name}" for name, path in exes.items()}
        self.files_rel[system] = [f"{rel}/{f.name}" for f in files]
        self.log.info(f"{self.port.name} ({system}): " + ", ".join(f.name for f in files))

    # ------------------------------------------------------------------ 4. finish
    def port_finish(self) -> None:
        p = self.port
        (self.root / "userdata").mkdir(exist_ok=True)
        if IS_LINUX and "linux" in self.exe_rels:
            for prog in p.programs:
                script = self.root / launcher_name(p, prog)
                if prog.name in self.exe_rels["linux"]:
                    script.write_text(linux_launcher(prog.name, appimage=False), encoding="utf-8")
                    script.chmod(0o755)
                elif script.exists():
                    script.unlink()
        manifest_path = self.root / p.manifest
        manifest = read_json(manifest_path)
        manifest.update({
            "game": p.id,
            "tool_version": TOOL_VERSION,
            "built": time.strftime("%Y-%m-%d %H:%M:%S"),
            "platform": host_triple(),
            "disc": {q.name: (self.disc.sha1.get(q.name, "") if self.disc else "") for q in p.programs},
            "programs": [q.name for q in self.programs],
        })
        for system in ("linux", "windows"):
            if system in self.exe_rels:
                manifest[f"exe_{system}"] = self.exe_rels[system]
                manifest[f"files_{system}"] = self.files_rel[system]
        write_json(manifest_path, manifest)
        where = ", ".join(f"{s}: {self.root / v[self.programs[0].name]}" for s, v in self.exe_rels.items())
        self.log.info(f"{p.name} is built ({where}). Start it from the {p.name} tab's Play page, or with the "
                      "launcher in its folder.")


def _unique(paths: Sequence[Path]) -> List[Path]:
    seen, out = set(), []
    for q in paths:
        if q.name not in seen:
            seen.add(q.name)
            out.append(q)
    return out


def launcher_name(p: Port, prog: Program) -> str:
    """The Linux launcher next to the game: 'Call of Duty 2.sh', 'Call of Duty 2 Multiplayer.sh'."""
    return f"{p.name.split(':')[0]}{' Multiplayer' if prog.label == 'Multiplayer' else ''}.sh"


def run_build(p: Port, source: str, opts: BuildOptions, log: Log, cancel: threading.Event,
              on_step: Optional[Callable[[int, int, str], None]] = None) -> str:
    """Builds one port; returns the line for the Done note."""
    pipe = PortPipeline(p, source, opts, log, cancel, on_step)
    pipe.run()
    return f"{p.name} is built in {pipe.root}."


# ---------------------------------------------------------------------- play
GRAPHICS_APIS = ("auto", "vulkan", "d3d12")


@dataclass
class PlayOptions:
    """The Play page's settings for one port: passed on the command line, so the game's own settings file is left
    as the player made it."""
    program: str = ""            # "" = the campaign
    fullscreen: bool = False
    mute: bool = False
    skip_films: bool = False
    fov: float = 0.0             # 0 = the game's own
    max_fps: int = -1            # -1 = the port's own default
    graphics_api: str = "auto"   # Windows
    extra: List[str] = field(default_factory=list)

    def problems(self, p: Port) -> List[str]:
        out = []
        if self.program and self.program not in [q.name for q in p.programs]:
            out.append(f"no program {self.program!r} in {p.name}")
        if self.fov and not (40.0 <= self.fov <= 140.0):
            out.append("the field of view goes from 40 to 140")
        if self.max_fps not in (-1,) and not (0 <= self.max_fps <= p.max_fps_top):
            out.append(f"the frame limit goes from 0 ({p.max_fps_zero}) to {p.max_fps_top}")
        if self.graphics_api not in GRAPHICS_APIS:
            out.append(f"the graphics API is one of {', '.join(GRAPHICS_APIS)}")
        return out

    def arguments(self, p: Port) -> List[str]:
        # [f2-menus] 2026-11-10 (owner: "check the cod3 tab for references. Also check for redundancy"): nothing is
        # passed for an unticked box. "--fullscreen=false" at every start undid the display mode chosen in the
        # game's F2 window, the fault Call of Duty 3's Play page had with its old "Start in a window" box.
        args = ["--fullscreen=true"] if self.fullscreen else []
        if self.mute:
            args.append("--audio_mute=true")
        if self.skip_films and p.skip_films:
            args.append(f"--{p.skip_films}=true")
        if self.fov and p.fov:
            args.append(f"--{p.fov}={round(self.fov)}" if p.fov_whole else f"--{p.fov}={self.fov:g}")
        if self.max_fps >= 0 and p.max_fps:
            args.append(f"--{p.max_fps}={self.max_fps}")
        if IS_WINDOWS and self.graphics_api != "auto":
            args.append(f"--{p.settings_prefix}_graphics_api={self.graphics_api}")
        return args + list(self.extra)

    def to_settings(self, p: Port) -> dict:
        k = f"port_{p.id}_"
        return {k + "program": self.program, k + "fullscreen": self.fullscreen, k + "mute": self.mute,
                k + "skip_films": self.skip_films, k + "fov": int(round(self.fov)), k + "max_fps": self.max_fps,
                k + "graphics_api": self.graphics_api}

    @classmethod
    def from_settings(cls, p: Port, s: dict) -> "PlayOptions":
        k = f"port_{p.id}_"
        o = cls()
        if isinstance(s.get(k + "program"), str):
            o.program = s[k + "program"]
        for name in ("fullscreen", "mute", "skip_films"):
            if isinstance(s.get(k + name), bool):
                setattr(o, name, s[k + name])
        if isinstance(s.get(k + "fov"), (int, float)) and not isinstance(s.get(k + "fov"), bool):
            o.fov = float(s[k + "fov"])
        if isinstance(s.get(k + "max_fps"), int) and not isinstance(s.get(k + "max_fps"), bool):
            o.max_fps = s[k + "max_fps"]
        if s.get(k + "graphics_api") in GRAPHICS_APIS:
            o.graphics_api = s[k + "graphics_api"]
        return o


# ---------------------------------------------------------------------- the game's own settings file
# [f2-menus] 2026-11-10 (owner 18:01: "check the cod3 tab for references. Also check for redundancy"; 18:13: "Your
# suggestions seem good."). The Play page's Settings rows are the game's own settings, in <program>.toml in the
# game's folder (cod2.toml, cod2mp.toml ...): the file the game reads at start and its F2 window writes. One copy, as
# Call of Duty 3's Game settings; nothing of them is passed on the command line from the Play page any more.
def settings_file_name(p: Port, program: str = "") -> str:
    return f"{program or p.programs[0].name}.toml"


def game_settings_of(p: Port) -> List["game_settings.Setting"]:
    """The settings the Play page has a row for, with each one's own name, range and default in that port."""
    setting = game_settings.Setting
    out = [setting("fullscreen", "Fullscreen", "Fill the screen instead of a window.", "bool", default=True),
           setting("audio_mute", "Mute", "No sound.", "bool", default=False)]
    if p.skip_films:
        out.append(setting(p.skip_films, "Skip the start-up films", "Straight to Press START.", "bool", default=False))
    if p.fov:
        out.append(setting(p.fov, "Field of view", "Degrees; 0 = the game's own.", "int", min=0, max=160, default=0,
                           decimal=not p.fov_whole))
    if p.max_fps:
        out.append(setting(p.max_fps, "Frame limit", f"Frames a second; 0 = {p.max_fps_zero}.", "int", min=0,
                           max=p.max_fps_top, default=p.max_fps_default))
    out.append(setting(f"{p.settings_prefix}_graphics_api", "Graphics API",
                       "auto: Vulkan, or Direct3D 12 when Vulkan can't start.", "choice",
                       tuple((a, a) for a in GRAPHICS_APIS), default="auto", platforms=("windows",)))
    return out


def load_game_settings(p: Port, output: Path, program: str = "") -> Tuple[dict, set]:
    """(each setting's value in <output>/<game>/<program>.toml, or its default; the settings the file has a line
    for)."""
    folder, name, settings = Path(output) / p.subdir, settings_file_name(p, program), game_settings_of(p)
    found = game_settings.read_values(folder, name)
    return (game_settings.load(folder, settings=settings, file_name=name),
            {s.key for s in settings if any(n in found for n in s.names)})


def save_game_settings(p: Port, output: Path, values: dict, program: str = "") -> None:
    """Change only `values` in that file, the rest of it kept byte for byte; SetupError while the game runs."""
    game_settings.save(Path(output) / p.subdir, values, settings=game_settings_of(p),
                       file_name=settings_file_name(p, program),
                       exes=[n for q in p.programs for n in (q.name, q.name + ".exe")])


def installed(p: Port, output: Path) -> Optional[dict]:
    """The port's manifest in <output>/<game>, when a build finished there."""
    m = read_json(Path(output) / p.subdir / p.manifest)
    return m if m.get("game") == p.id else None


def play_command(p: Port, output: Path, opts: PlayOptions) -> Tuple[Optional[List[str]], str]:
    """(the command that starts the chosen program, "" ) or (None, why it can't start)."""
    m = installed(p, output)
    if not m:
        return None, f"Build {p.name} first (its Set up page)."
    system = "windows" if IS_WINDOWS else "linux"
    exes = m.get(f"exe_{system}") or {}
    prog = opts.program or p.programs[0].name
    rel = exes.get(prog) if isinstance(exes, dict) else None
    if not rel:
        built = ", ".join(exes) if isinstance(exes, dict) and exes else "nothing"
        return None, (f"{p.name}'s {'Windows' if IS_WINDOWS else 'Linux'} version of {prog} isn't built "
                      f"(built: {built}).")
    exe = Path(output) / p.subdir / rel
    if not exe.is_file():
        return None, f"{exe} is gone; build {p.name} again."
    problems = opts.problems(p)
    if problems:
        return None, "; ".join(problems) + "."
    return [str(exe), *opts.arguments(p)], ""


def last_build(p: Port, output: Path) -> str:
    m = installed(p, output)
    if not m:
        return "Not built yet."
    progs = ", ".join(m.get("programs") or [])
    return f"Built {m.get('built', '?')} with setup tool {m.get('tool_version', '?')}: {progs}."


__all__ = ["Program", "Port", "PORTS", "PORT_ORDER", "COD2", "WAW", "COD4", "port", "Disc", "read_disc",
           "check_disc", "buildable", "PortPipeline", "run_build", "PlayOptions", "play_command", "installed",
           "last_build", "launcher_name", "GRAPHICS_APIS", "settings_file_name", "game_settings_of",
           "load_game_settings", "save_game_settings"]

#!/usr/bin/env python3
"""COD_3_WAW Setup tool. From your own copies of Call of Duty 3 (Xbox 360) and
World at War, it rips the games' files for the native engine and builds the
playable Call of Duty 3 recompilation. It installs whatever is missing.

    python3 cod3_waw_setup.py                 # GUI
    python3 cod3_waw_setup.py --launcher      # GUI, opened on the Play page
    python3 cod3_waw_setup.py build            # everything from input/ (and WaW on Steam)
    python3 cod3_waw_setup.py build --iso "Call of Duty 3.iso" --waw-steam "<WaW Steam folder>"
    python3 cod3_waw_setup.py doctor [--fix]  # what this PC has; --fix installs what's missing
    python3 cod3_waw_setup.py xiso "<disc image>" [--extract <folder> --only "*.ff"]
    python3 cod3_waw_setup.py map-pack "<patch download>" [--extract <folder>]

Versions: by default the one for this system. --linux and --windows pick
which to build, on either system: Linux builds the Windows version with
llvm-mingw, Windows builds the Linux version inside WSL 2.

World at War sources: a Steam folder, when given, is the only one used.
Without one, use --waw-xbox360 <disc image> or --waw-pc-disc <image, drive or
installed folder>. World at War itself is never run.

The discs lack the map packs (DLC). Add them with --waw-map-pack <official PC
patch download> (repeatable); `map-pack --list` shows where to get them.

Run `setup.sh` (Linux) or `setup.bat` (Windows) instead if Python/Tkinter
might be missing; they install them first.
"""
from __future__ import annotations

import argparse
import fnmatch
import os
import sys
import threading
from pathlib import Path

if sys.version_info < (3, 9):
    sys.exit("Python 3.9 or newer is required.")

sys.path.insert(0, str(Path(__file__).resolve().parent))

from c3w import common, deps, inputs, map_packs, pc_check, rippers, upscale, waw  # noqa: E402
from c3w.pipeline import KNOWN_XEX, BuildOptions, Pipeline  # noqa: E402
from c3w.xiso import XisoImage  # noqa: E402


def _cli_log() -> common.Log:
    return common.Log(sinks=[common.console_sink])


def _cli_approval(yes: bool):
    """Asks in the terminal before anything is downloaded or installed (the
    whole list, with where each thing goes). --yes approves it up front."""
    def ask(items) -> bool:
        print("\nThese will be downloaded and installed:")
        for i in items:
            print(f"  - {i.what}" + (f" ({i.size})" if i.size else ""))
            print(f"      from: {i.source or 'see above'}")
            print(f"      into: {i.where}")
        if yes:
            print("Approved with --yes.\n")
            return True
        if sys.stdin is None or not sys.stdin.isatty():
            print("Nothing was downloaded. Run again with --yes to approve, or in a terminal to be asked.")
            return False
        try:
            return input("Download and install these? [y/N] ").strip().lower() in ("y", "yes")
        except EOFError:
            return False
    return ask


def cmd_build(a: argparse.Namespace) -> int:
    steam, xbox360, pc_disc = a.waw_steam or "", a.waw_xbox360 or "", a.waw_pc_disc or ""
    # What isn't given is looked for in the input folder.
    found = inputs.scan(common.default_input_dir(), KNOWN_XEX)
    iso = a.iso or (str(found.cod3_iso) if found.cod3_iso else "")
    if not iso:
        print(f"Give the Call of Duty 3 disc image with --iso, or put it in {common.default_input_dir()}")
        return 2
    if not a.iso:
        print(f"Using the Call of Duty 3 disc image in the input folder: {iso}")
    if a.waw:
        kind = waw.guess_kind(Path(a.waw))
        steam = a.waw if kind == waw.STEAM else steam
        xbox360 = a.waw if kind == waw.XBOX360 else xbox360
        pc_disc = a.waw if kind == waw.PC_DISC else pc_disc
    if not steam and not xbox360 and not pc_disc and not a.no_waw:
        on_steam = waw.find_steam_waw()
        if on_steam:
            print(f"Found World at War on Steam: {on_steam} (use --no-waw to leave it out)")
            steam = str(on_steam)
        elif found.waw_xbox360:
            print(f"Using the World at War Xbox 360 disc image in the input folder: {found.waw_xbox360}")
            xbox360 = str(found.waw_xbox360)
    if a.no_waw:   # World at War left out, even if a source was given
        steam = xbox360 = pc_disc = ""
    if not steam and not xbox360 and not pc_disc:
        print("World at War: not used (it's optional; Call of Duty 3 builds and plays without it).")
    packs = [] if a.no_waw else (a.waw_map_pack or [])
    if not packs and not steam and (xbox360 or pc_disc) and found.map_packs:
        packs = [str(p) for p in found.map_packs]
        print("Using the map pack downloads in the input folder: " + ", ".join(Path(p).name for p in packs))
    # Which versions: --linux / --windows pick exactly those; neither means
    # this system's own (plus Windows with the older --windows-exe on Linux).
    if a.linux or a.windows:
        build_linux, build_windows = a.linux, a.windows or bool(a.windows_exe)
    else:
        build_linux = common.IS_LINUX
        build_windows = common.IS_WINDOWS or bool(a.windows_exe)
    jobs = a.jobs or common.recommended_jobs()
    if a.binaries_only:
        # The build inside WSL: the job count came from Windows, but this
        # virtual machine usually has about half the RAM; never go above what
        # fits here. Progress goes out as whole lines, which the Windows side
        # reads to show it.
        jobs = min(jobs, common.recommended_jobs())
        common.set_console_line_mode(True)
    opts = BuildOptions(
        cod3_source=iso, waw_steam=steam, waw_xbox360=xbox360, waw_pc_disc=pc_disc,
        waw_alternative=waw.PC_DISC if pc_disc and not xbox360 else waw.XBOX360,
        waw_map_packs=packs,
        output_dir=a.output, work_dir=a.work or str(common.default_work_dir()),
        jobs=jobs, build_recomp=not a.no_recomp, appimage=not a.no_appimage,
        cleanup=not a.no_cleanup, keep_sdk=a.keep_sdk, install_deps=not a.no_install_deps,
        fidelityfx=not a.no_fidelityfx, build_linux=build_linux, build_windows=build_windows,
        binaries_only=a.binaries_only, force_unknown_iso=a.force, skip_dep_check=a.skip_dep_check,
        upscale_textures=a.upscale_textures, upscale_model=a.upscale_model, upscale_noise=a.upscale_noise,
        upscale_scale=a.upscale_scale)
    log = _cli_log()
    cancel = threading.Event()
    common.APPROVAL.ask = _cli_approval(a.yes)
    try:
        Pipeline(opts, log, cancel).run()
    except KeyboardInterrupt:
        cancel.set()
        log.error("Canceled. Run the same command again to continue where it stopped.")
        return 130
    except common.Cancelled:
        log.error("Canceled.")
        return 130
    except common.SetupError as e:
        log.error(str(e))
        return 1
    except OSError as e:
        if not common.IS_WINDOWS:
            raise
        # Windows: a file the build had to change was open somewhere.
        log.error(f"{e}\n{pc_check.failure_hint([str(e)]) or 'See the log above.'}")
        return 1
    print()
    return 0


def cmd_add_map(a: argparse.Namespace) -> int:
    log = _cli_log()
    cancel = threading.Event()
    out = Path(a.output)
    runner = common.Runner(log, cancel)
    work = Path(a.work or common.default_work_dir())
    tc = deps.detect(work / "tools", runner)
    try:
        maps = waw.install_custom_map(Path(a.source), out, work / "scratch",
                                      waw.Unpackers(tc.sevenzip, tc.innoextract, tc.unar), runner, log, cancel)
        rip = out / "tools" / common.exe_name("rip")
        if rip.is_file():
            for m in maps:
                _, errors = rippers.rip_fastfiles(rip, m.fastfiles, m.folder / "ripped", runner, log)
                for e in errors:
                    log.warn(e)
    except common.SetupError as e:
        log.error(str(e))
        return 1
    print()
    return 0


def cmd_doctor(a: argparse.Namespace) -> int:
    log = _cli_log()
    runner = common.Runner(log, threading.Event())
    work = Path(a.work or common.default_work_dir())
    recomp = not a.no_recomp
    if a.fix:
        common.APPROVAL.ask = _cli_approval(a.yes)
        try:
            tc = deps.ensure(work / "tools", runner, log, threading.Event(), True, recomp=recomp)
        except common.SetupError as e:
            log.error(str(e))
            return 1
    else:
        tc = deps.detect(work / "tools", runner)
    print(f"\nPlatform     {common.host_triple()}  ({common.cpu_count()} CPUs, {common.total_ram_gb():.1f} GB RAM, "
          f"{common.recommended_jobs()} build threads recommended)")
    print(f"Work folder  {work}  (temporary build files; tools/ in it is kept: downloaded tools, Rust)")
    print(f"Output       {common.default_output_dir()}  (default; the game, the rips, saves and logs)")
    print(f"Settings     {common.config_dir()}")
    steam = waw.find_steam_waw()
    print(f"WaW (Steam)  {steam or 'not found'}")
    print(tc.summary())
    # Why each missing piece is needed and how to get it, and what about this
    # PC can get in the way (OneDrive, the Store's Python, a proxy ...).
    notes = pc_check.tool_notes(tc, recomp=recomp) + pc_check.system_notes(work)
    if notes:
        print("\n" + "\n\n".join(n.text() for n in notes))
    problems = tc.problems(recomp=recomp)
    if not problems:
        print("\nReady.")
        return 0
    print("\nMissing: " + "; ".join(problems))
    print("Run `doctor --fix` (or just build) to install them." if not a.fix else "See the messages above.")
    return 1 if a.fix else 0


def cmd_map_pack(a: argparse.Namespace) -> int:
    """Describes a map pack download (an official PC patch), or takes its
    fastfiles and .iwd files out. Its installer is never run."""
    if a.list or not a.download:
        for p in map_packs.KNOWN_PATCHES:
            print(f"{p.title}: {', '.join(f'{n} ({z})' if z else n for n, z in p.maps)}\n"
                  f"  download {p.archive} ({common.human(p.archive_bytes)}, MD5 {p.archive_md5}) from\n  {p.page}")
        found = map_packs.find_downloads()
        print("\nDownloads found: " + (", ".join(str(f) for f in found) if found else "none"))
        return 0
    log = _cli_log()
    cancel = threading.Event()
    download = Path(a.download)
    try:
        if not a.extract:
            for line in map_packs.describe(download, cancel):
                print(line)
            return 0
        runner = common.Runner(log, cancel)
        work = Path(a.work or common.default_work_dir())
        tc = deps.detect(work / "tools", runner)
        unpackers = map_packs.Unpackers(tc.sevenzip, tc.unshield, tc.innoextract)
        files = map_packs.extract(download, Path(a.extract), work / "scratch", unpackers, runner, log, cancel)
        for f in files:
            print(f"{f.stat().st_size:12d}  {f}")
    except (common.SetupError, OSError) as e:
        log.error(str(e))
        return 1
    except KeyboardInterrupt:
        cancel.set()
        return 130
    return 0


def cmd_xiso(a: argparse.Namespace) -> int:
    """Lists an Xbox / Xbox 360 disc image, or copies files out of it. The
    image is only read."""
    log = _cli_log()
    try:
        with XisoImage(Path(a.image)) as img:
            wanted = [p.lower() for p in (a.only or [])]

            def chosen(rel: str) -> bool:
                # A pattern is a folder prefix ("zone/") or a wildcard ("*.ff").
                low = rel.lower()
                return not wanted or any(low.startswith(w) or fnmatch.fnmatch(low, w) for w in wanted)

            if not a.extract:
                files = [e for e in img.files if chosen(e.path)]
                print(f"{Path(a.image).name}: layout {img.kind}, {len(files)} files, "
                      f"{common.human(sum(e.size for e in files))}")
                for e in sorted(files, key=lambda e: e.path.lower()):
                    print(f"{e.size:12d}  {e.path}")
                return 0
            img.extract(Path(a.extract), log, threading.Event(), only=chosen)
    except (common.SetupError, OSError) as e:
        log.error(str(e))
        return 1
    print()
    return 0


def main() -> int:
    if common.IS_WINDOWS:
        # Output sent to a file or a pipe uses the ANSI code page, which can't
        # spell every folder name (a Cyrillic or Chinese user name): replace
        # such letters instead of stopping with an error.
        for stream in (sys.stdout, sys.stderr):
            try:
                stream.reconfigure(errors="replace")
            except (AttributeError, ValueError):
                pass
    ap = argparse.ArgumentParser(description="COD_3_WAW setup tool", formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--launcher", action="store_true", help="open the GUI on the Play page")
    sub = ap.add_subparsers(dest="cmd")

    b = sub.add_parser("build", help="set everything up without the GUI")
    b.add_argument("--iso", help="Call of Duty 3 (Xbox 360) .iso, or a folder extracted from it "
                                 "(default: the one in the input folder)")
    b.add_argument("--waw-steam", help="World at War Steam folder (when given, the only WaW source used)")
    b.add_argument("--waw-xbox360", help="World at War Xbox 360 disc image (used when there's no Steam folder)")
    b.add_argument("--waw-pc-disc", help="World at War PC disc: image, drive, or a folder installed from it")
    b.add_argument("--waw", help="any World at War source; its kind is detected")
    b.add_argument("--no-waw", action="store_true",
                   help="leave World at War out completely (Call of Duty 3 doesn't need it)")
    b.add_argument("--waw-map-pack", action="append", metavar="DOWNLOAD",
                   help="an official PC patch with map packs, for the discs (repeatable; see `map-pack --list`)")
    b.add_argument("--output", default=str(common.default_output_dir()),
                   help="where everything goes (default: output/ here)")
    b.add_argument("--work", help="temporary build folder (default: work/ here; deleted afterwards)")
    b.add_argument("--jobs", type=int, help="build threads (default: based on CPU cores and RAM)")
    b.add_argument("--no-recomp", action="store_true", help="only rip; don't build the playable recompiled CoD3")
    b.add_argument("--linux", action="store_true",
                   help="build the Linux version (AppImage); on Windows it is built inside WSL 2")
    b.add_argument("--windows", action="store_true",
                   help="build the Windows version (cod3.exe); on Linux it is cross-compiled with llvm-mingw")
    b.add_argument("--no-appimage", action="store_true", help="the Linux version as a plain folder, not an AppImage")
    b.add_argument("--no-cleanup", action="store_true", help="keep the work folder")
    b.add_argument("--keep-sdk", action="store_true", help="keep the built SDK for faster rebuilds (downloaded tools in work/tools are always kept)")
    b.add_argument("--no-install-deps", action="store_true", help="never install missing tools")
    b.add_argument("--no-fidelityfx", action="store_true",
                   help="Windows: build without AMD FSR/CAS support (Linux builds never include it)")
    b.add_argument("--windows-exe", action="store_true",
                   help="Linux: also build the Windows version and ripper (same as --linux --windows)")
    b.add_argument("--binaries-only", action="store_true", help=argparse.SUPPRESS)  # the build inside WSL
    b.add_argument("--force", action="store_true", help="continue with an unrecognized CoD3 version")
    b.add_argument("--skip-dep-check", action="store_true",
                   help="advanced: build even if the dependency check reports something missing")
    b.add_argument("--upscale-textures", action="store_true",
                   help="also upscale the ripped textures with waifu2x (uses the graphics card; see "
                        "--upscale-scale) and pack them for the playable game (data/cod3/hd_textures; "
                        "F2 > Video turns them off)")
    b.add_argument("--upscale-model", default=upscale.DEFAULT_MODEL, choices=sorted(upscale.MODELS),
                   help="waifu2x style: photo (default), art or cunet (best, slowest)")
    b.add_argument("--upscale-noise", type=int, default=upscale.DEFAULT_NOISE, choices=sorted(upscale.NOISE_LEVELS),
                   help="waifu2x noise reduction, -1 (none) to 3 (strong); default 1")
    b.add_argument("--upscale-scale", type=int, default=upscale.DEFAULT_SCALE, choices=sorted(upscale.SCALES),
                   help="how many times larger waifu2x makes the textures: 1 (noise reduction only, with the "
                        "cunet style), 2 (default) or 4 (slow; 16 times the texture memory)")
    b.add_argument("--yes", action="store_true",
                   help="approve the downloads and installs without asking (they are still listed)")
    b.set_defaults(func=cmd_build)

    if waw.MODS_ENABLED:   # mods and custom maps are switched off for now
        m = sub.add_parser("add-map", aliases=["install-map"],
                           help="add a WaW mod or custom map (installer .exe, .zip/.7z/.rar or folder) to the "
                                "output folder")
        m.add_argument("source")
        m.add_argument("--output", default=str(common.default_output_dir()), help="the output folder of a build")
        m.add_argument("--work", help=argparse.SUPPRESS)
        m.set_defaults(func=cmd_add_map)

    d = sub.add_parser("doctor", help="show what this PC has; --fix installs what's missing")
    d.add_argument("--fix", action="store_true", help="install what's missing (asks first)")
    d.add_argument("--yes", action="store_true", help="with --fix: approve the downloads without asking")
    d.add_argument("--no-recomp", action="store_true", help="only check what ripping needs")
    d.add_argument("--work", help=argparse.SUPPRESS)
    d.set_defaults(func=cmd_doctor)

    x = sub.add_parser("xiso", help="list or extract an Xbox / Xbox 360 disc image (read-only)")
    x.add_argument("image")
    x.add_argument("--extract", metavar="FOLDER", help="copy the files into this folder instead of listing them")
    x.add_argument("--only", action="append", metavar="PATTERN",
                   help='only these paths: a folder prefix like zone/ or a wildcard like "*.ff" (repeatable)')
    x.set_defaults(func=cmd_xiso)

    mp = sub.add_parser("map-pack", help="where to get the map packs; describe or unpack a download (read-only)")
    mp.add_argument("download", nargs="?", help="the downloaded patch (.zip or PatchSetup .exe)")
    mp.add_argument("--list", action="store_true", help="the known map pack downloads and where to get them")
    mp.add_argument("--extract", metavar="FOLDER", help="take its fastfiles and .iwd files out into this folder")
    mp.add_argument("--work", help=argparse.SUPPRESS)
    mp.set_defaults(func=cmd_map_pack)

    a = ap.parse_args()
    if a.cmd:
        return a.func(a)
    try:
        from c3w import gui
    except ImportError as e:
        if common.IS_WINDOWS:
            print(f"The window needs Tkinter ({e}), which this Python doesn't have. Run setup.bat, which installs "
                  "a Python with it into work\\tools\\python, or install Python from "
                  "https://www.python.org/downloads/ with \"tcl/tk and IDLE\" ticked. Or use the command line:\n")
        else:
            print(f"The window needs Tkinter ({e}). Run setup.sh, which installs it, or install it yourself "
                  "(Debian/Ubuntu/Mint: python3-tk, Fedora: python3-tkinter, Arch: tk), or use the command line:\n")
        ap.print_help()
        return 1
    if not common.IS_WINDOWS and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        print("No display found; use the command line instead:\n")
        ap.print_help()
        return 1
    return gui.run(launcher=a.launcher)


if __name__ == "__main__":
    sys.exit(main())

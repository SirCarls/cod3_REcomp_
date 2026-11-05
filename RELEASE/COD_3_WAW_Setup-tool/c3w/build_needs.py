"""[acct2-decomp] 2026-11-05 (owner 10:32: "also refresh the dependency checks and options offered"; his choices:
re-check on change, and review what's checked): what a build with the Set up page's options needs on this PC.

Worked out the way the build's own Tools step does it (Pipeline.install_plan, Toolchain.problems without Rust when
the build needs no Rust), so what the Set up page says, what "Check this PC" lists and the approval list at Build
agree. Nothing is installed or downloaded here. gui.py works it out again whenever an option that changes it
changes (the versions, the recomp, the AppImage, texture upscaling, Tracy, installing, the work folder)."""
from __future__ import annotations

import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

from . import deps, upscale
from .common import IS_LINUX, Log, Runner
from .pipeline import BuildOptions, Pipeline


# A "Check this PC" row's text for a tool Build downloads (the dialog shows it as "Not installed", not "Missing").
AT_BUILD = "downloaded when you press Build"


def short(what: str) -> str:
    """An approval entry's name without its note: "CMake 3.31.6 (build tool)" -> "CMake 3.31.6"."""
    return what.split(" (")[0].split(", ")[0].strip()


def _join(items: List[str]) -> str:
    return items[0] if len(items) == 1 else ", ".join(items[:-1]) + " and " + items[-1]


@dataclass
class Needs:
    recomp: bool = False            # the SDK and a compiled game are part of this build
    rust: bool = True               # the ripper is built here (no prebuilt one)
    problems: List[str] = field(default_factory=list)   # missing, in the toolchain's own words
    installs: List[str] = field(default_factory=list)   # what Build downloads or installs first (approval list)
    install_deps: bool = True
    # Tools only some options need, as "Check this PC" rows: (name, found, where / what happens).
    option_rows: List[Tuple[str, bool, str]] = field(default_factory=list)

    def line(self) -> str:
        """One line for the Set up page."""
        parts = []
        if self.installs:
            parts.append("Build first downloads or installs (you approve the list): " + _join(self.installs) + ".")
        if self.problems and not self.install_deps:
            parts.append("Missing for this build: " + _join(self.problems) + ". Tick \"Install anything that's "
                         "missing\", or install them yourself (Tools > Check this PC says how).")
        elif self.problems and not self.installs:
            parts.append("Missing for this build: " + _join(self.problems) + ". Tools > Check this PC says how "
                         "to get them.")
        return " ".join(parts) or "Everything this build needs is on this PC."


def _option_tools(p: Pipeline) -> List[Tuple[str, str]]:
    """(pinned download key, what it is for) for the tools the chosen options add."""
    out = []
    if p.cross_windows:
        out.append(("llvm-mingw-linux", "llvm-mingw (compiles the Windows version here)"))
    if p.native and IS_LINUX and p.o.appimage:
        out.append(("appimagetool", "appimagetool (makes the Linux AppImage)"))
    if p.o.upscale_textures and not p.o.binaries_only:
        out.append((upscale.portable_key(), "waifu2x (texture upscaling)"))
    return out


def check(opts: BuildOptions, tc: Optional[deps.Toolchain] = None, runner: Optional[Runner] = None) -> Needs:
    """What a build with `opts` needs here. `tc`: a toolchain already detected (else it is detected now)."""
    log = Log(sinks=[])
    p = Pipeline(opts, log, threading.Event())
    if runner is not None:
        p.runner = runner
    recomp = p.native or p.cross_windows
    rust = p._needs_rust
    tc = tc or deps.detect(p.tools_dir, p.runner)
    problems = tc.problems(recomp=recomp)
    if not rust:
        problems = [x for x in problems if x not in tc.rust_problems()]
    installs = list(dict.fromkeys(short(i.what) for i in p.install_plan(recomp)))
    rows = []
    for key, what in _option_tools(p):
        found = deps._portable_ok(p.tools_dir, key)
        rows.append((what, found, str(Path(p.work_real) / "tools" / key) if found else
                     AT_BUILD + " (you approve it first)"))
    return Needs(recomp=recomp, rust=rust, problems=problems, installs=installs, install_deps=opts.install_deps,
                 option_rows=rows)

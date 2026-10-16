#!/usr/bin/env python3
"""Makes RELEASE/: everything a new user needs to start, and nothing else.

    python3 tools/make_release.py

RELEASE/ (next to tools/) gets the files git would publish: the setup tool
with the recomp template and the SDK patches, the Rust engine's source, the
docs, the tools, the licence and the start scripts, plus START-HERE.txt.
It never contains game files or anything extracted from a game: the same
check as tools/check_repo.py runs over the copy, and the script fails if it
finds game data, an oversized file or a personal path.

Not copied: dev/, input/, output/, work/, config/ (your own builds, discs, logs, settings),
build output (engine/target, __pycache__), the recomp's generated C++, and
RELEASE/ itself. Run it again after every change; it starts the folder over.
"""
from __future__ import annotations

import fnmatch
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RELEASE = ROOT / "RELEASE"
sys.path.insert(0, str(ROOT / "tools"))
import check_repo  # noqa: E402

# Never copied, at any depth (besides what .gitignore keeps out of git).
SKIP_DIRS = {".git", "target", "__pycache__", "RELEASE", "Claude outputs", ".vscode", ".idea"} | \
    check_repo.FORBIDDEN_DIRS
SKIP_FILES = {"*.pyc", "*.swp", ".DS_Store", "Thumbs.db", "desktop.ini", "*.AppImage", "*.iso", "*.zip",
              "*.rar"}
# Made by the setup tool on each machine.
SKIP_PATHS = ["config/*",   # the setup tool's own settings (not recomp/config/, which is source)
              "COD_3_WAW_Setup-tool/recomp/generated/*", "COD_3_WAW_Setup-tool/recomp/assets/*",
              "COD_3_WAW_Setup-tool/recomp/out/*", "engine/bin/*"]
KEEP_PATHS = ["COD_3_WAW_Setup-tool/recomp/generated/rexglue.cmake"]

START_HERE = """Call of Duty 3 (Xbox 360) for PC, with World at War content
==============================================================

This folder is the whole project, without any game files. You bring your
own game:

  - your Call of Duty 3 disc image (Xbox 360, .iso), required;
  - your Call of Duty: World at War installation, optional.

Start:

  Windows:  double-click setup.bat
  Linux:    sh setup.sh

The setup tool (COD3-setup-tool) asks for your disc image and an output
folder. Before it downloads or installs anything, it lists each item, where
it comes from and the folder it goes into, and waits for your approval.
Then it extracts the game data from your disc and builds the playable game
in the output folder. It takes 30 to 120 minutes the first time.

Everything it makes stays in this folder: input/ (your discs), output/
(the game, saves, logs), work/ (build files; work/tools keeps the
downloaded tools) and config/ (its settings). Only Linux system packages
and, on Windows, Visual Studio's Build Tools go where their own installers
put them. "Where things go" in the setup tool lists every folder.

Read README.md for the details, docs/ for how everything works, and
LICENSE for the terms. Nothing in this folder comes from a game disc.
"""


def files_to_copy() -> list[Path]:
    try:
        out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
        tracked = [ROOT / p for p in out.decode("utf-8", "replace").split("\0") if p]
    except (OSError, subprocess.CalledProcessError):
        tracked = [p for p in ROOT.rglob("*") if p.is_file()]
    picked = []
    for path in tracked:
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        posix = rel.as_posix()
        if set(rel.parts[:-1]) & SKIP_DIRS or rel.parts[0] in SKIP_DIRS:
            continue
        if any(fnmatch.fnmatch(rel.name, pat) for pat in SKIP_FILES):
            continue
        if any(fnmatch.fnmatch(posix, pat) for pat in SKIP_PATHS) and posix not in KEEP_PATHS:
            continue
        picked.append(path)
    return sorted(picked)


def main() -> int:
    files = files_to_copy()
    if RELEASE.exists():
        shutil.rmtree(RELEASE)
    for src in files:
        dst = RELEASE / src.relative_to(ROOT)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    (RELEASE / "START-HERE.txt").write_text(START_HERE, encoding="utf-8")
    copied = [p for p in RELEASE.rglob("*") if p.is_file()]
    problems = check_repo.check_files(RELEASE, copied)
    if problems:
        print("RELEASE/ would contain things that must not be published:")
        for p in problems:
            print("  " + p)
        return 1
    size = sum(p.stat().st_size for p in copied)
    print(f"RELEASE/: {len(copied)} files, {size / (1024 * 1024):.1f} MB, no game data or personal paths.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

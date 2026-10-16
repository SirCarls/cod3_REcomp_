#!/usr/bin/env python3
"""Fails if the repository holds anything that looks like game data.

The project ships no Activision files: everything game-specific is made on
each user's machine from their own discs. This check runs in CI (and can be
run before a commit) over the files git would publish:

    python3 tools/check_repo.py            # files tracked by git (or all files, outside git)

It flags:
- game file types (.xex, .cod, .wbk, .ff, .iwd, .iso, ...);
- files whose first bytes are a known game or disc format;
- any file over 5 MB (source code and docs are far smaller);
- personal paths in text files: a user's home or mounted drives
  (/home/<name>/, /media/<name>/, /Users/<name>/, C:\\Users\\<name>\\, /mnt/<drive>/).
  Write $HOME, %USERPROFILE% or <your folder> instead.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAX_BYTES = 5 * 1024 * 1024
GAME_EXTENSIONS = {".xex", ".cod", ".wbk", ".ff", ".iwd", ".iso", ".xiso", ".god", ".stfs", ".bik",
                   ".xma", ".xpr", ".xbe"}
# Magic numbers at the start of a file.
GAME_MAGIC = {
    b"XEX2": "Xbox 360 executable",
    b"KAPF": "Call of Duty 3 archive",
    b"APKF": "Call of Duty 3 chunk",
    b"IWff": "Call of Duty fastfile",
    b"NCH\0": "Call of Duty 3 packed block",
}
# Folders that never belong in the repository (ignored by .gitignore too).
FORBIDDEN_DIRS = {"input", "output", "work", "dev", "assets", "ripped", "logs", "build", "data", "launcher",
                  "userdata"}
# Files that may contain the magic strings as data (tests, docs, this file).
ALLOWED = {"tools/check_repo.py"}
# Absolute paths that name a person or their drives. Generic placeholders
# (<user>, user, runner for CI) are fine.
PERSONAL_PATH = re.compile(
    r"(/home/|/media/|/Users/|[A-Za-z]:\\\\?Users\\\\?)(?!<|user\b|runner\b|USERNAME\b)[A-Za-z0-9._-]+"
    r"|/mnt/(?!<|c/)[A-Za-z0-9._-]+/")
TEXT_SUFFIXES = {".md", ".txt", ".py", ".rs", ".toml", ".sh", ".bat", ".ps1", ".yml", ".yaml", ".json",
                 ".cpp", ".h", ".hpp", ".c", ".cmake", ".patch", ""}


def candidate_files() -> list[Path]:
    try:
        out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
        return [ROOT / p for p in out.decode("utf-8", "replace").split("\0") if p]
    except (OSError, subprocess.CalledProcessError):
        skip = {".git", "target", "__pycache__", "RELEASE", "Claude outputs"} | FORBIDDEN_DIRS
        top_only = {"config"}   # the setup tool's settings (recomp/config/ is source)
        return [p for p in ROOT.rglob("*") if p.is_file() and not (set(p.relative_to(ROOT).parts) & skip)
                and p.relative_to(ROOT).parts[0] not in top_only]


def check_files(root: Path, files: list[Path]) -> list[str]:
    """Problems with these files (game data, oversized files, personal paths)."""
    problems = []
    for path in files:
        rel = path.relative_to(root).as_posix()
        if rel in ALLOWED or rel.endswith("/tools/check_repo.py") or not path.is_file():
            continue
        if path.relative_to(root).parts[0] in FORBIDDEN_DIRS:
            problems.append(f"{rel}: lives in {path.relative_to(root).parts[0]}/, which holds game data")
            continue
        if path.suffix.lower() in GAME_EXTENSIONS:
            problems.append(f"{rel}: a game file type ({path.suffix})")
            continue
        size = path.stat().st_size
        if size > MAX_BYTES:
            problems.append(f"{rel}: {size // (1024 * 1024)} MB, too big for source code")
            continue
        with open(path, "rb") as f:
            head = f.read(4)
        if head in GAME_MAGIC:
            problems.append(f"{rel}: starts like a {GAME_MAGIC[head]}")
            continue
        if path.suffix.lower() in TEXT_SUFFIXES:
            text = path.read_text(encoding="utf-8", errors="replace")
            for n, line in enumerate(text.splitlines(), 1):
                m = PERSONAL_PATH.search(line)
                if m:
                    problems.append(f"{rel}:{n}: personal path {m.group(0)!r}")
    return problems


def main() -> int:
    problems = check_files(ROOT, candidate_files())
    if problems:
        print("Game data, oversized files or personal paths found; they must not be published:")
        for p in problems:
            print("  " + p)
        return 1
    print("No game data or personal paths found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

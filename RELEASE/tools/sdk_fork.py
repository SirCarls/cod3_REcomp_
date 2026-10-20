#!/usr/bin/env python3
"""The project's ReXGlue SDK fork and the patch files, kept in step.

The setup tool builds the SDK from the project's fork (branch cod3) at the
commit COD_3_WAW_Setup-tool/patches/fork.json names, plus any patch file that
commit doesn't hold yet. When the fork can't be fetched, or a patch file it
holds has changed, it builds from the official SDK commit plus every patch
file instead. Both routes must give the same tree; this tool checks that and
moves the pin.

    python3 tools/sdk_fork.py status
        What the setup tool would do with the patch files as they are now
        (no git, no network).

    python3 tools/sdk_fork.py verify --repo PATH [--commit SHA] [--hold all|NNNN]
        In a git repository that has both the official commit and the fork
        commit (a clone of the fork that also fetched the official repo, say),
        applies the patch files the fork commit should hold to the official
        commit in a temporary worktree, edits the resolve shaders like the
        setup tool, and compares the result with the fork commit. PATH's own
        working tree is never touched. --hold NNNN: only the patch files
        numbered up to NNNN (the default is every file in patches/).

    python3 tools/sdk_fork.py pin --repo PATH --commit SHA [--hold all|NNNN] [--fork-url URL]
        verify, then rewrite patches/fork.json for that commit.

    python3 tools/sdk_fork.py export --repo PATH --commit SHA --name NNNN-short-name
        Writes one fork commit as patches/NNNN-short-name.patch, so the
        backup route (official SDK + patch files) has it too.

Windows-only patches (patches/mingw) are never in the fork's cod3 branch: the
Windows version's build applies them on top, on either route.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "COD_3_WAW_Setup-tool"
sys.path.insert(0, str(TOOL))

from c3w import pipeline, sdk_shaders  # noqa: E402

PATCHES = TOOL / "patches"
DEFAULT_FORK_URL = "https://github.com/SirCarls/COD3-rexglue-sdk.git"


def git(repo: Path, *args: str, capture: bool = False) -> str:
    r = subprocess.run(["git", "-C", str(repo), *args], check=True, text=True,
                       stdout=subprocess.PIPE if capture else None)
    return (r.stdout or "").strip()


def held_patches(hold: str, patches_dir: Path = PATCHES):
    """The main-list patch files a fork commit holds: all, or those numbered
    up to NNNN."""
    files = pipeline.patch_files(patches_dir, cross_windows=False)
    if hold == "all":
        return files
    if not re.fullmatch(r"\d{4}", hold):
        raise SystemExit("--hold takes 'all' or a four-digit patch number")
    return [p for p in files if p.name[:4] <= hold]


def tree_of(repo: Path, patches, commit_base: str) -> str:
    """The tree id of the official commit with `patches` applied and the
    resolve shaders edited, made in a temporary worktree."""
    tmp = Path(tempfile.mkdtemp(prefix="sdk-fork-"))
    wt = tmp / "tree"
    try:
        git(repo, "worktree", "add", "-q", "--detach", str(wt), commit_base)
        scratch = tmp / "lf"
        for p in patches:
            git(wt, "apply", "--whitespace=nowarn", str(pipeline.lf_patch(p, scratch)))
        sdk_shaders.edit_sdk(wt)
        git(wt, "add", "-A")
        return git(wt, "write-tree", capture=True)
    finally:
        subprocess.run(["git", "-C", str(repo), "worktree", "remove", "--force", str(wt)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(tmp, ignore_errors=True)


def verify(repo: Path, commit: str, hold: str) -> bool:
    patches = held_patches(hold)
    want = git(repo, "rev-parse", f"{commit}^{{tree}}", capture=True)
    got = tree_of(repo, patches, pipeline.SDK_COMMIT)
    if got == want:
        print(f"OK: the official {pipeline.SDK_COMMIT[:9]} + {len(patches)} patch files "
              f"({patches[0].name[:4]}-{patches[-1].name[:4]}) + the shader edit = fork {commit[:9]} (tree {want[:9]})")
        return True
    names = git(repo, "diff-tree", "-r", "--name-only", got, want, capture=True)
    print(f"DIFFERENT: official + patch files gives tree {got[:9]}, fork {commit[:9]} has {want[:9]}. Files:")
    print("  " + "\n  ".join(names.splitlines()[:50]))
    return False


def cmd_status(_args) -> int:
    src = pipeline.plan_sdk_source(PATCHES, cross_windows=False, shaders_version=sdk_shaders.version())
    if src.fork_commit:
        print(f"Builds use the fork {src.fork_repo} at {src.fork_commit}")
        if src.fork_patches:
            print("  plus newer patch files: " + ", ".join(p.name for p in src.fork_patches))
        print(f"  (backup: the official {pipeline.SDK_COMMIT[:9]} + {len(src.official_patches)} patch files)")
    else:
        print(f"Builds use the official {pipeline.SDK_COMMIT[:9]} + {len(src.official_patches)} patch files: "
              f"{src.why_not_fork}")
    return 0


def cmd_verify(args) -> int:
    commit = args.commit or pipeline.read_fork_file(PATCHES)["commit"]
    return 0 if verify(Path(args.repo), commit, args.hold) else 1


def cmd_pin(args) -> int:
    repo = Path(args.repo)
    commit = git(repo, "rev-parse", f"{args.commit}^{{commit}}", capture=True)
    if not verify(repo, commit, args.hold):
        return 1
    data = {
        "repo": args.fork_url,
        "branch": "cod3",
        "commit": commit,
        "base": pipeline.SDK_COMMIT,
        "shaders": sdk_shaders.version(),
        "patches": {p.relative_to(PATCHES).as_posix(): pipeline.patch_hash(p) for p in held_patches(args.hold)},
    }
    if not pipeline._GITHUB_REPO_URL.match(data["repo"]):
        raise SystemExit(f"--fork-url must look like https://github.com/OWNER/REPO.git, not {data['repo']}")
    path = PATCHES / pipeline.FORK_FILE
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Wrote {path.relative_to(ROOT)}: {commit[:9]} holds {len(data['patches'])} patch files")
    return 0


def cmd_export(args) -> int:
    if not re.fullmatch(r"\d{4}-[a-z0-9][a-z0-9-]*", args.name):
        raise SystemExit("--name looks like 0065-short-name-in-lowercase")
    out = PATCHES / f"{args.name}.patch"
    if out.exists():
        raise SystemExit(f"{out.relative_to(ROOT)} exists already")
    diff = subprocess.run(["git", "-C", args.repo, "diff", "--no-color", "--no-ext-diff", f"{args.commit}^",
                           args.commit], check=True, capture_output=True).stdout
    if not diff:
        raise SystemExit("that commit changes nothing")
    out.write_bytes(diff)
    print(f"Wrote {out.relative_to(ROOT)} ({len(diff)} bytes)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    for name in ("verify", "pin"):
        s = sub.add_parser(name)
        s.add_argument("--repo", required=True)
        s.add_argument("--commit", required=(name == "pin"))
        s.add_argument("--hold", default="all")
        if name == "pin":
            s.add_argument("--fork-url", default=DEFAULT_FORK_URL)
    s = sub.add_parser("export")
    s.add_argument("--repo", required=True)
    s.add_argument("--commit", required=True)
    s.add_argument("--name", required=True)
    args = ap.parse_args(argv)
    return {"status": cmd_status, "verify": cmd_verify, "pin": cmd_pin, "export": cmd_export}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())

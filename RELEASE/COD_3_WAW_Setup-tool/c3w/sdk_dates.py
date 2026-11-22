"""[waw-decomp] 2026-11-16 SDK source files keep their dates when their bytes did not change.

Every SDK build starts from a clean checkout (pipeline._checkout_sdk: `git checkout --force`, `git clean`, the patch
files applied again, c3w/sdk_shaders.py's resolve shaders edited again). That writes every patched file anew, so each
one got today's date even when its bytes were the same as before. Ninja and CMake's install go by dates: the SDK
recompiled everything that includes those files, its install copied the headers again with the new dates, and the
game was rebuilt from scratch (one of those headers is in the generated code's precompiled header). The Linux and
the Windows (llvm-mingw) SDK share one checkout, so each one's checkout also re-dated what the other had built from.

What this module does, for each SDK flavor (the native one, named after its CMake preset, and "mingw"):

- remember(), after that flavor's SDK build succeeded, writes a record (work/sdk-src-<flavor>.json): for every file a
  patch names (the main and the Windows patch lists), every resolve shader header and every file an earlier record
  named, its SHA-256 and its date (mtime in nanoseconds); and the size and date of the build folder's .ninja_log;
- settle(), after the next checkout for that flavor, hashes each of those files again and gives each one whose
  bytes are exactly the recorded ones its recorded date back (the record alone is never trusted), and today's date
  to each of the others; then the record is deleted until that flavor's build succeeds again.

Why that is safe: the record describes the files exactly as the flavor's build saw them, and that build folder has
not been built since (its .ninja_log is unchanged, and settle() deletes the record before a build starts). A file
with the recorded bytes and date looks to ninja and to CMake's install exactly as it did then. Every other file
named here gets today's date, newer than anything built before, so whatever depends on it is built again; that
also covers a file the other flavor gave an older date back to while it held other bytes. Without a usable record
every file named here gets today's date, which is what the checkout always did. A mistake can only cost an extra
rebuild, never an object older than its source.

restore_unchanged() is the same idea for files the tool writes anew every time and nobody else writes (the Windows
build's extra header folder): the date they had just before, when the bytes came out the same.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import time
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Optional, Set, Tuple

RECORD_VERSION = 1
RECORD_PREFIX = "sdk-src-"   # work/sdk-src-<flavor>.json

# The file names a patch touches: its "--- a/..." and "+++ b/..." lines, renames and copies. Names git puts in
# quotes (unusual characters) are left out; such a file just keeps the date git gives it.
_PATCH_NAME = re.compile(r"^(?:--- a/|\+\+\+ b/|rename from |rename to |copy from |copy to )([^\"\t].*?)\s*$")

FileState = Tuple[str, int]   # (SHA-256 of the bytes, mtime in nanoseconds)


def record_path(work: Path, flavor: str) -> Path:
    return Path(work) / f"{RECORD_PREFIX}{flavor}.json"


def file_state(path: Path) -> Optional[FileState]:
    """The SHA-256 and date of a regular file (not a link), or None when there is no such file."""
    try:
        st = os.lstat(path)
        if not stat.S_ISREG(st.st_mode):
            return None
        digest = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest(), st.st_mtime_ns


def set_mtime(path: Path, mtime_ns: Optional[int]) -> bool:
    """Gives `path` that date (None: now), keeping its access time. False when it can't be done."""
    try:
        st = os.lstat(path)
        os.utime(path, ns=(st.st_atime_ns, time.time_ns() if mtime_ns is None else mtime_ns))
    except OSError:
        return False
    return True


def patch_names(patches: Iterable[Path]) -> Set[str]:
    """The SDK-relative file names the patch files touch."""
    names: Set[str] = set()
    for patch in patches:
        try:
            text = Path(patch).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            m = _PATCH_NAME.match(line)
            if m:
                names.add(m.group(1))
    return names


def _usable_name(name: str) -> bool:
    """A plain relative name inside the checkout, outside its build output (out/) and git's own folder."""
    parts = Path(name).parts
    return bool(parts) and not Path(name).is_absolute() and ".." not in parts and parts[0] not in ("out", ".git")


def _ninja_log_state(build_dir: Path) -> Optional[List[int]]:
    """[size, mtime in ns] of a build folder's .ninja_log: it changes with every build that runs in that folder."""
    try:
        st = (Path(build_dir) / ".ninja_log").stat()
    except OSError:
        return None
    return [st.st_size, st.st_mtime_ns]


def _read_record(path: Path) -> dict:
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


class SdkDates:
    """The dates of one SDK flavor's source files around a checkout (see the module's description).

    sdk_src: the checkout; work: the work folder the records go in; flavor: "linux-amd64", "win-amd64" or "mingw";
    build_dir: that flavor's SDK build folder; patches: every patch file (both lists, whichever flavor this is, so
    each record also holds the files only the other flavor patches); more: a function giving other files the
    checkout writes anew (the resolve shader headers), absolute paths inside sdk_src."""

    def __init__(self, sdk_src: Path, work: Path, flavor: str, build_dir: Path, patches: Iterable[Path],
                 more: Optional[Callable[[], Iterable[Path]]] = None):
        self.sdk_src = Path(sdk_src)
        self.work = Path(work)
        self.flavor = flavor
        self.build_dir = Path(build_dir)
        self.patches = list(patches)
        self.more = more
        self.names: Set[str] = set()

    def _names(self) -> Set[str]:
        """Every file this keeps dates for: the patches' files, the other files the checkout writes, and every file
        a record of any flavor holds (one this flavor may have to give today's date to)."""
        names = patch_names(self.patches)
        if self.more:
            for p in self.more():
                try:
                    names.add(Path(p).relative_to(self.sdk_src).as_posix())
                except ValueError:
                    continue
        for rec in self.work.glob(RECORD_PREFIX + "*.json"):
            files = _read_record(rec).get("files")
            if isinstance(files, dict):
                names.update(n for n in files if isinstance(n, str))
        return {n for n in names if _usable_name(n)}

    def _record(self) -> Dict[str, FileState]:
        """This flavor's record, when its build folder hasn't been built since it was written; else empty."""
        data = _read_record(record_path(self.work, self.flavor))
        if data.get("version") != RECORD_VERSION or data.get("flavor") != self.flavor:
            return {}
        log = _ninja_log_state(self.build_dir)
        if log is None or data.get("ninja_log") != log:
            return {}
        files = data.get("files")
        if not isinstance(files, dict):
            return {}
        good: Dict[str, FileState] = {}
        for name, value in files.items():
            if (isinstance(name, str) and isinstance(value, list) and len(value) == 2
                    and isinstance(value[0], str) and isinstance(value[1], int)):
                good[name] = (value[0], value[1])
        return good

    def settle(self) -> Tuple[int, int]:
        """After a checkout for this flavor: the recorded date back on every file with the recorded bytes, today's
        date on every other file named here. Returns (dates kept, dates made today)."""
        self.names = self._names()
        record = self._record()
        # Until this flavor's build succeeds again (remember()), there is no record: a build that stops midway may
        # have compiled some files from the new bytes. (Its .ninja_log would tell too; this is the second guard.)
        try:
            record_path(self.work, self.flavor).unlink(missing_ok=True)
        except OSError:
            record = {}
        kept = renewed = 0
        for name in sorted(self.names):
            path = self.sdk_src / name
            now = file_state(path)
            if now is None:
                continue
            old = record.get(name)
            if old is not None and old[0] == now[0]:
                if now[1] == old[1] or set_mtime(path, old[1]):
                    kept += 1
                    continue
            # Other bytes than this flavor built with, or no record: newer than anything built before.
            if set_mtime(path, None):
                renewed += 1
        return kept, renewed

    def remember(self) -> bool:
        """After this flavor's SDK build succeeded: records the files as that build saw them. False (and no record:
        the next checkout dates everything today, as before) when the build folder has no .ninja_log."""
        log = _ninja_log_state(self.build_dir)
        if log is None:
            return False
        files = {}
        for name in sorted(self.names or self._names()):
            state = file_state(self.sdk_src / name)
            if state is not None:
                files[name] = [state[0], state[1]]
        path = record_path(self.work, self.flavor)
        tmp = path.with_name(path.name + ".tmp")
        try:
            tmp.write_text(json.dumps({"version": RECORD_VERSION, "flavor": self.flavor, "ninja_log": log,
                                       "files": files}, indent=1), encoding="utf-8")
            tmp.replace(path)
        except OSError:   # a full disk, say: no record, so the next checkout simply dates everything today
            tmp.unlink(missing_ok=True)
            return False
        return True


def snapshot(paths: Iterable[Path]) -> Dict[Path, FileState]:
    """The bytes and dates of the regular files among `paths`, before they are written anew."""
    states = {}
    for p in paths:
        state = file_state(Path(p))
        if state is not None:
            states[Path(p)] = state
    return states


def restore_unchanged(before: Dict[Path, FileState]) -> int:
    """Gives every file in `before` whose bytes came out the same its earlier date back; returns how many. Only for
    files nothing else writes: the date they get back is the one they had a moment ago."""
    n = 0
    for path, (digest, mtime_ns) in before.items():
        now = file_state(path)
        if now is not None and now[0] == digest and (now[1] == mtime_ns or set_mtime(path, mtime_ns)):
            n += 1
    return n


__all__ = ["RECORD_PREFIX", "SdkDates", "file_state", "patch_names", "record_path", "restore_unchanged",
           "set_mtime", "snapshot"]

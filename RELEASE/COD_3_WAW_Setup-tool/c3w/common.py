"""Shared helpers: platform detection, per-user folders, logging, running
commands, downloads and guarded deletion.

Nothing in here hard-codes a user path. Every location is either relative to
the folder the tool was started from (input/, output/, work/), comes from the
environment (XDG_*, APPDATA, LOCALAPPDATA, SystemDrive, HOME), or is what the
user picked in the GUI. Paths are handled with pathlib, so Windows and Linux
separators both work.
"""
from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import tarfile
import threading
import time
import urllib.request
import zipfile
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, List, Optional, Sequence

IS_WINDOWS = os.name == "nt"
IS_LINUX = sys.platform.startswith("linux")
APP_ID = "cod3-waw-setup"
APP_NAME = "COD3-setup-tool"  # the window title (owner, 2026-10-10)


def wine_version() -> str:
    """The Wine version when this Windows Python runs under Wine, else "".
    setup.bat sets C3W_WINE=1 when it finds Wine's registry key; Wine's
    ntdll also exports wine_get_version, which Windows' doesn't."""
    if not IS_WINDOWS:
        return ""
    try:
        import ctypes
        get = ctypes.windll.ntdll.wine_get_version
        get.restype = ctypes.c_char_p
        return (get() or b"").decode("ascii", "replace") or "unknown version"
    except (AttributeError, OSError):
        return "unknown version" if os.environ.get("C3W_WINE") == "1" else ""


UNDER_WINE = bool(wine_version())


class Cancelled(Exception):
    """Raised when the user pressed Cancel."""


class SetupError(Exception):
    """A step failed in a way the user can act on (message is shown as-is)."""


# ---------------------------------------------------------------------------
# Platform folders
# ---------------------------------------------------------------------------
def exe_name(name: str) -> str:
    return name + ".exe" if IS_WINDOWS and not name.lower().endswith(".exe") else name


def _env_path(var: str) -> Optional[Path]:
    v = os.environ.get(var)
    return Path(v) if v else None


def legacy_config_dir() -> Path:
    """Where settings were kept before 2026-10-10 (the user's profile)."""
    if IS_WINDOWS:
        base = _env_path("APPDATA") or Path.home() / "AppData" / "Roaming"
    else:
        base = _env_path("XDG_CONFIG_HOME") or Path.home() / ".config"
    return base / APP_ID


def config_dir() -> Path:
    """Where the tool keeps its own settings: config/ in the project folder,
    like everything else it makes (owner, 2026-10-10: "everything that can
    be should be kept within the same folder"). Settings from the old place
    in the user's profile are copied over once. Only if the project folder
    can't be written (unzipped somewhere read-only) does it stay in the
    profile."""
    here = project_dir() / "config"
    old = legacy_config_dir()
    try:
        here.mkdir(parents=True, exist_ok=True)
        probe = here / ".write-test"
        probe.write_text("", encoding="ascii")
        probe.unlink()
    except OSError:
        return old
    if not (here / "settings.json").exists() and (old / "settings.json").is_file():
        try:
            shutil.copy2(old / "settings.json", here / "settings.json")
        except OSError:
            pass
    return here


def cache_dir() -> Path:
    if IS_WINDOWS:
        base = _env_path("LOCALAPPDATA") or Path.home() / "AppData" / "Local"
    else:
        base = _env_path("XDG_CACHE_HOME") or Path.home() / ".cache"
    return base / APP_ID


def project_dir() -> Path:
    """The folder this tool was started from: the one holding setup.sh /
    setup.bat (or, for the launcher copy inside a built game's folder, that
    folder). Its own folders hold everything by default:

        input/    your discs and downloads (looked at first)
        output/   the built game
        work/     temporary build files (deleted at the end), except
                  work/tools: downloaded tools, Rust and (Windows) Python,
                  kept for the next build
        config/   this tool's settings
    """
    return Path(__file__).resolve().parent.parent.parent


def default_input_dir() -> Path:
    return project_dir() / "input"


def default_work_dir() -> Path:
    return project_dir() / "work"


def default_output_dir() -> Path:
    return project_dir() / "output"


def build_path(work: Path) -> Path:
    """The path the compilers are given for `work`.

    Some build tools cope badly with spaces in paths, and Windows has a
    260-character path limit. When `work` has either problem, the builds go
    through a short link to it instead: a symbolic link in the per-user cache
    folder on Linux, a directory junction on Windows (neither needs admin
    rights). The files themselves stay in `work`. If no link can be made,
    `work` is used as it is.

    On Windows, letters outside plain ASCII (a user folder named José) count as a
    problem too: tools that use the ANSI code page can't open such paths. And
    the Microsoft Store's Python keeps what it writes in AppData to itself, so
    with it the link goes to the system drive's root instead."""
    work = Path(work)
    if not awkward_path(str(work)):
        return work
    if IS_WINDOWS:
        local = _env_path("LOCALAPPDATA")
        candidates = [local / "c3w-work"] if local and not store_python() else []
        candidates.append(Path(os.environ.get("SystemDrive", "C:") + os.sep) / "c3w-work")
    else:
        candidates = [cache_dir() / "work-link"]
        user = os.environ.get("USER") or os.environ.get("LOGNAME") or "user"
        candidates.append(Path("/var/tmp") / f"{APP_ID}-{user}-work")
    work.mkdir(parents=True, exist_ok=True)
    for link in candidates:
        if awkward_path(str(link)):
            continue
        try:
            if _points_to(link, work):
                return link
            if link.is_symlink() or (IS_WINDOWS and _is_junction(link)):
                _remove_link(link)
            elif link.exists():
                continue   # a real folder of that name: leave it alone
            link.parent.mkdir(parents=True, exist_ok=True)
            if IS_WINDOWS:
                import _winapi  # CPython on Windows; junctions need no admin rights
                _winapi.CreateJunction(str(work), str(link))
            else:
                link.symlink_to(work, target_is_directory=True)
            return link
        except (OSError, ImportError, AttributeError):
            continue
    return work


def awkward_path(text: str) -> bool:
    """A path some build tools choke on: one with spaces; on Windows also one
    longer than 60 characters (the build's own paths add up to 200 more, and
    Windows' limit is 260) or with letters outside plain ASCII."""
    return " " in text or (IS_WINDOWS and (len(text) > 60 or not text.isascii()))


def store_python() -> bool:
    """This Python is the Microsoft Store's (it runs in an app sandbox)."""
    exe = (sys.executable or "").lower().replace("/", "\\")
    return IS_WINDOWS and ("\\windowsapps\\" in exe or "pythonsoftwarefoundation.python" in exe)


def _points_to(link: Path, target: Path) -> bool:
    try:
        return (link.is_symlink() or (IS_WINDOWS and _is_junction(link))) and \
            os.path.samefile(link, target)
    except OSError:
        return False


def _is_junction(p: Path) -> bool:
    is_junction = getattr(os.path, "isjunction", None)   # Python 3.12+
    if is_junction:
        return bool(is_junction(p))
    try:
        return bool(os.lstat(p).st_file_attributes & 0x400)   # FILE_ATTRIBUTE_REPARSE_POINT
    except (OSError, AttributeError):
        return False


def _remove_link(link: Path) -> None:
    """Removes a symbolic link or junction itself, never what it points to."""
    if IS_WINDOWS and _is_junction(link):
        os.rmdir(link)   # removes the junction only
    else:
        link.unlink()


def remove_build_link(link: Path, work: Path) -> None:
    """Removes the link made by build_path(), if `link` is one."""
    link = Path(link)
    if link != Path(work) and _points_to(link, work):
        _remove_link(link)


def host_triple() -> str:
    machine = platform.machine().lower()
    arch = {"x86_64": "amd64", "amd64": "amd64", "aarch64": "arm64", "arm64": "arm64"}.get(machine, machine)
    osname = "win" if IS_WINDOWS else ("linux" if IS_LINUX else sys.platform)
    return f"{osname}-{arch}"


def cpu_count() -> int:
    try:
        return len(os.sched_getaffinity(0))  # type: ignore[attr-defined]
    except (AttributeError, OSError):
        return os.cpu_count() or 2


def total_ram_gb() -> float:
    try:
        if IS_LINUX:
            with open("/proc/meminfo", encoding="ascii") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        return int(line.split()[1]) / (1024 * 1024)
        if IS_WINDOWS:
            import ctypes

            class MEMSTAT(ctypes.Structure):
                _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong),
                            ("total", ctypes.c_ulonglong), ("avail", ctypes.c_ulonglong),
                            ("tpage", ctypes.c_ulonglong), ("apage", ctypes.c_ulonglong),
                            ("tvirt", ctypes.c_ulonglong), ("avirt", ctypes.c_ulonglong),
                            ("ext", ctypes.c_ulonglong)]
            st = MEMSTAT()
            st.len = ctypes.sizeof(MEMSTAT)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st))
            return st.total / (1024 ** 3)
    except Exception:
        pass
    return 8.0


def recommended_jobs() -> int:
    """Recompiled game code is heavy: budget ~2 GB of RAM per compiler job."""
    by_ram = max(1, int(total_ram_gb() // 2))
    return max(1, min(cpu_count(), by_ram))


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def free_space(path: Path) -> int:
    p = Path(path)
    while not p.exists() and p.parent != p:
        p = p.parent
    return shutil.disk_usage(p).free


def dir_size(path: Path) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += (Path(root) / name).stat().st_size
            except OSError:
                pass
    return total


def sha1_file(path: Path) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Path safety
# ---------------------------------------------------------------------------
def resolved(p: Path) -> Path:
    try:
        return Path(p).expanduser().resolve()
    except OSError:
        return Path(os.path.abspath(Path(p).expanduser()))


def is_within(child: Path, parent: Path) -> bool:
    c, p = resolved(child), resolved(parent)
    if IS_WINDOWS:
        c, p = Path(str(c).lower()), Path(str(p).lower())
    return c == p or p in c.parents


def overlaps(a: Path, b: Path) -> bool:
    return is_within(a, b) or is_within(b, a)


# The work folder is emptied at the end of a build (all but tools/), so the
# tool only uses a folder it made itself: this file marks it.
WORK_MARKER = ".cod3-setup-work"
# What a build leaves in its work folder (for work folders from before the
# marker existed).
WORK_NAMES = {"tools", "wsl", "scratch", "rust-target", "waw_xbox360", "waw_pc_disc", "mingw", "setup.log",
              "rexglue-sdk", "upscale", "sdk.stamp.json", "sdk-mingw.stamp.json", "cod3", "appimage-runtime",
              "AppDir", "map-pack-unpack", "map-unpack", WORK_MARKER}


def owns_work_dir(work: Path) -> bool:
    """Whether `work` is a work folder this tool marked as its own."""
    return (Path(work) / WORK_MARKER).is_file()


def claim_work_dir(work: Path) -> None:
    """Marks `work` as this tool's work folder before anything goes in it.
    Everything in a work folder except tools/ is deleted at the end of a
    build, so a folder that already holds other files (Documents, Downloads,
    a game folder ...) is refused: SetupError. A new or empty folder, the
    default work folder, and one holding only what a build leaves there are
    accepted."""
    work = Path(work)
    if owns_work_dir(work):
        return
    try:
        others = sorted(p.name for p in work.iterdir() if p.name not in WORK_NAMES) if work.is_dir() else []
    except OSError as e:
        raise SetupError(f"Can't read the work folder {work} ({e}).")
    if work.exists() and not work.is_dir():
        raise SetupError(f"The work folder {work} is a file. Pick an empty folder.")
    if others and resolved(work) != resolved(default_work_dir()):
        shown = ", ".join(others[:5]) + (", ..." if len(others) > 5 else "")
        raise SetupError(f"The work folder {work} already holds other files ({shown}). Everything in the work "
                         "folder except tools/ is deleted at the end of a build, so pick an empty folder (or "
                         "leave it empty to use the default).")
    work.mkdir(parents=True, exist_ok=True)
    (work / WORK_MARKER).write_text("COD3-setup-tool work folder: its contents (all but tools/) are deleted at "
                                    "the end of every build.\n", encoding="utf-8")


def safe_rmtree(target: Path, allowed_root: Path, protected: Iterable[Path], log: "Log") -> None:
    """Delete `target` only if it lives inside `allowed_root` and does not
    overlap any protected path (the CoD3 ISO, the WaW folder, the output)."""
    target = Path(target)
    if not target.exists():
        return
    if not is_within(target, allowed_root):
        raise SetupError(f"Refusing to delete {target}: it is outside {allowed_root}")
    for p in protected:
        if p and overlaps(target, Path(p)):
            raise SetupError(f"Refusing to delete {target}: it overlaps protected path {p}")

    def onerror(func, path, exc):  # read-only files (git objects on Windows)
        try:
            os.chmod(path, 0o700)
            func(path)
        except OSError:
            pass

    log.info(f"Removing {target}")
    if sys.version_info >= (3, 12):
        shutil.rmtree(target, onexc=onerror)
    else:
        shutil.rmtree(target, onerror=onerror)


# ---------------------------------------------------------------------------
# Logging (thread-safe, fans out to the GUI / console and a log file)
# ---------------------------------------------------------------------------
@dataclass
class Log:
    sinks: list = field(default_factory=list)          # callables(kind, text, frac)
    file: Optional[Path] = None
    _lock: threading.Lock = field(default_factory=threading.Lock)

    def _emit(self, kind: str, text: str, frac: Optional[float] = None) -> None:
        with self._lock:
            if self.file and text:
                try:
                    self.file.parent.mkdir(parents=True, exist_ok=True)
                    with open(self.file, "a", encoding="utf-8", errors="replace") as f:
                        f.write(f"{time.strftime('%H:%M:%S')} [{kind}] {text}\n")
                except OSError:
                    pass
            for sink in list(self.sinks):
                try:
                    sink(kind, text, frac)
                except Exception:
                    pass

    def info(self, text: str) -> None:
        self._emit("info", text)

    def warn(self, text: str) -> None:
        self._emit("warn", text)

    def error(self, text: str) -> None:
        self._emit("error", text)

    def step(self, text: str) -> None:
        self._emit("step", text)

    def output(self, text: str) -> None:
        self._emit("out", text)

    def progress(self, frac: Optional[float], text: str = "") -> None:
        self._emit("progress", text, frac)


_console_state = {"progress": False}


def set_console_line_mode(on: bool) -> None:
    """Every message, progress included, as one whole line: for when another
    program reads this output line by line (the build inside WSL)."""
    _console_state["lines"] = on


def console_sink(kind: str, text: str, frac: Optional[float]) -> None:
    if _console_state.get("lines"):
        if kind == "progress":
            if not text:
                return
            now = time.time()
            if now - _console_state.get("last_line", 0.0) < 0.5 and (frac is None or frac < 1.0):
                return   # twice a second is plenty for a progress bar
            _console_state["last_line"] = now
            pct = f"{frac * 100:5.1f}% " if frac is not None else "  0.0% "
            sys.stdout.write(f"  {pct}{text[:100]}\n")
        else:
            prefix = {"step": "==> ", "warn": "WARNING: ", "error": "ERROR: ", "out": "    "}.get(kind, "")
            sys.stdout.write(prefix + text + "\n")
        sys.stdout.flush()
        return
    if kind == "progress":
        if text:
            pct = f"{frac * 100:5.1f}% " if frac is not None else ""
            lead = "\r" if _console_state["progress"] else "\n"
            sys.stdout.write(f"{lead}  {pct}{text[:100]:<100}")
            sys.stdout.flush()
            _console_state["progress"] = True
        return
    prefix = {"step": "\n==> ", "warn": "WARNING: ", "error": "ERROR: ", "out": "    "}.get(kind, "")
    # Progress is redrawn in place with \r; every other message starts on a
    # fresh line.
    sys.stdout.write("\n" + prefix + text)
    sys.stdout.flush()
    _console_state["progress"] = False


# ---------------------------------------------------------------------------
# Running commands
# ---------------------------------------------------------------------------
class Runner:
    """Runs external commands, streams their output to the log, and can kill
    the whole process tree when the user cancels."""

    def __init__(self, log: Log, cancel: threading.Event):
        self.log = log
        self.cancel = cancel
        self._proc: Optional[subprocess.Popen] = None
        # Optional: turns a failed command's last output lines into a plain
        # hint for the error message (pc_check.failure_hint; "" = none).
        self.failure_hint: Optional[Callable[[Sequence[str]], str]] = None

    def check_cancel(self) -> None:
        if self.cancel.is_set():
            raise Cancelled()

    def run(self, cmd: Sequence, cwd: Optional[Path] = None, env: Optional[dict] = None,
            check: bool = True, quiet: bool = False,
            line_filter: Optional[Callable[[str], None]] = None) -> int:
        self.check_cancel()
        cmd = [str(c) for c in cmd]
        self.log.info("$ " + " ".join(_quote(c) for c in cmd))
        kwargs: dict = dict(cwd=str(cwd) if cwd else None, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        if IS_WINDOWS:
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | 0x08000000  # NO_WINDOW
        else:
            kwargs["start_new_session"] = True
        try:
            proc = subprocess.Popen(cmd, **kwargs)
        except FileNotFoundError:
            raise SetupError(f"Program not found: {cmd[0]}")
        except OSError as e:
            if not IS_WINDOWS:
                raise
            # Windows: an antivirus that blocks or quarantines the program,
            # or a file that isn't a program for this PC.
            raise SetupError(f"Windows couldn't start {cmd[0]} ({e}). If an antivirus blocked it, allow it (or "
                             "add the project folder to its exclusions) and build again.")
        self._proc = proc
        watcher = threading.Thread(target=self._watch, args=(proc,), daemon=True)
        watcher.start()
        assert proc.stdout is not None
        tail: deque = deque(maxlen=400)   # the last lines, for failure_hint
        for raw in iter(proc.stdout.readline, b""):
            line = raw.decode("utf-8", errors="replace").rstrip()
            tail.append(line)
            if line_filter:
                line_filter(line)
            if not quiet and line:
                self.log.output(line)
        proc.stdout.close()
        rc = proc.wait()
        self._proc = None
        if self.cancel.is_set():
            raise Cancelled()
        if check and rc != 0:
            hint = self.failure_hint(list(tail)) if self.failure_hint else ""
            raise SetupError(f"Command failed (exit code {rc}): {' '.join(_quote(c) for c in cmd[:6])}"
                             f"{' ...' if len(cmd) > 6 else ''}\nSee the log above for details."
                             + (f"\n\n{hint}" if hint else ""))
        return rc

    def capture(self, cmd: Sequence, cwd: Optional[Path] = None, env: Optional[dict] = None,
                timeout: float = 60) -> tuple[int, str]:
        try:
            kwargs: dict = {}
            if IS_WINDOWS:
                kwargs["creationflags"] = 0x08000000
            p = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None, env=env,
                               capture_output=True, timeout=timeout, stdin=subprocess.DEVNULL, **kwargs)
            out = (p.stdout or b"").decode("utf-8", "replace") + (p.stderr or b"").decode("utf-8", "replace")
            return p.returncode, out
        except (OSError, subprocess.TimeoutExpired) as e:
            return -1, str(e)

    def _watch(self, proc: subprocess.Popen) -> None:
        while proc.poll() is None:
            if self.cancel.wait(0.5):
                kill_tree(proc)
                return


def kill_tree(proc: subprocess.Popen) -> None:
    try:
        if IS_WINDOWS:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)],
                           capture_output=True, creationflags=0x08000000)
        else:
            os.killpg(proc.pid, signal.SIGTERM)
            time.sleep(2)
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
    except (OSError, ProcessLookupError):
        pass


def _quote(s: str) -> str:
    return f'"{s}"' if (" " in s or not s) else s


# ---------------------------------------------------------------------------
# Approval: nothing is downloaded or installed without the user's yes
# ---------------------------------------------------------------------------
@dataclass
class Install:
    """Something the tool wants to download or install, as the user sees it
    before approving it."""
    key: str          # identifies it (a pinned download's name, "rust", "system-packages" ...)
    what: str         # "CMake 3.31.6 (build tool)"
    where: str        # the folder it ends up in, or "system folders (apt)"
    size: str = ""    # "about 50 MB"
    source: str = ""  # "github.com", "your distribution's package manager"


class Approval:
    """Asks the user once for a list of downloads and installs, and remembers
    the answer for this run. Owner, 2026-10-10: "things will require approval
    before downloading". The GUI and the command line each set `ask`; with
    no `ask`, nothing is approved."""

    def __init__(self) -> None:
        self.ask: Optional[Callable[[List[Install]], bool]] = None
        self._approved: set = set()
        self._lock = threading.Lock()

    def reset(self) -> None:
        with self._lock:
            self._approved.clear()

    def approved(self, key: str) -> bool:
        with self._lock:
            return key in self._approved

    def require(self, items: Sequence[Install], log: "Log") -> None:
        """Returns once every item is approved; raises SetupError if the user
        says no (nothing is downloaded then)."""
        with self._lock:
            todo = [i for i in items if i.key not in self._approved]
        seen: set = set()
        todo = [i for i in todo if not (i.key in seen or seen.add(i.key))]
        if not todo:
            return
        if self.ask is None or not self.ask(todo):
            raise SetupError("Not approved, so nothing was downloaded or installed:\n  - " +
                             "\n  - ".join(f"{i.what} -> {i.where}" for i in todo))
        with self._lock:
            self._approved.update(i.key for i in todo)
        for i in todo:
            log.info(f"Approved: {i.what}{f' ({i.size})' if i.size else ''} -> {i.where}")


APPROVAL = Approval()


def url_host(url: str) -> str:
    return url.split("://", 1)[-1].split("/", 1)[0]


# ---------------------------------------------------------------------------
# Downloads and archives
# ---------------------------------------------------------------------------
def download(url: str, dest: Path, log: Log, cancel: threading.Event, sha256: Optional[str] = None,
             approval: Optional[Install] = None) -> Path:
    """Downloads `url` to `dest`, checking `sha256`. Asks first (APPROVAL)
    unless `approval`, or this URL, was approved already; a file downloaded
    earlier that matches is used without asking."""
    dest = Path(dest)
    if dest.exists() and dest.stat().st_size > 0:
        if not sha256 or _sha256_file(dest) == sha256.lower():
            return dest
        log.warn(f"{dest.name} doesn't match its checksum; downloading it again.")
        dest.unlink()
    APPROVAL.require([approval or Install(url, dest.name, str(dest.parent), "", url_host(url))], log)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    log.info(f"Downloading {url}")
    req = urllib.request.Request(url, headers={"User-Agent": f"{APP_ID}/1.0"})
    h = hashlib.sha256()
    try:
        with urllib.request.urlopen(req, timeout=60) as r, open(tmp, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            done = 0
            last = 0.0
            while True:
                if cancel.is_set():
                    raise Cancelled()
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                h.update(chunk)
                done += len(chunk)
                now = time.time()
                if now - last > 0.25:
                    last = now
                    log.progress(done / total if total else None,
                                 f"{dest.name}: {human(done)}" + (f" / {human(total)}" if total else ""))
    except (OSError, ValueError) as e:   # URLError, timeouts, a dropped connection, no space
        tmp.unlink(missing_ok=True)
        raise SetupError(f"Download failed: {url} ({e}). Check the internet connection and build again.")
    if sha256 and h.hexdigest().lower() != sha256.lower():
        tmp.unlink(missing_ok=True)
        raise SetupError(f"Checksum mismatch for {url}")
    tmp.replace(dest)
    return dest


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def extract_archive(archive: Path, dest: Path, log: Log) -> Path:
    """Extract .zip / .tar.* archives with Python only (no external tools)."""
    archive, dest = Path(archive), Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    name = archive.name.lower()
    log.info(f"Extracting {archive.name}")
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for m in z.infolist():
                _check_member(dest, m.filename)
            z.extractall(dest)
            # zipfile drops the executable bit; restore it from the Unix mode.
            if not IS_WINDOWS:
                for m in z.infolist():
                    mode = (m.external_attr >> 16) & 0o777
                    if mode & 0o111:
                        os.chmod(dest / m.filename, mode)
    elif ".tar" in name:
        with tarfile.open(archive) as t:
            for m in t.getmembers():
                _check_member(dest, m.name)
                if m.issym() or m.islnk():
                    # A link may only point inside the destination too.
                    base = (dest / m.name).parent if m.issym() else dest
                    _check_member(dest, os.path.relpath(base / m.linkname, dest))
            if hasattr(tarfile, "data_filter"):   # Python 3.12, and 3.9.17+ / 3.10.12+ / 3.11.4+
                t.extractall(dest, filter="data")
            else:
                t.extractall(dest)
    else:
        raise SetupError(f"Don't know how to extract {archive.name}")
    return dest


def _check_member(dest: Path, name: str) -> None:
    target = (dest / name)
    if not is_within(target, dest):
        raise SetupError(f"Archive member escapes the destination: {name}")


# ---------------------------------------------------------------------------
# Copying with progress
# ---------------------------------------------------------------------------
def copy_tree(src: Path, dst: Path, log: Log, cancel: threading.Event, label: str,
              hardlink: bool = False, skip: Callable[[Path], bool] = lambda p: False) -> None:
    """Copy (or hard-link) a folder, skipping files that already match in size.
    Never touches `src`."""
    src, dst = Path(src), Path(dst)
    files = []
    for root, dirs, names in os.walk(src):
        rel_root = Path(root).relative_to(src)
        if not skip(rel_root):
            (dst / rel_root).mkdir(parents=True, exist_ok=True)  # keep empty folders too
        for n in names:
            rel = rel_root / n
            if not skip(rel):
                files.append(rel)
    total = sum((src / r).stat().st_size for r in files) or 1
    done = 0
    last = 0.0
    link_ok = hardlink
    for rel in files:
        if cancel.is_set():
            raise Cancelled()
        s, d = src / rel, dst / rel
        size = s.stat().st_size
        if d.exists() and d.stat().st_size == size:
            done += size
            continue
        d.parent.mkdir(parents=True, exist_ok=True)
        if link_ok:
            try:
                if d.exists():
                    d.unlink()
                os.link(s, d)
            except OSError:
                link_ok = False  # different filesystem / exFAT: fall back to copying
        if not link_ok:
            tmp = d.with_name(d.name + ".c3wtmp")
            shutil.copyfile(s, tmp)
            shutil.copystat(s, tmp, follow_symlinks=True)
            tmp.replace(d)
        done += size
        now = time.time()
        if now - last > 0.2:
            last = now
            log.progress(done / total, f"{label}: {rel.as_posix()}")
    log.progress(1.0, f"{label}: done ({human(total)})")


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, indent=2), encoding="utf-8")
    tmp.replace(path)


def read_json(path: Path) -> dict:
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def foreign_path(value: object) -> bool:
    """True for an absolute path written on the other kind of system, as in a
    settings file copied from Linux to Windows or back: "/srv/games" on
    Windows (it would mean C:\\srv\\games), "C:\\..." or "\\\\server\\share" on
    Linux and macOS. Such a path points somewhere nobody chose."""
    if not isinstance(value, str) or not value.strip():
        return False
    v = value.strip()
    if IS_WINDOWS:
        return v.startswith("/") and not v.startswith("//")
    return bool(v[:1].isalpha() and v[1:2] == ":" and v[2:3] in ("\\", "/")) or v.startswith("\\\\")


def drop_foreign_paths(settings: dict, keys: Iterable[str]) -> tuple:
    """Settings without the paths written on another system (foreign_path):
    (the settings, the keys that were dropped). A list of paths keeps the
    ones that are fine here."""
    out, dropped = dict(settings), []
    for key in keys:
        value = out.get(key)
        if isinstance(value, list):
            kept = [v for v in value if not foreign_path(v)]
            if len(kept) != len(value):
                out[key] = kept
                dropped.append(key)
        elif foreign_path(value):
            del out[key]
            dropped.append(key)
    return out, dropped


def open_in_file_manager(path: Path) -> None:
    path = Path(path)
    if IS_WINDOWS:
        os.startfile(str(path))  # type: ignore[attr-defined]
    else:
        opener = shutil.which("xdg-open") or shutil.which("gio")
        if opener:
            args = [opener, str(path)] if opener.endswith("xdg-open") else [opener, "open", str(path)]
            subprocess.Popen(args, start_new_session=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

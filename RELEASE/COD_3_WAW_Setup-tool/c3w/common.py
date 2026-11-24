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
import http.client   # [acct2-decomp] 2026-11-21 download(): HTTPException isn't an OSError
import json
import ntpath   # [waw-decomp] 2026-11-16 awkward_path: Windows' rules on any system
import os
import platform
import shutil
import signal
import subprocess
import sys
import tarfile
import threading
import time
import traceback   # [acct2-ocr] 2026-11-22 the detailed log: tracebacks
import urllib.request
import zipfile
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime   # [acct2-ocr] 2026-11-22 the detailed log: the date and time zone in its header
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
    with it the link goes to the system drive's root instead.

    [waw-decomp] 2026-11-16: any character outside PLAIN_PATH_CHARACTERS counts
    (awkward_path); the link is alias_path's. [waw-decomp] 2026-11-17 (S9): its
    name carries a short digest of the work folder's path (work-<8 hex>-link,
    c3w-work-<8 hex>), so two awkward work folders building at once never
    repoint each other's link."""
    return alias_path(work, work_link_name(work))


def work_link_name(work: Path) -> str:
    """[waw-decomp] 2026-11-17 (S9): the name alias_path gives the work folder's link (also shown on the Set up page
    and in pc_check's note)."""
    return "work-" + path_digest(Path(work))


def alias_path(target: Path, name: str) -> Path:
    """[waw-decomp] 2026-11-16 (rc2-paths; owner 14:33 "make sure ANY character that windows supports within a path
    is allowed"): build_path's link, for any folder whose path goes to the build tools (the work folder, the
    project folder, Python's folder). `target` itself when its path is plain (awkward_path), else a link to it with
    a plain name: <cache>/<name>-link or /var/tmp/cod3-waw-setup-<user>-<name> on Linux (symbolic links),
    %LOCALAPPDATA%\\c3w-<name> or <system drive>\\c3w-<name> on Windows (directory junctions; neither needs admin
    rights). The folder keeps the name the user gave it; if no link can be made, `target` is used as it is."""
    target = Path(target)
    if not awkward_path(str(target)):
        return target
    if IS_WINDOWS:
        local = _env_path("LOCALAPPDATA")
        candidates = [local / f"c3w-{name}"] if local and not store_python() else []
        candidates.append(Path(os.environ.get("SystemDrive", "C:") + os.sep) / f"c3w-{name}")
    else:
        candidates = [cache_dir() / f"{name}-link"]
        user = os.environ.get("USER") or os.environ.get("LOGNAME") or "user"
        candidates.append(Path("/var/tmp") / f"{APP_ID}-{user}-{name}")
    target.mkdir(parents=True, exist_ok=True)
    for link in candidates:
        if awkward_path(str(link)):
            continue
        try:
            if _points_to(link, target):
                return link
            if link.is_symlink() or (IS_WINDOWS and _is_junction(link)):
                _remove_link(link)
            elif link.exists():
                continue   # a real folder of that name: leave it alone
            link.parent.mkdir(parents=True, exist_ok=True)
            if IS_WINDOWS:
                import _winapi  # CPython on Windows; junctions need no admin rights
                _winapi.CreateJunction(str(target), str(link))
            else:
                link.symlink_to(target, target_is_directory=True)
            return link
        except (OSError, ImportError, AttributeError):
            continue
    return target


def project_link() -> Path:
    """[waw-decomp] 2026-11-16: project_dir() as the build tools are given it (alias_path): its bro/, ngl_vk/,
    engine/ and the setup tool's recomp/src go into CMake's files and the compilers' command lines. The link's name
    carries a short digest of the folder's path, so two copies of the project building at once never share one."""
    return alias_path(project_dir(), "project-" + path_digest(project_dir()))


def path_digest(path: Path) -> str:
    """[waw-decomp] 2026-11-16: 8 hex digits that tell one folder's path from another's (for links' names)."""
    return hashlib.sha1(str(path).encode("utf-8", "surrogatepass")).hexdigest()[:8]


# [waw-decomp] 2026-11-16 (rc2-paths): the ASCII characters every build tool takes in a path as they are. A path with
# any other ASCII character (Windows allows all but < > : " / \ | ? * in names) reaches the build tools through a
# link (alias_path): CMake splits lists at ";" and expands "${...}" in the files the tool writes for it, make and
# ninja give "$" "#" and ":" a meaning, shells quotes "$" and "`", .bat files and cmd "%" "!" "^" and "&", the
# linker's -Wl, options ",". Letters outside ASCII are plain on Linux (UTF-8 throughout) but not on Windows, whose
# ANSI code page has no room for most of them.
PLAIN_PATH_CHARACTERS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._-+~")


def awkward_path(text: str, windows: Optional[bool] = None) -> bool:
    """A path some build tools choke on: one with an ASCII character outside
    PLAIN_PATH_CHARACTERS (a space included); on Windows also one with any
    letter outside ASCII or longer than 60 characters (the build's own paths
    add up to 200 more, and Windows' limit is 260); its drive letter and
    backslashes are plain there. [waw-decomp] 2026-11-16: was spaces only
    (and on Windows letters outside ASCII and the length). `windows`: judge
    it by Windows' rules (None: this system's; the tests check both)."""
    text = str(text)
    if windows is None:
        windows = IS_WINDOWS
    if windows:
        if len(text) > 60:
            return True
        drive, text = ntpath.splitdrive(text)
        if drive and not (len(drive) == 2 and drive[1] == ":" and drive[0].isascii() and drive[0].isalpha()):
            return True   # \\server\share: through a link as well
        text = text.replace("\\", "/")
        return any(c not in PLAIN_PATH_CHARACTERS for c in text)
    return any(c.isascii() and c not in PLAIN_PATH_CHARACTERS for c in text)


def cmake_quote(text: str) -> str:
    """[waw-decomp] 2026-11-16: `text` as a CMake quoted argument for the files the tool writes for CMake (toolchain
    files): \\ " and $ escaped, so "${v}" in a folder's name is read back as it is, not as a variable. A ";" still
    makes a list where CMake uses the value: such paths reach CMake through alias_path's links."""
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$") + '"'


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


def free_text(path: Optional[Path]) -> str:
    """[acct2-ocr] 2026-11-22 the free space where `path` is, for a log line ("?" when it can't be read)."""
    try:
        return human(free_space(Path(path))) if path else "?"
    except (OSError, ValueError):
        return "?"


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
              "AppDir", "map-pack-unpack", "map-unpack", "bro", WORK_MARKER}   # [acct2-bro] bro: Big Red One's build


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
#
# [acct2-ocr] 2026-11-22 (owner 00:00 "Please make the setup tool logging better and more verbose so I would be
# able to figure out what's wrong"): every start of the tool (the window or a command) writes its own session log,
# <config>/logs/tool-<date>-<time>-<pid>.log (the last KEEP_SESSION_LOGS), with every message of every Log, the
# details (debug) included, times to the millisecond, the full traceback of every error and crash, and what each
# command and download did. The screen shows the details only when asked (the Log page's Details box, --verbose, or
# COD3_SETUP_VERBOSE=1); the files always have them.
# ---------------------------------------------------------------------------
VERBOSE_ENV = "COD3_SETUP_VERBOSE"
KEEP_SESSION_LOGS = 20
SESSION_LOGS = "logs"            # under config_dir()
SESSION_PREFIX = "tool-"
_SECRET_WORDS = ("TOKEN", "SECRET", "PASSWORD", "PASSWD", "APIKEY", "API_KEY", "CREDENTIAL", "AUTH")
_session: dict = {"file": None, "fd": None, "lock": threading.Lock(), "last_progress": 0.0}
_verbose = {"on": os.environ.get(VERBOSE_ENV, "").strip() not in ("", "0", "false", "no", "off")}


def set_verbose(on: bool) -> None:
    """Show the details (debug lines) on the console too."""
    _verbose["on"] = bool(on)


def verbose() -> bool:
    return _verbose["on"]


def log_stamp() -> str:
    """The time a log line starts with: 18:10:43.217."""
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def session_log() -> Optional[Path]:
    """This run's session log, or None before start_session_log()."""
    return _session["file"]


def _session_write(kind: str, text: str, skip: Optional[Path] = None) -> None:
    path = _session["file"]
    if not path or not text or (skip is not None and skip == path):
        return
    if kind == "progress":   # a progress line a second is plenty in the session log
        now = time.monotonic()
        if now - _session["last_progress"] < 1.0:
            return
        _session["last_progress"] = now
    with _session["lock"]:
        try:
            with open(path, "a", encoding="utf-8", errors="replace") as f:
                f.write(f"{log_stamp()} [{kind}] {text}\n")
        except OSError:
            pass


def _masked(name: str, value: str) -> str:
    return "<hidden>" if any(w in name.upper() for w in _SECRET_WORDS) else value


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
                        f.write(f"{log_stamp()} [{kind}] {text}\n")
                except OSError:
                    pass
            _session_write(kind, text, skip=self.file)
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

    def debug(self, text: str) -> None:
        """A detail for finding out what went wrong: always in the log files, on screen when asked."""
        self._emit("debug", text)

    def exception(self, e: BaseException, context: str = "") -> None:
        """An error with where it happened: a SetupError (the tool's own message, already worded for the user)
        gets its traceback as details; anything else is logged as an error with its whole traceback."""
        tb = "".join(traceback.format_exception(type(e), e, e.__traceback__)).rstrip()
        lead = f"{context}: " if context else ""
        if isinstance(e, (SetupError, Cancelled)):
            self.debug(f"{lead}{type(e).__name__} raised here:\n{tb}")
        else:
            self.error(f"{lead}unexpected {type(e).__name__}: {e}\n{tb}")


def start_session_log(what: str, argv: Optional[Sequence[str]] = None, header: Sequence[str] = (),
                      folder: Optional[Path] = None) -> Optional[Path]:
    """Starts this run's session log (once; a second call returns the first file): the header says what started,
    with which Python on which system, from where and with what environment, and the crash hooks are put in
    (install_crash_logging). Old session logs past KEEP_SESSION_LOGS are removed. None when no folder can be
    written."""
    if _session["file"]:
        return _session["file"]
    try:
        logs = Path(folder) if folder else config_dir() / SESSION_LOGS
        logs.mkdir(parents=True, exist_ok=True)
        path = logs / f"{SESSION_PREFIX}{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}.log"
        path.touch()
    except OSError:
        return None
    _session["file"] = path
    for old in sorted(logs.glob(f"{SESSION_PREFIX}*.log"))[:-KEEP_SESSION_LOGS]:
        try:
            old.unlink()
        except OSError:
            pass
    now = datetime.now().astimezone()
    lines = [f"---- session ---- {what}, started {now.strftime('%Y-%m-%d %H:%M:%S %Z (UTC%z)')}",
             f"command line: {' '.join(_quote(str(a)) for a in (argv if argv is not None else sys.argv))}",
             f"folder: {os.getcwd()}",
             f"python: {sys.version.split()[0]} ({sys.executable}); {platform.platform()}; "
             f"{platform.machine()}; pid {os.getpid()}"
             + (f"; Wine {wine_version()}" if UNDER_WINE else ""),
             f"details on screen: {'yes' if verbose() else 'no'} (the files always have them; "
             f"{VERBOSE_ENV}=1 or --verbose shows them)",
             f"this log: {path}"]
    lines += list(header)
    names = ("DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "DESKTOP_SESSION", "LANG",
             "LC_ALL", "LANGUAGE", "SHELL", "TERM", "PATH", "PYTHONPATH", "PYTHONHOME", "VIRTUAL_ENV",
             "CONDA_PREFIX", "VULKAN_SDK", "VK_ICD_FILENAMES", "LD_LIBRARY_PATH", "APPIMAGE", "WSL_DISTRO_NAME",
             "C3W_WINE", "WINEPREFIX", "PROCESSOR_ARCHITECTURE", "NUMBER_OF_PROCESSORS", "SystemDrive",
             "TEMP", "TMPDIR", VERBOSE_ENV)
    env = [f"  {n}={_masked(n, os.environ[n])}" for n in names if n in os.environ]
    if env:
        lines.append("environment:\n" + "\n".join(env))
    _session_write("info", "\n".join(lines))
    install_crash_logging()
    return path


def install_crash_logging() -> None:
    """Every error nobody caught goes to the session log with its traceback: the main thread's
    (sys.excepthook), a worker thread's (threading.excepthook) and Python's own crashes (faulthandler: a
    segmentation fault or a hang killed by a signal, written straight into the log). The hooks hand on to the
    ones before them, so nothing else changes."""
    if _session.get("hooks"):
        return
    _session["hooks"] = True
    before_sys = sys.excepthook
    _session["before"] = (sys.excepthook, threading.excepthook, None)

    def sys_hook(kind, value, tb):
        if not issubclass(kind, KeyboardInterrupt):
            _session_write("error", "uncaught " + "".join(traceback.format_exception(kind, value, tb)).rstrip())
        before_sys(kind, value, tb)
    sys.excepthook = sys_hook
    before_thread = threading.excepthook

    def thread_hook(args):
        if args.exc_type is not SystemExit:
            name = args.thread.name if args.thread else "?"
            _session_write("error", f"uncaught in thread {name}: " + "".join(
                traceback.format_exception(args.exc_type, args.exc_value, args.exc_traceback)).rstrip())
        before_thread(args)
    threading.excepthook = thread_hook
    try:
        import faulthandler
        if _session["file"]:
            _session["fd"] = open(_session["file"], "a", encoding="utf-8", errors="replace")
            faulthandler.enable(file=_session["fd"], all_threads=True)
    except (OSError, RuntimeError, ValueError, AttributeError):
        pass
    try:
        import warnings
        before_warn = warnings.showwarning

        def warn_hook(message, category, filename, lineno, file=None, line=None):
            _session_write("debug", f"python warning: {category.__name__}: {message} ({filename}:{lineno})")
            before_warn(message, category, filename, lineno, file, line)
        warnings.showwarning = warn_hook
        _session["before"] = _session["before"][:2] + (before_warn,)
    except Exception:   # noqa: BLE001 - only a convenience
        pass


def stop_session_log() -> None:
    """Ends the session log here: the hooks put back as they were, faulthandler off and its file closed (the
    tests start and stop sessions of their own)."""
    before = _session.pop("before", None)
    if before:
        sys.excepthook, threading.excepthook = before[0], before[1]
        if before[2] is not None:
            import warnings
            warnings.showwarning = before[2]
    if _session.get("fd") is not None:
        try:
            import faulthandler
            faulthandler.disable()
        except (RuntimeError, ValueError):
            pass
        try:
            _session["fd"].close()
        except OSError:
            pass
    _session.update(file=None, fd=None, hooks=False, last_progress=0.0)


def session_info(text: str) -> None:
    """A line for the session log only (not on screen): versions and such."""
    _session_write("info", text)


def end_session_log(code: object = None) -> None:
    """The session's last line: how it ended."""
    if _session["file"]:
        _session_write("info", f"---- session ended ({'exit code ' + str(code) if code is not None else 'closed'})")


_console_state = {"progress": False}


def set_console_line_mode(on: bool) -> None:
    """Every message, progress included, as one whole line: for when another
    program reads this output line by line (the build inside WSL)."""
    _console_state["lines"] = on


def console_sink(kind: str, text: str, frac: Optional[float]) -> None:
    if kind == "debug" and not verbose():   # [acct2-ocr] 2026-11-22 details: in the log files, here only when asked
        return
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
            prefix = {"step": "==> ", "warn": "WARNING: ", "error": "ERROR: ", "out": "    ",
                      "debug": "  . "}.get(kind, "")
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
    prefix = {"step": "\n==> ", "warn": "WARNING: ", "error": "ERROR: ", "out": "    ", "debug": "  . "}.get(kind, "")
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
        self._log_context(cwd, env)   # [acct2-ocr] 2026-11-22 where and with what it ran
        started = time.monotonic()
        kwargs: dict = dict(cwd=str(cwd) if cwd else None, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        if IS_WINDOWS:
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | 0x08000000  # NO_WINDOW
        else:
            kwargs["start_new_session"] = True
        try:
            proc = subprocess.Popen(cmd, **kwargs)
        except FileNotFoundError:
            self.log.debug(f"not started: {cmd[0]} isn't there (PATH: {(env or os.environ).get('PATH', '')})")
            raise SetupError(f"Program not found: {cmd[0]}")
        except OSError as e:
            self.log.debug(f"not started: {type(e).__name__}: {e}")
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
        self.log.debug(f"started, pid {proc.pid}")
        tail: deque = deque(maxlen=400)   # the last lines, for failure_hint
        lines = 0
        for raw in iter(proc.stdout.readline, b""):
            line = raw.decode("utf-8", errors="replace").rstrip()
            tail.append(line)
            lines += 1
            if line_filter:
                line_filter(line)
            if not quiet and line:
                self.log.output(line)
        proc.stdout.close()
        rc = proc.wait()
        self._proc = None
        took = time.monotonic() - started
        if self.cancel.is_set():
            self.log.debug(f"canceled after {took:.1f} s: {cmd[0]}")
            raise Cancelled()
        self.log.debug(f"exit code {rc}{_rc_meaning(rc)} after {took:.1f} s, {lines} lines of output: "
                       f"{Path(cmd[0]).name}")
        if rc != 0:
            # A quiet command's output isn't in the log above: its end is, now that it failed.
            shown = [x for x in tail if x][-60:]
            if quiet and shown:   # a warning when it stops the job, a detail when the caller expects it may fail
                (self.log.warn if check else self.log.debug)(
                    f"{Path(cmd[0]).name} ended with exit code {rc}; its last {len(shown)} lines:\n"
                    + "\n".join(shown))
            self.log.debug("the whole command: " + " ".join(_quote(c) for c in cmd))
        if check and rc != 0:
            hint = self.failure_hint(list(tail)) if self.failure_hint else ""
            raise SetupError(f"Command failed (exit code {rc}): {' '.join(_quote(c) for c in cmd[:6])}"
                             f"{' ...' if len(cmd) > 6 else ''}\nSee the log above for details."
                             + (f"\n\n{hint}" if hint else ""))
        return rc

    def capture(self, cmd: Sequence, cwd: Optional[Path] = None, env: Optional[dict] = None,
                timeout: float = 60) -> tuple[int, str]:
        # [acct2-ocr] 2026-11-22 details: what was asked, its answer's start and how it ended
        text = " ".join(_quote(str(c)) for c in cmd)
        started = time.monotonic()
        try:
            kwargs: dict = {}
            if IS_WINDOWS:
                kwargs["creationflags"] = 0x08000000
            p = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None, env=env,
                               capture_output=True, timeout=timeout, stdin=subprocess.DEVNULL, **kwargs)
            out = (p.stdout or b"").decode("utf-8", "replace") + (p.stderr or b"").decode("utf-8", "replace")
            first = [x for x in out.splitlines() if x.strip()][:8]
            self.log.debug(f"$ {text}{f'  (in {cwd})' if cwd else ''}: exit code {p.returncode}"
                           f"{_rc_meaning(p.returncode)} after {time.monotonic() - started:.1f} s"
                           + ("".join("\n  | " + x[:300] for x in first)))
            return p.returncode, out
        except (OSError, subprocess.TimeoutExpired) as e:
            self.log.debug(f"$ {text}: not run or too slow ({type(e).__name__}: {e})")
            return -1, str(e)

    def _log_context(self, cwd: Optional[Path], env: Optional[dict]) -> None:
        """[acct2-ocr] 2026-11-22 the details of a command about to run: its folder and what its environment
        changes (secrets hidden)."""
        self.log.debug(f"in {cwd if cwd else os.getcwd()}")
        if env is None:
            return
        base = os.environ
        changed = [f"{k}={_masked(k, str(v))}" for k, v in sorted(env.items()) if base.get(k) != v]
        removed = sorted(k for k in base if k not in env)
        if changed:
            self.log.debug("environment set: " + "; ".join(changed))
        if removed:
            self.log.debug("environment without: " + ", ".join(removed))

    def _watch(self, proc: subprocess.Popen) -> None:
        while proc.poll() is None:
            if self.cancel.wait(0.5):
                kill_tree(proc)
                return


def _rc_meaning(rc: int) -> str:
    """[acct2-ocr] 2026-11-22 what an exit code says when it isn't a plain number: a signal (Linux) or a
    Windows status (an access violation, a missing DLL)."""
    if -64 <= rc < 0 and not IS_WINDOWS:
        try:
            return f" (killed by {signal.Signals(-rc).name})"
        except ValueError:
            return f" (killed by signal {-rc})"
    if IS_WINDOWS or rc > 0xFFFF or rc < -0xFFFF:
        status = rc & 0xFFFFFFFF
        known = {0xC0000005: "access violation", 0xC0000135: "a DLL it needs is missing",
                 0xC0000139: "a DLL it needs is the wrong version", 0xC000013A: "stopped with Ctrl+C",
                 0xC00000FD: "stack overflow", 0xC0000409: "stack buffer overrun", 0xC000001D: "illegal instruction",
                 0xC0000142: "a DLL failed to start", 0x40010004: "killed (debugger or task manager)"}
        if status in known:
            return f" (0x{status:08X}: {known[status]})"
        if status >= 0xC0000000:
            return f" (0x{status:08X})"
    return ""


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
            log.debug(f"{dest.name}: already downloaded ({human(dest.stat().st_size)}"
                      f"{', checksum matches' if sha256 else ', no checksum to compare'}): {dest}")
            return dest
        log.warn(f"{dest.name} doesn't match its checksum; downloading it again.")
        dest.unlink()
    APPROVAL.require([approval or Install(url, dest.name, str(dest.parent), "", url_host(url))], log)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    log.info(f"Downloading {url}")
    log.debug(f"to {dest}; expected sha256 {sha256 or '(none given)'}; "
              f"free there {free_text(dest.parent)}")
    req = urllib.request.Request(url, headers={"User-Agent": f"{APP_ID}/1.0"})
    h = hashlib.sha256()
    started = time.monotonic()
    done = 0
    try:
        with urllib.request.urlopen(req, timeout=60) as r, open(tmp, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            final = r.geturl() if hasattr(r, "geturl") else url
            log.debug(f"answer: HTTP {getattr(r, 'status', '?')}, {human(total) if total else 'size not given'}"
                      f"{f', from {final}' if final != url else ''}")
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
            # [acct2-decomp] 2026-11-21: a connection that closes early ends read() without an error, so a
            # download cut off halfway looked finished (and, without a checksum, was kept as the file).
            if total and done != total:
                raise ValueError(f"cut off after {human(done)} of {human(total)}")
    # URLError, timeouts, a dropped connection, no space; [acct2-decomp] 2026-11-21 http.client's own errors too
    # (IncompleteRead when a chunked reply ends early: an HTTPException, not an OSError, so it was a traceback).
    except (OSError, ValueError, http.client.HTTPException) as e:
        tmp.unlink(missing_ok=True)
        log.debug(f"download stopped after {human(done)} and {time.monotonic() - started:.1f} s: "
                  f"{type(e).__name__}: {e}")
        raise SetupError(f"Download failed: {url} ({e}). Check the internet connection and build again.")
    took = max(time.monotonic() - started, 1e-3)
    got = h.hexdigest().lower()
    if sha256 and got != sha256.lower():
        tmp.unlink(missing_ok=True)
        log.debug(f"checksum: got sha256 {got}, expected {sha256.lower()}")
        raise SetupError(f"Checksum mismatch for {url}")
    log.debug(f"downloaded {human(done)} in {took:.1f} s ({human(done / took)}/s); sha256 {got}"
              f"{' (matches)' if sha256 else ''}")
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
              hardlink: bool = False, skip: Callable[[Path], bool] = lambda p: False,
              overwrite: bool = False) -> None:
    """Copy (or hard-link) a folder, skipping files that already match in size.
    Never touches `src`. [acct2-decomp] 2026-11-21 `overwrite`: every file is
    written again, also one of the same size (for `dst` holding another copy's
    files: a file of the same size can hold other bytes)."""
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
        if d.exists() and d.stat().st_size == size and (not overwrite or _same_file(s, d)):
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


def _same_file(a: Path, b: Path) -> bool:
    """[acct2-decomp] 2026-11-21: `a` and `b` are one file (a hard link made by an earlier copy_tree)."""
    try:
        return os.path.samefile(a, b)
    except OSError:
        return False


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, indent=2), encoding="utf-8")
    tmp.replace(path)


def read_json(path: Path) -> dict:
    """A JSON file's object: {} when the file is missing, unreadable, not JSON, or holds something else (a list, a
    string, null). [acct2-decomp] 2026-11-21 (owner 11:18 "review everything"): every caller takes a dict, and a
    settings.json holding null, a string or a list of values stopped the window before it opened."""
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


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


def start_and_collect(args: Sequence[str], on_exit: Optional[Callable[[int], None]] = None,
                      **kwargs) -> subprocess.Popen:
    """Starts a program that runs on its own (the game, a file manager) and
    collects it when it ends, on a thread of its own. Without that, a
    program started from here that has ended stays in the system's process
    list as a "zombie" until this tool closes: the game looked as if it was
    still running (owner, 2026-10-16: "ghost application"). `on_exit` gets
    the exit code (on that thread)."""
    # [acct2-ocr] 2026-11-22 the session log says what was started (the game from Play, a file manager), from
    # where, and how it ended
    shown = " ".join(_quote(str(a)) for a in args)
    _session_write("info", f"started: {shown}" + (f"  (in {kwargs['cwd']})" if kwargs.get("cwd") else ""))
    env = kwargs.get("env")
    if env is not None:
        changed = [f"{k}={_masked(k, str(v))}" for k, v in sorted(env.items()) if os.environ.get(k) != v]
        if changed:
            _session_write("debug", "its environment set: " + "; ".join(changed))
    try:
        proc = subprocess.Popen(list(args), **kwargs)
    except (OSError, ValueError) as e:
        _session_write("error", f"couldn't start {args[0]}: {type(e).__name__}: {e}")
        raise
    started = time.monotonic()
    _session_write("debug", f"pid {proc.pid}")

    def collect() -> None:
        code = proc.wait()
        _session_write("info" if code == 0 else "warn",
                       f"{Path(str(args[0])).name} (pid {proc.pid}) ended: exit code {code}{_rc_meaning(code)} after "
                       f"{time.monotonic() - started:.0f} s")
        if on_exit:
            try:
                on_exit(code)
            except Exception:  # noqa: BLE001 - a report only
                pass
    threading.Thread(target=collect, daemon=True).start()
    return proc


def open_in_file_manager(path: Path) -> None:
    path = Path(path)
    if IS_WINDOWS:
        os.startfile(str(path))  # type: ignore[attr-defined]
    else:
        opener = shutil.which("xdg-open") or shutil.which("gio")
        if opener:
            args = [opener, str(path)] if opener.endswith("xdg-open") else [opener, "open", str(path)]
            start_and_collect(args, start_new_session=True,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

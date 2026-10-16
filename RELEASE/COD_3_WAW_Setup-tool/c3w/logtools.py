"""Reading, filtering and packing up logs (the game's and this tool's).

Two formats are read:

    [2026-10-12 18:49:54.416] [info] [core] [t548] text      the game (spdlog)
    18:10:43 [info] text                                       this tool (common.Log)

A line without that start continues the entry above it (a message over
several lines). Where the logs are:

    <output>/logs/setup-*.log               this tool's builds (the last few)
    <output>/userdata/logs/cod3.log         the game; cod3.1.log ... are older parts
    <output>/userdata/logs/report-*.txt     the game's `cod3_report` reports

make_bug_report() zips the newest of each, cod3.toml and this tool's
settings, with personal details replaced by placeholders (<HOME>, <USER>,
<HOST>, <OUTPUT>, <PROJECT>, <ISO>, <WAW>, <WORK>, <IP>). Game files are
never included.
"""
from __future__ import annotations

import getpass
import os
import platform
import re
import socket
import sys
import time
import zipfile
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set, Tuple

from . import common

# Levels as entries give them: the game's (spdlog) and this tool's (common.Log).
LEVELS = ("trace", "debug", "info", "step", "out", "progress", "warn", "error", "critical")
_LEVEL_NAMES = {"warning": "warn", "err": "error", "crit": "critical", "dbg": "debug", "trc": "trace"}
# A filter for "error" also shows "critical" ones.
_LEVEL_GROUP = {"critical": "error"}

GAME_LINE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)\] \[(\w+)\] \[([^\]]*)\] "
                       r"(?:\[t\d+\] )?(.*)$")
SETUP_LINE = re.compile(r"^(\d{2}:\d{2}:\d{2}) \[(\w+)\] (.*)$")
SESSION_MARK = "---- session ----"
EXIT_MARK = "; exiting"

GAME_LOG = Path("userdata") / "logs" / "cod3.log"
SETUP_LOGS = Path("logs")
REPORTS = Path("userdata") / "logs"

MAX_LINE = 4000                      # characters kept of one line
MAX_SESSION = 8 * 1024 * 1024        # bytes of the game's last session in a bug report
MAX_TEXT = 4 * 1024 * 1024           # bytes of any other text file in a bug report
SESSION_LEAD = 30.0                  # seconds before the session line that still belong to it


@dataclass
class Entry:
    time: str      # as written in the log ("18:10:43" or "2026-10-12 18:49:54.416"), "" if none
    level: str     # one of LEVELS ("warning" is "warn"), "info" for lines without one
    source: str    # the game's part ("core", "gpu", ...); "" for this tool's logs
    text: str      # the message (more lines joined with "\n")
    raw: str       # the line(s) as read


def parse_line(line: str) -> Optional[Entry]:
    """The entry a line starts, or None for a line that continues one."""
    m = GAME_LINE.match(line)
    if m:
        level = m.group(2).lower()
        return Entry(m.group(1), _LEVEL_NAMES.get(level, level), m.group(3), m.group(4), line)
    m = SETUP_LINE.match(line)
    if m:
        level = m.group(2).lower()
        return Entry(m.group(1), _LEVEL_NAMES.get(level, level), "", m.group(3), line)
    return None


def parse_lines(lines: Iterable[str]) -> List[Entry]:
    entries: List[Entry] = []
    for line in lines:
        line = line.rstrip("\r\n")
        if len(line) > MAX_LINE:
            line = line[:MAX_LINE] + " ..."
        entry = parse_line(line)
        if entry:
            entries.append(entry)
        elif entries and line.strip():
            entries[-1].text += "\n" + line
            entries[-1].raw += "\n" + line
        elif line.strip():
            entries.append(Entry("", "info", "", line, line))
    return entries


def tail_lines(path: Path, max_lines: int) -> List[str]:
    """The last max_lines lines of a file (read from the end, so a big log
    is cheap). A line cut by where reading started is left out."""
    block = 256 * 1024
    chunks: List[bytes] = []
    newlines = 0
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        pos = f.tell()
        while pos > 0 and newlines <= max_lines:
            step = min(block, pos)
            pos -= step
            f.seek(pos)
            chunk = f.read(step)
            chunks.append(chunk)
            newlines += chunk.count(b"\n")
    lines = b"".join(reversed(chunks)).decode("utf-8", "replace").splitlines()
    if pos > 0 and lines:
        lines = lines[1:]   # started mid-line
    return lines[-max_lines:] if max_lines > 0 else []


def read_entries(path: Path, max_lines: int = 20000) -> List[Entry]:
    """The last max_lines lines of a log, as entries. [] if it can't be read."""
    try:
        return parse_lines(tail_lines(Path(path), max_lines))
    except OSError:
        return []


def filter_entries(entries: Iterable[Entry], levels: Set[str], query: str) -> List[Entry]:
    """Entries whose level is in levels (all when empty; "error" includes
    "critical") and whose line contains query (case-insensitive; all when
    empty)."""
    levels = {_LEVEL_NAMES.get(lv.lower(), lv.lower()) for lv in (levels or ())}
    q = (query or "").strip().lower()
    out = []
    for e in entries:
        if levels and e.level not in levels and _LEVEL_GROUP.get(e.level) not in levels:
            continue
        if q and q not in e.raw.lower():
            continue
        out.append(e)
    return out


# ---------------------------------------------------------------------------
# Where the logs are
# ---------------------------------------------------------------------------
def _stamp(name: str) -> str:
    """"20261012-181043" in a file name as "2026-10-12 18:10"."""
    m = re.search(r"(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})", name)
    return f"{m.group(1)}-{m.group(2)}-{m.group(3)} {m.group(4)}:{m.group(5)}" if m else name


def _rotated(out: Path) -> List[Path]:
    """cod3.1.log, cod3.2.log ... (newest first)."""
    folder = (out / GAME_LOG).parent
    found = []
    try:
        for p in folder.iterdir():
            m = re.fullmatch(r"cod3\.(\d+)\.log", p.name)
            if m and p.is_file():
                found.append((int(m.group(1)), p))
    except OSError:
        return []
    return [p for _, p in sorted(found)]


def _newest(folder: Path, pattern: str) -> List[Path]:
    try:
        return sorted((p for p in folder.glob(pattern) if p.is_file()), key=lambda p: p.name, reverse=True)
    except OSError:
        return []


def known_logs(out_dir: Path) -> List[Tuple[str, Path]]:
    """(label, path) for every log the tool knows about in an output folder:
    the game's log, the game's reports, the setup logs (newest first) and
    the older parts of the game's log."""
    out = Path(out_dir)
    logs: List[Tuple[str, Path]] = []
    if (out / GAME_LOG).is_file():
        logs.append(("Game log", out / GAME_LOG))
    logs += [(f"Game report {_stamp(p.name)}", p) for p in _newest(out / REPORTS, "report-*.txt")]
    logs += [(f"Setup log {_stamp(p.name)}", p) for p in _newest(out / SETUP_LOGS, "setup-*.log")]
    logs += [(f"Game log, older part {i}", p) for i, p in enumerate(_rotated(out), 1)]
    return logs


# ---------------------------------------------------------------------------
# The game's last session
# ---------------------------------------------------------------------------
def _time_of(line: str) -> Optional[float]:
    m = GAME_LINE.match(line)
    if not m:
        return None
    try:
        return datetime.strptime(m.group(1)[:19], "%Y-%m-%d %H:%M:%S").timestamp()
    except ValueError:
        return None


def _read_lines(path: Path) -> List[str]:
    try:
        with open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
            return f.read().splitlines()
    except OSError:
        return []


def _session_start(lines: List[str], header: int) -> int:
    """Where the run whose session line is lines[header] starts: the lines
    just before it from the same start-up (less than SESSION_LEAD seconds
    before it, after the last run's "exiting")."""
    start = header
    t_header = _time_of(lines[header])
    for i in range(header - 1, -1, -1):
        line = lines[i]
        t = _time_of(line)
        if t is None:
            if GAME_LINE.match(line):
                break
            continue   # more lines of the entry above: in only if that entry is
        if SESSION_MARK in line or EXIT_MARK in line or t_header is None or not 0 <= t_header - t <= SESSION_LEAD:
            break
        start = i
    return start


def last_session(out_dir: Path) -> List[str]:
    """The lines of the game's last run: from a little before its last
    "---- session ----" line (the start-up lines just before it) to the end
    of the log, across the rotated parts if the run filled more than one.
    Without such a line, the end of the log."""
    out = Path(out_dir)
    parts = [p for p in [out / GAME_LOG] + _rotated(out) if p.is_file()]   # newest first
    lines: List[str] = []
    header, used = -1, 0
    while header < 0 and used < len(parts):
        lines = _read_lines(parts[used]) + lines
        used += 1
        header = max((i for i, line in enumerate(lines) if SESSION_MARK in line), default=-1)
    if header < 0:
        return lines[-20000:]
    while True:
        start = _session_start(lines, header)
        if start > 0 or used >= len(parts):
            return lines[start:]
        # The run's start-up lines may be at the end of the older part.
        older = _read_lines(parts[used])
        used += 1
        lines = older + lines
        header += len(older)


def _cap(text: str, limit: int) -> str:
    """At most about `limit` bytes: the start and the (longer) end, with a
    note where lines were left out."""
    data = text.encode("utf-8")
    if len(data) <= limit:
        return text
    head = data[:limit // 8].decode("utf-8", "ignore").rsplit("\n", 1)[0]
    tail = data[-(limit - limit // 8):].decode("utf-8", "ignore").split("\n", 1)[-1]
    left_out = text.count("\n") - head.count("\n") - tail.count("\n")
    return f"{head}\n... ({left_out} lines left out) ...\n{tail}"


# ---------------------------------------------------------------------------
# Personal details
# ---------------------------------------------------------------------------
_NOT_NAMES = {"root", "user", "users", "admin", "administrator", "steam", "game", "games", "info", "test",
              "guest", "default", "public", "home", "localhost", "wine", "deck", "player", "system", "local"}
# An IPv4 address (not a version number such as 0.10.0.0 or 1.2.3.4-dev).
_IPV4 = re.compile(r"(?<![\w.])((?:25[0-5]|2[0-4]\d|1?\d?\d)(?:\.(?:25[0-5]|2[0-4]\d|1?\d?\d)){3})"
                   r"(?![\w-]|\.\d)")
_KEEP_IPS = {"127.0.0.1", "0.0.0.0", "255.255.255.255"}
# Home folders whoever's they are (logs can come from another PC or user):
# /home/<name>, /Users/<name>, C:\Users\<name> (any slashes, JSON-escaped too;
# a Windows name can have spaces), and removable drives under /media/<name>
# and /run/media/<name>. Group 1 is the start, group 2 the name.
_SEP = r"[\\/]{1,2}"
_NAME = r"[^\\/\s\"'<>|:*?]+"
_SPACED_NAME = r"[^\\/\s\"'<>|:*?](?:[^\\/\"'<>|:*?\r\n]{0,62}[^\\/\s\"'<>|:*?])?(?=[\\/])"
_ANY_HOME = re.compile(r"(?<![\w])([A-Za-z]:" + _SEP + r"(?:Users|Documents and Settings)" + _SEP + r")(" +
                       _SPACED_NAME + "|" + _NAME + r")|(?<![\w])(" + _SEP + r"(?:home|Users)" + _SEP + r")(" +
                       _NAME + ")", re.IGNORECASE)
_ANY_MEDIA = re.compile(r"(?<![\w])(" + _SEP + r"(?:run" + _SEP + r")?media" + _SEP + r")(" + _NAME + ")")
# A folder whose path ends where an output folder's own files start (the
# build's output folder, wherever it was when the log was written), and the
# project folder (where this tool's folder starts).
_PART = r"[^\\/\s\"'<>|*?:]+"
_ANY_OUTPUT = re.compile(r"(?<![\w:])((?:[A-Za-z]:)?(?:" + _SEP + _PART + r")+?)(?=" + _SEP +
                         r"(?:(?:CoD3-x86_64\.AppImage|userdata|data" + _SEP + r"(?:cod3|waw)|tools" + _SEP +
                         r"rip(?:\.exe)?|bin-windows|cod3\.toml|cod3_waw\.json|launcher|Launcher\.(?:sh|bat)|"
                         r"assets)(?![\w.-])|logs" + _SEP + r"setup-\d))")
_ANY_PROJECT = re.compile(r"(?<![\w:])((?:[A-Za-z]:)?(?:" + _SEP + _PART + r")+?)(?=" + _SEP +
                          r"COD_3_WAW_Setup-tool(?![\w.-]))")


def _identity() -> Tuple[List[str], List[str], List[str]]:
    """This PC's home folders, user names and host names (what to hide)."""
    homes = {str(Path.home())}
    for var in ("HOME", "USERPROFILE"):
        if os.environ.get(var):
            homes.add(os.environ[var])
    if os.environ.get("HOMEDRIVE") and os.environ.get("HOMEPATH"):
        homes.add(os.environ["HOMEDRIVE"] + os.environ["HOMEPATH"])
    users = {os.environ.get(v, "") for v in ("USER", "USERNAME", "LOGNAME")}
    try:
        users.add(getpass.getuser())
    except Exception:  # noqa: BLE001 - no user name to find is fine
        pass
    hosts = {os.environ.get(v, "") for v in ("COMPUTERNAME", "HOSTNAME")}
    for get in (socket.gethostname, platform.node):
        try:
            hosts.add(get())
        except Exception:  # noqa: BLE001
            pass
    hosts |= {h.split(".")[0] for h in hosts if h}
    return sorted(h for h in homes if h), sorted(u for u in users if u), sorted(h for h in hosts if h)


def _path_forms(path: str) -> Set[str]:
    """The ways a folder's path is written in logs and settings: as is,
    with / or \\, JSON- or TOML-escaped, and as Wine shows a Linux path
    (Z:\\...)."""
    p = path.strip().rstrip("/\\")
    if len(p) < 4 or re.fullmatch(r"[A-Za-z]:", p):
        return set()   # a drive or "/": too short to hide without hiding everything
    fwd, back = p.replace("\\", "/"), p.replace("/", "\\")
    forms = {p, fwd, back, back.replace("\\", "\\\\")}
    if fwd.startswith("/"):
        forms |= {"Z:" + fwd, "Z:" + back, ("Z:" + back).replace("\\", "\\\\")}
    return forms


class Scrubber:
    """Replaces personal details in text with placeholders: the given paths
    (longest first), then home folders, output and project folders this PC
    doesn't know (moved, or from another PC), user and host names as whole
    words (also the names seen in any home folder path, so a cut-off path
    elsewhere doesn't show one), and IP addresses."""

    def __init__(self, paths: Iterable[Tuple[str, str]] = (), users: Iterable[str] = (),
                 hosts: Iterable[str] = ()):
        self.forms: Dict[str, str] = {}
        self._compiled: Optional[list] = None
        for value, placeholder in paths:
            self._add_path(str(value), placeholder)
        self.names: Dict[str, str] = {}
        for h in hosts:
            self._add_name(h, "<HOST>")
        for u in users:
            self._add_name(u, "<USER>")

    def _add_path(self, path: str, placeholder: str) -> None:
        for form in _path_forms(path):
            if form.lower() not in {f.lower() for f in self.forms}:
                self.forms[form] = placeholder
                self._compiled = None

    def _add_name(self, name: str, placeholder: str) -> None:
        name = name.strip()
        if len(name) >= 3 and name.lower() not in _NOT_NAMES and name.lower() not in self.names:
            self.names[name.lower()] = placeholder

    def learn(self, text: str) -> None:
        """Note the user names in home folder paths and the output and
        project folders the text shows, to hide them everywhere (also where a
        path is cut off or stands alone). Call it on every file first."""
        for m in _ANY_HOME.finditer(text):
            self._add_name(m.group(2) or m.group(4) or "", "<USER>")
        for m in _ANY_MEDIA.finditer(text):
            self._add_name(m.group(2), "<USER>")
        for m in _ANY_PROJECT.finditer(text):
            self._add_path(m.group(1), "<PROJECT>")
        for m in _ANY_OUTPUT.finditer(text):
            if "<" not in m.group(1):
                self._add_path(m.group(1), "<OUTPUT>")

    def _patterns(self) -> list:
        if self._compiled is None:
            # Longest first, so a folder inside the home folder keeps its own name.
            ordered = sorted(self.forms.items(), key=lambda kv: -len(kv[0]))
            self._compiled = [(re.compile(re.escape(form) + r"(?![\w-])", re.IGNORECASE), ph)
                              for form, ph in ordered]
        return self._compiled

    def __call__(self, text: str) -> str:
        self.learn(text)
        for pattern, placeholder in self._patterns():
            text = pattern.sub(placeholder, text)
        # Then the same kinds of folders this PC doesn't know about.
        text = _ANY_PROJECT.sub("<PROJECT>", text)
        text = _ANY_OUTPUT.sub("<OUTPUT>", text)
        text = _ANY_HOME.sub("<HOME>", text)
        text = _ANY_MEDIA.sub(lambda m: m.group(1) + "<USER>", text)
        for name in sorted(self.names, key=len, reverse=True):
            text = re.sub(r"(?<![\w])" + re.escape(name) + r"(?![\w])", self.names[name], text, flags=re.IGNORECASE)
        return _IPV4.sub(lambda m: m.group(1) if m.group(1) in _KEEP_IPS or m.group(1).startswith("0.")
                         else "<IP>", text)


def scrubber_for(out_dir: Path, settings: Optional[dict] = None) -> Scrubber:
    """A Scrubber for this PC, the output folder, the project folder and the
    paths in the tool's settings (the disc, World at War, the work folder)."""
    homes, users, hosts = _identity()
    settings = settings if isinstance(settings, dict) else {}
    paths: List[Tuple[str, str]] = []
    for p in {str(out_dir), str(common.resolved(Path(out_dir)))}:
        paths.append((p, "<OUTPUT>"))
    project = common.project_dir()
    paths += [(str(project), "<PROJECT>"), (str(common.resolved(project)), "<PROJECT>")]
    named = {"cod3_source": "<ISO>", "waw_steam": "<WAW>", "waw_xbox360": "<WAW>", "waw_pc_disc": "<WAW>",
             "output_dir": "<OUTPUT>", "work_dir": "<WORK>"}
    for key, placeholder in named.items():
        value = settings.get(key)
        if isinstance(value, str) and value.strip():
            paths.append((value, placeholder))
    packs = settings.get("waw_map_packs")
    if isinstance(packs, list):
        paths += [(v, "<MAP_PACK>") for v in packs if isinstance(v, str) and v.strip()]
    paths += [(h, "<HOME>") for h in homes]
    return Scrubber(paths, users, hosts)


# ---------------------------------------------------------------------------
# The bug report
# ---------------------------------------------------------------------------
def _read_text(path: Path, limit: int) -> Optional[str]:
    try:
        size = path.stat().st_size
        if size > limit:
            return "\n".join(tail_lines(path, 200000))[-limit:]
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None


def make_bug_report(out_dir: Path, dest_zip: Path, settings_file: Optional[Path] = None) -> Tuple[Path, List[str]]:
    """Zip what helps with a bug report: the newest setup log, the game's
    last session, the newest game report, cod3.toml, the build's record and
    this tool's settings, plus a check of the build (c3w/repair.py), all with
    personal details replaced (see Scrubber). Returns (zip, names inside).
    settings_file: this tool's settings (default: config/settings.json)."""
    out = Path(out_dir)
    dest = Path(dest_zip)
    if settings_file is None:
        settings_file = common.config_dir() / "settings.json"
    settings = common.read_json(settings_file) if Path(settings_file).is_file() else {}
    scrub = scrubber_for(out, settings)
    files: List[Tuple[str, str]] = []

    setup = _newest(out / SETUP_LOGS, "setup-*.log")
    if setup:
        text = _read_text(setup[0], MAX_TEXT)
        if text is not None:
            files.append(("setup.log", text))
    session = last_session(out)
    if session:
        files.append(("game-last-session.log", _cap("\n".join(session) + "\n", MAX_SESSION)))
    reports = _newest(out / REPORTS, "report-*.txt")
    if reports:
        text = _read_text(reports[0], MAX_TEXT)
        if text is not None:
            files.append(("game-report.txt", text))
    for name, path in (("cod3.toml", out / "cod3.toml"), ("cod3_waw.json", out / "cod3_waw.json"),
                       ("setup-settings.json", Path(settings_file))):
        text = _read_text(path, MAX_TEXT) if path.is_file() else None
        if text is not None:
            files.append((name, text))
    try:
        from . import repair
        report = repair.check_install(out)
        check = [report.summary()] + [f"[{i.severity}] {i.title}: {i.detail}" for i in report.issues]
        files.append(("check.txt", "\n".join(check) + "\n"))
    except Exception as e:  # noqa: BLE001 - the report is still useful without the check
        files.append(("check.txt", f"The check couldn't run: {type(e).__name__}\n"))

    from .pipeline import TOOL_VERSION
    about = [f"Bug report made by {common.APP_NAME} {TOOL_VERSION} on {time.strftime('%Y-%m-%d %H:%M')}",
             f"System: {platform.system()} {platform.machine()}" + (" (Wine)" if common.UNDER_WINE else ""),
             f"Python: {sys.version.split()[0]}",
             "Personal details (folders, user and computer names, IP addresses) are replaced by placeholders",
             "such as <HOME>, <USER>, <HOST> and <OUTPUT>. No game files are included.", "",
             "Files:"] + [f"  {name}" for name, _ in files]
    files.insert(0, ("README.txt", "\n".join(about) + "\n"))

    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".tmp")
    try:
        for _name, text in files:
            scrub.learn(text)
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for name, text in files:
                z.writestr(name, scrub(text))
        os.replace(tmp, dest)
    except OSError as e:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise common.SetupError(f"Couldn't write {dest.name}: {e.strerror or e}") from None
    return dest, [name for name, _ in files]

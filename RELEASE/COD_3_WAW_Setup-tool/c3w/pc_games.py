"""Call of Duty 2 and United Offensive: the Rust engine's tools for them.

- `newcopy` (engine/tools/newcopy, docs/NEW_COPY.md) finds the player's own
  install (Steam on Windows and Linux, a CB copy, any folder) and makes a
  "new copy" for the Rust engine: only the game's data archives (.pk3 /
  .iwd) and version text. Program files are never read, copied or run, so
  Steam's DRM (which lives in the executables) never comes into it.
- `iw-server` (engine/tools/iw_server, docs/PC_SERVER.md) is our own LAN
  server that the games' PC clients can list and join. It reads the
  protocol tables from the game's executable (only read: never run,
  copied or changed); it is given that file with --exe.

Both are built with cargo from the project's engine/ folder, like the
ripper (c3w/rippers.py), and copied into the output folder's tools/. This
module has no Tk: the checks, command lines and finding installs are
tested without a window.
"""
from __future__ import annotations

import filecmp
import os
import re
import shutil
import socket
import subprocess
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from . import deps, rippers, waw
from .common import (APPROVAL, IS_WINDOWS, Install, Log, Runner, SetupError, exe_name, host_triple, kill_tree,
                     overlaps)


@dataclass(frozen=True)
class PcGame:
    id: str                 # the tab's id, and newcopy's name for the game
    name: str
    server_key: str         # iw-server's --game value
    steam_app: int
    steam_folder: str       # its folder in steamapps/common
    version: str            # the patch the Rust engine targets
    executable: str         # the multiplayer program iw-server reads its tables from (never run)
    # How a folder is recognized: (folder, archive name prefix, extension)
    # that must all be there. Only names are looked at.
    archives: Tuple[Tuple[str, str, str], ...]
    maps: Tuple[str, ...]   # the stock multiplayer maps (any other name can be typed)
    default_map: str


COD2 = PcGame("cod2", "Call of Duty 2", "cod2", 2630, "Call of Duty 2", "1.3", "CoD2MP_s.exe",
              (("main", "iw_", ".iwd"),),
              ("mp_breakout", "mp_brecourt", "mp_burgundy", "mp_carentan", "mp_dawnville", "mp_decoy",
               "mp_downtown", "mp_farmhouse", "mp_harbor", "mp_leningrad", "mp_matmata", "mp_railyard",
               "mp_rhine", "mp_toujane", "mp_trainstation"),
              "mp_toujane")
CODUO = PcGame("coduo", "United Offensive", "uo", 2640, "Call of Duty", "1.51", "CoDUOMP.exe",
               (("uo", "pakuo", ".pk3"), ("main", "pak", ".pk3")),
               ("mp_arnhem", "mp_berlin", "mp_cassino", "mp_foy", "mp_italy", "mp_kharkov", "mp_kursk",
                "mp_ponyri", "mp_rhinevalley", "mp_sicily", "mp_uo_carentan", "mp_uo_dawnville", "mp_uo_depot",
                "mp_uo_harbor", "mp_uo_hurtgen", "mp_uo_powcamp", "mp_uo_stanjel"),
               "mp_arnhem")
PC_GAMES: Dict[str, PcGame] = {g.id: g for g in (COD2, CODUO)}

DEFAULT_PORT = 28960          # both games' own default
PORT_RANGE = (1024, 65535)    # below 1024 needs administrator rights on Linux
MAX_CLIENTS_RANGE = (1, 64)   # what iw-server takes
DEFAULT_MAX_CLIENTS = 16
MAX_HOSTNAME = 48
DEFAULT_HOSTNAME = "Rust LAN server"
COPIES_FOLDER = "copies"      # the new copies' default place: <output>/copies/<game>
# The languages newcopy can pick when an install has several ("" = the one there is).
LANGUAGES = ("", "english", "french", "german", "italian", "spanish", "russian", "polish")

# The Rust programs: binary -> (cargo package, folders whose sources it is built from).
TOOLS: Dict[str, Tuple[str, Tuple[str, ...]]] = {
    "newcopy": ("newcopy", ("tools/newcopy", "crates/game_sources")),
    "iw-server": ("iw_server", ("tools/iw_server", "crates/iw_pc", "crates/game_sources")),
}

_MAP_NAME = re.compile(r"^[A-Za-z0-9_\-]{1,64}$")


# ---------------------------------------------------------------------------
# Checks: each returns "" when the value is fine, else a short reason
# ---------------------------------------------------------------------------
def check_map(name: str) -> str:
    name = (name or "").strip()
    if not name:
        return "Pick a map."
    if not _MAP_NAME.match(name):
        return "Map names use letters, digits, _ and - only."
    return ""


def parse_int(text, low: int, high: int) -> Optional[int]:
    """`text` as a whole number from low to high, or None."""
    try:
        value = int(str(text).strip())
    except (TypeError, ValueError):
        return None
    return value if low <= value <= high else None


def check_port(text) -> str:
    return "" if parse_int(text, *PORT_RANGE) is not None else \
        f"Port: a number from {PORT_RANGE[0]} to {PORT_RANGE[1]}."


def check_max_clients(text) -> str:
    return "" if parse_int(text, *MAX_CLIENTS_RANGE) is not None else \
        f"Players: {MAX_CLIENTS_RANGE[0]} to {MAX_CLIENTS_RANGE[1]}."


def check_hostname(name: str) -> str:
    """The name the LAN list shows. The games' info strings can't hold \\,
    " or ; and show plain ASCII (^1 ... color codes are fine)."""
    name = name or ""
    if not name.strip():
        return "Type a server name."
    if len(name) > MAX_HOSTNAME:
        return f"Server name: {MAX_HOSTNAME} characters at most."
    if any(c in '\\";' for c in name) or any(not (32 <= ord(c) < 127) for c in name):
        return 'Server name: plain letters, digits and signs; no \\, " or ;.'
    return ""


def _folder_problem(text: str, what: str) -> Tuple[Optional[Path], str]:
    text = (text or "").strip()
    if not text:
        return None, f"Choose the {what}."
    p = Path(text)
    if not p.is_absolute():
        return None, f"The {what} must be a full path."
    return p, ""


def check_install(text: str, game: PcGame) -> str:
    """An install folder, or "" (empty: newcopy looks in Steam)."""
    if not (text or "").strip():
        return ""
    folder, why = _folder_problem(text, "game folder")
    if why:
        return why
    assert folder is not None
    if not folder.is_dir():
        return "The game folder isn't there."
    ok, message = recognize(folder, game)
    return "" if ok else message


def check_copy_folder(text: str, install: str) -> str:
    """Where the new copy goes: a full path, never inside the game's folder
    (newcopy refuses that too) and never holding it."""
    out, why = _folder_problem(text, "copy folder")
    if why:
        return why
    assert out is not None
    if install.strip() and Path(install.strip()).is_absolute() and overlaps(out, Path(install.strip())):
        return "The copy can't go inside the game's folder (or hold it)."
    if out.exists() and not out.is_dir():
        return "The copy folder is a file."
    return ""


@dataclass
class ServerSettings:
    map: str = ""
    port: str = str(DEFAULT_PORT)
    hostname: str = DEFAULT_HOSTNAME
    max_clients: str = str(DEFAULT_MAX_CLIENTS)

    def problems(self) -> List[str]:
        return [p for p in (check_map(self.map), check_port(self.port), check_hostname(self.hostname),
                            check_max_clients(self.max_clients)) if p]


# ---------------------------------------------------------------------------
# Command lines (lists: no shell, so spaces in paths are safe)
# ---------------------------------------------------------------------------
def newcopy_find(exe: Path, install: str = "") -> List[str]:
    cmd = [str(exe), "find"]
    if install.strip():
        cmd += ["--from", str(Path(install.strip()))]
    return cmd


def newcopy_make(exe: Path, game: PcGame, out: str, install: str = "", full_copy: bool = False,
                 language: str = "") -> List[str]:
    """`newcopy make <game> --out <out> [--from <install>] [--copy]
    [--language <lang>]`. Raises SetupError on a value newcopy would refuse."""
    why = check_copy_folder(out, install) or check_install(install, game)
    if why:
        raise SetupError(why)
    if language and language not in LANGUAGES:
        raise SetupError(f"Unknown language: {language}")
    cmd = [str(exe), "make", game.id, "--out", str(Path(out.strip()))]
    if install.strip():
        cmd += ["--from", str(Path(install.strip()))]
    if full_copy:
        cmd.append("--copy")
    if language:
        cmd += ["--language", language]
    return cmd


def newcopy_check(exe: Path, copy_dir: str) -> List[str]:
    folder, why = _folder_problem(copy_dir, "copy folder")
    if why:
        raise SetupError(why)
    return [str(exe), "check", str(folder)]


def server_command(exe: Path, game: PcGame, settings: ServerSettings,
                   game_exe: Optional[Path] = None) -> List[str]:
    """`iw-server --game cod2|uo --map <map> --port <n> --hostname <name>
    --max-clients <n> [--exe <game's program>]`. Raises SetupError on a
    value it would refuse. `game_exe` is the file iw-server reads its tables
    from (only read; see game_executable)."""
    problems = settings.problems()
    if problems:
        raise SetupError(problems[0])
    cmd = [str(exe), "--game", game.server_key, "--map", settings.map.strip(),
           "--port", str(parse_int(settings.port, *PORT_RANGE)), "--hostname", settings.hostname,
           "--max-clients", str(parse_int(settings.max_clients, *MAX_CLIENTS_RANGE))]
    if game_exe is not None:
        cmd += ["--exe", str(game_exe)]
    return cmd


def game_executable(install: str, game: PcGame) -> Optional[Path]:
    """The game's multiplayer program in its folder (any case), for
    iw-server's --exe. iw-server only reads its tables; it is never run."""
    folder = Path(install.strip()) if install.strip() else None
    if folder is None or not folder.is_absolute():
        return None
    try:
        exe = _child(folder, game.executable)
    except OSError:
        return None
    return exe if exe is not None and exe.is_file() else None


def server_env(base: Optional[dict], install: str, game_exe: Optional[Path] = None) -> dict:
    """The server's environment. With the game's program given by --exe,
    nothing more is needed. Else the install folder goes in IW_PC_INPUT,
    where iw-server looks for that program (a list of folders, so only a
    folder without the system's list separator); without either it looks
    in Steam."""
    env = dict(base if base is not None else os.environ)
    folder = install.strip()
    if game_exe is None and folder and os.pathsep not in folder:
        env["IW_PC_INPUT"] = str(Path(folder))
    else:
        env.pop("IW_PC_INPUT", None)
    return env


def default_copy_dir(output_dir: str) -> str:
    base = Path(output_dir.strip()) if output_dir.strip() else None
    return str(base / COPIES_FOLDER) if base else ""


# ---------------------------------------------------------------------------
# Finding installs (names only: no file is opened)
# ---------------------------------------------------------------------------
def _child(folder: Path, name: str) -> Optional[Path]:
    return waw._child(folder, name)   # case-insensitive: Windows copies on Linux


def recognize(folder: Path, game: PcGame) -> Tuple[bool, str]:
    """Is `folder` an install of `game`? Looks only at file names: the data
    folders and their archives, as newcopy does."""
    folder = Path(folder)
    if not folder.is_dir():
        return False, "Folder not found."
    for sub, prefix, ext in game.archives:
        d = _child(folder, sub)
        try:
            names = [p.name.lower() for p in d.iterdir()] if d and d.is_dir() else []
        except OSError:
            names = []
        if not any(n.startswith(prefix) and n.endswith(ext) for n in names):
            return False, f"Not a {game.name} folder (no {sub}/{prefix}*{ext})."
    kind = "Steam" if waw.find_steamapps(folder) else "folder"
    return True, f"{game.name} found ({kind})."


def find_steam(game: PcGame) -> List[Path]:
    """The game's installs in the Steam libraries on this PC."""
    found: List[Path] = []
    for library in waw.steam_libraries():
        folder = library / "steamapps" / "common" / game.steam_folder
        if folder not in found and recognize(folder, game)[0]:
            found.append(folder)
    return found


@dataclass
class FoundInstall:
    """One install in `newcopy find`'s report."""
    game: str           # cod, coduo or cod2
    name: str
    version: str = ""
    language: str = ""
    source: str = ""
    folder: str = ""
    archives: str = ""
    missing: str = ""
    notes: List[str] = field(default_factory=list)


_FIND_HEAD = re.compile(r"^(\S.*) \((cod|coduo|cod2)\)$")
_FIND_FIELD = re.compile(r"^  (\w+)\s+(.*)$")


def parse_find(text: str) -> List[FoundInstall]:
    """The installs in `newcopy find`'s output: a line "<name> (<key>)"
    each, then indented "key   value" lines."""
    found: List[FoundInstall] = []
    current: Optional[FoundInstall] = None
    for line in text.splitlines():
        m = _FIND_HEAD.match(line.rstrip())
        if m:
            current = FoundInstall(m.group(2), m.group(1))
            found.append(current)
            continue
        if not line.strip():
            continue
        if not line.startswith(" "):
            current = None   # "Steam:", "Proton builds ..." and other blocks
            continue
        f = _FIND_FIELD.match(line)
        if current is None or not f:
            continue
        key, value = f.group(1), f.group(2).strip()
        if key == "version":
            current.version = value.split(" (")[0]
        elif key in ("language", "source", "folder", "archives", "missing"):
            setattr(current, key, value)
        elif key in ("note", "hint"):
            current.notes.append(value)
    return found


def _same_folder(a: str, b: str) -> bool:
    """Whether two folder names mean the same folder (case and separators as
    the system has them; Windows' \\\\?\\ prefix for long paths ignored)."""
    def norm(p: str) -> str:
        p = p.strip()
        if p.startswith("\\\\?\\"):
            p = p[4:]
        return os.path.normcase(os.path.normpath(p))
    if norm(a) == norm(b):
        return True
    try:
        return Path(a.strip()).resolve() == Path(b.strip()).resolve()
    except OSError:
        return False


def pick_found(found: Sequence[FoundInstall], game_id: str, install: str) -> Optional[FoundInstall]:
    """The entry in `newcopy find`'s report for the install chosen on the
    page. newcopy lists Steam's installs first and the --from folder after,
    so with a folder chosen it is the entry for that folder; with none, the
    first (Steam's)."""
    mine = [f for f in found if f.game == game_id]
    if not install.strip():
        return mine[0] if mine else None
    exact = [f for f in mine if f.folder and _same_folder(f.folder, install)]
    if exact:
        return exact[0]
    # A folder newcopy spelled differently: the last one not from Steam.
    own = [f for f in mine if not f.source.lower().startswith("steam")]
    return own[-1] if own else None


def find_summary(f: FoundInstall) -> str:
    """One short line about an install for the Done note: "Call of Duty 2
    1.0 (your folder): 9 found, 0.9 GB, 7 missing: iw_09.iwd, ...". The
    full report (with the folder) is in the log."""
    where = "Steam" if f.source.lower().startswith("steam") else "your folder"
    text = f"{f.name} {f.version or '(version unknown)'} ({where}): {f.archives or 'archives listed in the log'}"
    return text + (f": {f.missing}." if f.missing else ".")


def copy_progress(log: Log) -> Callable[[str], None]:
    """newcopy make's "[ 3/40] linked main/iw_00.iwd" lines as progress."""
    pattern = re.compile(r"^\s*\[\s*(\d+)/(\d+)\]\s+(.*)$")

    def handle(line: str) -> None:
        m = pattern.match(line)
        if m and int(m.group(2)) > 0:
            log.progress(int(m.group(1)) / int(m.group(2)), m.group(3).strip()[:80])
    return handle


def local_address() -> str:
    """This PC's address on the local network, for "connect <address>:<port>".
    A UDP socket is pointed at a documentation address to learn which
    interface would be used; no packet is sent. "" when there's none."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))   # TEST-NET-1 (RFC 5737): never routed anywhere real
            address = s.getsockname()[0]
            return "" if address.startswith("0.") else address
    except OSError:
        return ""


# ---------------------------------------------------------------------------
# The Rust programs: building and finding them
# ---------------------------------------------------------------------------
def engine_has(tool: str, tool_root: Path) -> bool:
    """Whether the project's engine/ folder has the tool's source."""
    package_dir = TOOLS[tool][1][0]
    return (rippers.engine_dir(tool_root) / package_dir / "Cargo.toml").is_file()


def installed(tool: str, output: Optional[Path]) -> Optional[Path]:
    """The copy made earlier, in <output>/tools."""
    if output is not None:
        exe = Path(output) / "tools" / exe_name(tool)
        if exe.is_file():
            return exe
    return None


def prebuilt(tool: str, tool_root: Path) -> Optional[Path]:
    exe = rippers.engine_dir(tool_root) / "bin" / host_triple() / exe_name(tool)
    return exe if exe.is_file() else None


def cargo_home(env: Optional[dict] = None) -> Path:
    """Where cargo keeps what it downloads: CARGO_HOME in the build's
    environment (the project's own Rust sets it), else ~/.cargo, wherever
    cargo itself is installed (/usr/bin/cargo too)."""
    e = env if env is not None else os.environ
    return Path(e.get("CARGO_HOME") or Path.home() / ".cargo")


def build_install(env: Optional[dict] = None) -> Install:
    """The approval entry for cargo's list of Rust libraries (crates.io), for
    a first build: these two programs use none, but cargo reads the list for
    the whole engine. Nothing else is downloaded."""
    return Install("crates", "Rust library list (crates.io) for building newcopy and iw-server; they use no "
                   "outside libraries", str(cargo_home(env) / "registry"), "under 1 MB", "crates.io")


def _cargo_has_index(env: Optional[dict] = None) -> bool:
    return (cargo_home(env) / "registry" / "index").is_dir()


def build_tool(tool: str, tool_root: Path, target_dir: Path, cargo: Path, jobs: int, runner: Runner, log: Log,
               env: Optional[dict] = None) -> Path:
    """`cargo build --release --locked -p <package>` in engine/; returns the
    binary. Build files go to `target_dir` (the work folder)."""
    package = TOOLS[tool][0]
    engine = rippers.engine_dir(tool_root)
    if not engine_has(tool, tool_root):
        raise SetupError(f"{tool}'s source isn't in {engine}. Update the project folder to a version that has "
                         f"engine/{TOOLS[tool][1][0]}.")
    build_env = dict(env if env is not None else os.environ)
    if not _cargo_has_index(build_env):
        APPROVAL.require([build_install(build_env)], log)
    log.info(f"Building {tool} (Rust; a minute or two the first time)...")
    build_env["CARGO_TARGET_DIR"] = str(target_dir)
    build_env["PATH"] = str(Path(cargo).parent) + os.pathsep + build_env.get("PATH", "")
    runner.run([cargo, "build", "--release", "--locked", "--jobs", str(max(1, jobs)), "-p", package],
               cwd=engine, env=build_env)
    exe = target_dir / "release" / exe_name(tool)
    if not exe.is_file():
        raise SetupError(f"The {tool} build finished but made no {exe.name}; see the log.")
    return exe


OLD_COPY = ".old"   # a running copy moved aside on Windows: <name>.old, <name>.old2 ...


def install_copy(exe: Path, output: Path) -> Path:
    """Copies a built tool into <output>/tools, so it works after the work
    folder is cleaned up. The copy there may be running (a LAN server), so
    it is never written over:

    - when it is already the same file, it is left as it is;
    - else the new one is written next to it and swapped in by a rename.
      Linux lets a running program's file be replaced that way (the running
      one keeps the old file). Windows doesn't, but lets it be renamed: it
      is moved aside to <name>.old and removed on a later run.

    Raises OSError when it can't be done; the copy there is unchanged then."""
    dest = Path(output) / "tools" / exe.name
    dest.parent.mkdir(parents=True, exist_ok=True)
    _remove_old_copies(dest)
    if dest.is_file() and (exe.resolve() == dest.resolve() or filecmp.cmp(exe, dest, shallow=False)):
        return dest
    new = dest.with_name(dest.name + ".new")
    try:
        shutil.copy2(exe, new)
        if not IS_WINDOWS:
            new.chmod(new.stat().st_mode | 0o755)
        try:
            os.replace(new, dest)
        except PermissionError:
            if not dest.exists():
                raise
            os.replace(dest, _free_old_name(dest))   # the running copy, moved aside
            os.replace(new, dest)
    finally:
        if new.exists():
            try:
                new.unlink()
            except OSError:
                pass
    return dest


def _old_copies(dest: Path) -> List[Path]:
    prefix = dest.name + OLD_COPY
    try:
        return [p for p in dest.parent.iterdir() if p.name.startswith(prefix)]
    except OSError:
        return []


def _free_old_name(dest: Path) -> Path:
    taken = {p.name for p in _old_copies(dest)}
    n = 1
    while (dest.name + OLD_COPY + (str(n) if n > 1 else "")) in taken:
        n += 1
    return dest.with_name(dest.name + OLD_COPY + (str(n) if n > 1 else ""))


def _remove_old_copies(dest: Path) -> None:
    """Copies moved aside earlier, once they no longer run."""
    for p in _old_copies(dest):
        try:
            p.unlink()
        except OSError:
            pass   # still running: next time


def ensure_tool(tool: str, tool_root: Path, output: Path, work: Path, jobs: int, runner: Runner, log: Log,
                cancel: threading.Event, allow_install: bool) -> Path:
    """The tool, ready to run: built (or brought up to date) from engine/
    when its source is there, else the copy made earlier or a prebuilt one.
    Rust is installed first when it's missing, after the user approves."""
    tools_dir = work / "tools"
    have_source = engine_has(tool, tool_root)
    ready = installed(tool, output) or prebuilt(tool, tool_root)
    if have_source:
        tc = deps.detect(tools_dir, runner)
        if tc.rust_problems() and allow_install:
            tc = deps.ensure(tools_dir, runner, log, cancel, True, recomp=False, archives=False)
        if not tc.rust_problems():
            try:
                exe = build_tool(tool, tool_root, work / "rust-target", tc.cargo, jobs, runner, log, env=tc.env)
                return install_copy(exe, output)
            except (SetupError, OSError) as e:
                if isinstance(e, SetupError) and "Not approved" in str(e):
                    raise
                why = (e.args[0].splitlines()[0] if isinstance(e, SetupError) and e.args else str(e)) or "?"
                if ready is None:
                    if isinstance(e, SetupError):
                        raise
                    raise SetupError(f"{tool} was built, but couldn't be put in {Path(output) / 'tools'}: {why}")
                log.warn(f"{tool} wasn't updated ({why}); using {ready}.")
                return ready
        if ready is None:
            raise SetupError(f"Building {tool} needs Rust: " + "; ".join(tc.rust_problems()) +
                             ". Turn on installing what's missing (Set up), or install Rust from https://rustup.rs")
    if ready is None:
        raise SetupError(f"{tool} isn't built yet, and its source isn't in the engine folder. Update the project "
                         "folder to a version with engine/tools.")
    return ready


# ---------------------------------------------------------------------------
# The LAN server as a background process
# ---------------------------------------------------------------------------
class ServerProcess:
    """iw-server running in the background: its output goes line by line to
    `on_line`, `on_exit` gets the exit code. stop() ends it (and anything it
    started)."""

    def __init__(self, on_line: Callable[[str], None], on_exit: Callable[[int], None]):
        self.on_line = on_line
        self.on_exit = on_exit
        self.proc: Optional[subprocess.Popen] = None
        self.stopping = False

    @property
    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def start(self, cmd: Sequence[str], env: Optional[dict] = None, cwd: Optional[Path] = None) -> None:
        if self.running:
            raise SetupError("The server is already running.")
        kwargs: dict = dict(stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            env=env, cwd=str(cwd) if cwd else None)
        if IS_WINDOWS:
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | 0x08000000  # no console window
        else:
            kwargs["start_new_session"] = True
        try:
            self.proc = subprocess.Popen([str(c) for c in cmd], **kwargs)
        except OSError as e:
            raise SetupError(f"The server didn't start: {e}")
        self.stopping = False
        threading.Thread(target=self._read, args=(self.proc,), daemon=True).start()

    def _read(self, proc: subprocess.Popen) -> None:
        assert proc.stdout is not None
        for raw in iter(proc.stdout.readline, b""):
            line = raw.decode("utf-8", errors="replace").rstrip()
            if line:
                self.on_line(line)
        proc.stdout.close()
        code = proc.wait()
        self.on_exit(code)

    def stop(self, wait: bool = True) -> None:
        """Ends the server. With `wait` (call it off the UI thread) it is
        made sure of, killing it after a few seconds if it hangs; without,
        it is only asked to end (when the tool quits: at once)."""
        proc = self.proc
        if proc is None or proc.poll() is not None:
            return
        self.stopping = True
        if wait:
            kill_tree(proc)
        else:
            try:
                proc.terminate()
            except OSError:
                pass

"""The setup tool's own updater: newer releases of this project folder, signed
by the project's release key (docs/UPDATES.md).

What makes it safe to use:
- Every release carries `COD_3_WAW_Setup-tool/release.json`: the version, the
  release notes and each file's path, size and SHA-256. Its Ed25519 signature
  (`release.json.sig`) is made on the owner's own PC with a key that never
  goes to GitHub (tools/sign_release.py). The public keys are in
  `c3w/update_keys.json`, which is itself one of the signed files.
- GitHub only carries the files. Nothing it serves is used unless the
  signature checks out with a key this copy already trusts, and every file
  matches its SHA-256 and size before it's written anywhere.
- The signature is checked before the manifest is even read. Only versions
  newer than this copy's own release (and not older than the newest one seen)
  are offered, so an old release can't be played back.
- Changing the keys (or where releases come from) takes the offline recovery
  key. A release signed with the signing key can still change any code
  (including this file), as with any software update: the recovery key lets
  the owner move players who haven't installed a bad release to a new key.
- Paths are checked: relative, no "..", no links on the way, never the
  player's own folders (input, output, work, config, dev).
- Nothing new runs before it's checked: this copy's code checks and downloads
  the update into config/update/pending/, and it's moved in place at the next
  start (cod3_waw_setup.py calls `at_start` before it loads any other part of
  the tool; this file needs only the standard library and ed25519.py). A
  journal (config/update/applying.json) is written before the first file is
  replaced: an update cut off halfway (power loss, a closed window) is rolled
  back at the next start. The replaced files are kept in
  config/update/previous/ (Undo).
- Nothing is fetched without the player's choice: update checks are off until
  the player allows them (asked once), and an update is only downloaded when
  the player says so.

The checks, the plan and the apply step are plain functions with the network
passed in (`fetch`), so tests run them on a local folder.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import time
import http.client
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

from . import ed25519

FORMAT = 1
PRODUCT = "cod3-setup-tool"
# Prepended to the manifest before signing: a signature made for this means
# nothing anywhere else.
SIGNED_PREFIX = b"cod3-setup-tool release manifest v1\n"

TOOL_DIR = "COD_3_WAW_Setup-tool"
MANIFEST_PATH = f"{TOOL_DIR}/release.json"
SIGNATURE_PATH = f"{TOOL_DIR}/release.json.sig"
KEYS_PATH = f"{TOOL_DIR}/c3w/update_keys.json"
# The player's own folders in the project folder: never in a release.
USER_DIRS = frozenset({"input", "output", "work", "config", "dev"})

HOST = "raw.githubusercontent.com"
MAX_MANIFEST = 8 << 20
MAX_SIGNATURE = 4096
MAX_FILE = 256 << 20
MAX_FILES = 50000
USER_AGENT = "cod3-setup-tool-updater"

Fetch = Callable[[str, int], bytes]
Progress = Callable[[float, str], None]

_VERSION = re.compile(r"^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$")
_REF = re.compile(r"^(v(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})|[0-9a-f]{40})$")
_REPO = re.compile(r"^[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$")
_PREFIX = re.compile(r"^([A-Za-z0-9_.-]{1,100}/){0,4}$")
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_WINDOWS_NAMES = frozenset({"con", "prn", "aux", "nul", "conin$", "conout$", "clock$"} |
                           {f"com{i}" for i in range(0, 10)} | {f"lpt{i}" for i in range(0, 10)})
_BAD_CHARS = set('<>:"|?*\\')
_SHORT_NAME = re.compile(r"~[0-9]")   # Windows 8.3 names (COD_3_~1) name the same file another way
EOLS = ("", "crlf")                     # how a file's git copy becomes the file (see _to_crlf)
# The updater's own files are replaced last (an update cut off halfway is
# rolled back by the copy of this file that is in place).
_LAST = (f"{TOOL_DIR}/c3w/__init__.py", f"{TOOL_DIR}/c3w/ed25519.py", f"{TOOL_DIR}/c3w/update.py",
         f"{TOOL_DIR}/cod3_waw_setup.py")
JOURNAL_FORMAT = 1   # config/update/applying.json and undo.json: later versions must keep reading it


class UpdateError(Exception):
    """An update that can't or mustn't be used; the message says why."""


# ---------------------------------------------------------------------------
# Where things are
# ---------------------------------------------------------------------------
def release_root() -> Path:
    """The project folder this copy of the tool belongs to (the one with
    setup.sh): this file is <root>/COD_3_WAW_Setup-tool/c3w/update.py."""
    return Path(__file__).resolve().parent.parent.parent


def _config_dir() -> Path:
    # common.config_dir() is <project>/config too; not imported here, so this
    # file runs before the rest of the tool is loaded.
    return release_root() / "config"


def update_dir() -> Path:
    """config/update/ (pending/, previous/, applying.json)."""
    return _config_dir() / "update"


def _state_path() -> Path:
    return _config_dir() / "updates.json"


def _human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def read_state() -> dict:
    try:
        data = json.loads(_state_path().read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def write_state(**changes) -> None:
    state = {**read_state(), **changes}
    path = _state_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(state, indent=2), encoding="utf-8")
    tmp.replace(path)


def checks_allowed() -> Optional[bool]:
    """Whether the player allows update checks: True, False, or None when
    not asked yet (then nothing is fetched)."""
    v = read_state().get("checks_allowed")
    return v if isinstance(v, bool) else None


def set_checks_allowed(allowed: bool) -> None:
    write_state(checks_allowed=bool(allowed))


# ---------------------------------------------------------------------------
# Versions, paths, keys
# ---------------------------------------------------------------------------
def parse_version(text: str) -> Tuple[int, int, int]:
    m = _VERSION.match(text if isinstance(text, str) else "")
    if not m:
        raise UpdateError(f"not a version: {text!r}")
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def safe_path(path: str) -> bool:
    """A release file's path: relative with '/', no '.', '..' or empty parts,
    nothing Windows can't name, never the player's folders, .git, Python's
    caches or the manifest itself."""
    if not isinstance(path, str) or not path or len(path) > 400 or path.startswith("/"):
        return False
    if any(not 32 <= ord(c) < 127 for c in path):
        return False   # plain ASCII names only (no look-alike or superscript characters)
    parts = path.split("/")
    folded = [p.casefold() for p in parts]
    for part in parts:
        if part in ("", ".", "..") or part.endswith((" ", ".")) or part.startswith(" ") or len(part) > 200:
            return False
        if any(c in _BAD_CHARS for c in part) or _SHORT_NAME.search(part):
            return False
        if part.split(".", 1)[0].rstrip(" ").casefold() in _WINDOWS_NAMES:
            return False
    # Letter case doesn't matter on Windows and macOS: Config/ is config/.
    if folded[0] in USER_DIRS or ".git" in folded or "__pycache__" in folded or folded[-1].endswith(".pyc"):
        return False
    whole = path.casefold()
    if whole in (MANIFEST_PATH.casefold(), SIGNATURE_PATH.casefold()):
        return False
    return not (whole == KEYS_PATH.casefold() and path != KEYS_PATH)


def _is_link(p: Path) -> bool:
    """A symlink, or on Windows any reparse point (junctions too)."""
    try:
        st = os.lstat(p)
    except OSError:
        return False
    if stat.S_ISLNK(st.st_mode):
        return True
    return bool(getattr(st, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def _to_crlf(data: bytes) -> bytes:
    """Git's checkout of a text file with eol=crlf: each LF becomes CRLF."""
    return re.sub(rb"(?<!\r)\n", b"\r\n", data)


@dataclass
class Keys:
    """c3w/update_keys.json: where releases come from and who may sign them."""
    repo: str = ""            # "owner/name" on GitHub
    prefix: str = ""          # the release folder inside the repository ("RELEASE/")
    signing: List[bytes] = field(default_factory=list)
    recovery: List[bytes] = field(default_factory=list)
    file_sha256: str = ""

    @property
    def configured(self) -> bool:
        return bool(self.repo and (self.signing or self.recovery))


def _hex_keys(value) -> List[bytes]:
    if not isinstance(value, list):
        raise UpdateError("update_keys.json: a key list isn't a list")
    out = []
    for k in value:
        if not isinstance(k, str) or not _HEX64.match(k):
            raise UpdateError("update_keys.json: a key isn't 64 hex digits")
        out.append(bytes.fromhex(k))
    return out


def parse_keys(data: bytes) -> Keys:
    try:
        d = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        raise UpdateError("update_keys.json isn't valid JSON")
    if not isinstance(d, dict) or d.get("format") != FORMAT:
        raise UpdateError("update_keys.json has an unknown format")
    source = d.get("source") or {}
    repo, prefix = source.get("repo", ""), source.get("prefix", "")
    if repo and not _REPO.match(repo):
        raise UpdateError("update_keys.json: the repository isn't owner/name")
    if not isinstance(prefix, str) or not _PREFIX.match(prefix):
        raise UpdateError("update_keys.json: the release folder isn't a plain relative folder")
    return Keys(repo=repo, prefix=prefix, signing=_hex_keys(d.get("signing", [])),
                recovery=_hex_keys(d.get("recovery", [])), file_sha256=hashlib.sha256(data).hexdigest())


def load_keys(root: Path) -> Keys:
    try:
        return parse_keys((root / KEYS_PATH).read_bytes())
    except OSError:
        return Keys()


# ---------------------------------------------------------------------------
# Releases
# ---------------------------------------------------------------------------
@dataclass
class Release:
    """A checked release.json."""
    version: Tuple[int, int, int]
    version_text: str
    ref: str                  # the git tag (or commit) its files are fetched from
    date: str
    notes: str
    files: Dict[str, Tuple[str, int]]   # path -> (sha256, size) of the file as it is in a player's folder
    manifest: bytes
    signature: bytes
    key: bytes
    by_recovery: bool
    eol: Dict[str, str] = field(default_factory=dict)   # path -> "crlf": git's copy has LF, the file CRLF
    executable: frozenset = frozenset()                   # paths with the executable bit (Linux, macOS)


def signature_file(key: bytes, signature: bytes) -> bytes:
    return (json.dumps({"format": FORMAT, "key": key.hex(), "signature": signature.hex()}, indent=2) +
            "\n").encode("ascii")


def verify_release(manifest: bytes, signature: bytes, keys: Keys) -> Release:
    """Checks the signature first, then reads the manifest. Raises
    UpdateError with the reason when anything is wrong."""
    if len(manifest) > MAX_MANIFEST or len(signature) > MAX_SIGNATURE:
        raise UpdateError("the release information is too big")
    try:
        sig = json.loads(signature.decode("ascii"))
        k = sig.get("key", "")
        key = bytes.fromhex(k) if isinstance(k, str) and _HEX64.match(k) else b""
        raw = sig.get("signature", "")
        sig_bytes = bytes.fromhex(raw) if isinstance(raw, str) and re.fullmatch(r"[0-9a-f]{128}", raw) else b""
    except (UnicodeDecodeError, ValueError, AttributeError, TypeError, RecursionError):
        raise UpdateError("the release signature file is unreadable")
    if not isinstance(sig, dict) or sig.get("format") != FORMAT or not key or not sig_bytes:
        raise UpdateError("the release signature file is unreadable")
    if key not in keys.signing and key not in keys.recovery:
        raise UpdateError("the release is signed by a key this copy doesn't trust")
    if not ed25519.verify(key, SIGNED_PREFIX + manifest, sig_bytes):
        raise UpdateError("the release's signature doesn't match its contents")
    # Signed by a trusted key: now it may be read.
    try:
        d = json.loads(manifest.decode("utf-8"))
    except (UnicodeDecodeError, ValueError, RecursionError):
        raise UpdateError("the signed release information isn't valid JSON")
    if not isinstance(d, dict) or d.get("format") != FORMAT or d.get("product") != PRODUCT:
        raise UpdateError("the signed release information is for something else")
    version_text = d.get("version", "")
    version = parse_version(version_text)
    ref = d.get("ref", "")
    if not isinstance(ref, str) or not _REF.match(ref):
        raise UpdateError("the signed release names no valid tag")
    files = d.get("files")
    if not isinstance(files, dict) or not files or len(files) > MAX_FILES:
        raise UpdateError("the signed release lists no files")
    checked: Dict[str, Tuple[str, int]] = {}
    eols: Dict[str, str] = {}
    executable = set()
    folded = set()
    for path, entry in files.items():
        if not safe_path(path):
            raise UpdateError(f"the signed release lists a path that isn't allowed: {path!r}")
        if path.casefold() in folded:
            raise UpdateError(f"the signed release lists {path!r} twice (letter case)")
        folded.add(path.casefold())
        if not isinstance(entry, dict):
            raise UpdateError(f"bad entry for {path}")
        sha, size = entry.get("sha256", ""), entry.get("size")
        if not isinstance(sha, str) or not _HEX64.match(sha) or not isinstance(size, int) or \
                isinstance(size, bool) or not 0 <= size <= MAX_FILE:
            raise UpdateError(f"bad entry for {path}")
        eol, x = entry.get("eol", ""), entry.get("exec", False)
        if eol not in EOLS or not isinstance(x, bool):
            raise UpdateError(f"bad entry for {path}")
        checked[path] = (sha, size)
        if eol:
            eols[path] = eol
        if x:
            executable.add(path)
    notes, date = d.get("notes", ""), d.get("date", "")
    if isinstance(notes, str):   # shown in the window and the terminal: no control characters
        notes = "".join(c for c in notes if c == "\n" or (ord(c) >= 32 and c != "\x7f"))
    return Release(version=version, version_text=version_text, ref=ref,
                   date=date if isinstance(date, str) else "", notes=notes[:8000] if isinstance(notes, str) else "",
                   files=checked, manifest=manifest, signature=signature, key=key,
                   by_recovery=key in keys.recovery, eol=eols, executable=frozenset(executable))


def installed_release(root: Path, keys: Keys) -> Optional[Release]:
    """This copy's own signed release.json, or None (not a signed release, or
    it doesn't check out)."""
    try:
        manifest = (root / MANIFEST_PATH).read_bytes()
        signature = (root / SIGNATURE_PATH).read_bytes()
    except OSError:
        return None
    try:
        return verify_release(manifest, signature, keys)
    except UpdateError:
        return None


def git_checkout(root: Path) -> Optional[Path]:
    """The git checkout `root` is in (the folder with .git), if it is in one."""
    for folder in (root, *root.parents):
        if (folder / ".git").exists():
            return folder
    return None


def why_not_here(root: Optional[Path] = None) -> str:
    """'' when this copy can update itself, else why not (in plain words)."""
    here = Path(__file__).resolve().parent.parent
    root = root or release_root()
    if here.name != TOOL_DIR or not (root / "setup.sh").is_file():
        return "This copy of the setup tool is the launcher in a game folder; the setup tool's own folder updates it."
    if git_checkout(root):
        # A git clone (the owner's, or a player's) is updated with git; files
        # put in place here would fight with it.
        return "This copy is a git checkout, so it isn't updated here: use  git pull  in it."
    keys = load_keys(root)
    if not keys.configured:
        return "This copy has no release key yet, so it can't check for updates."
    if installed_release(root, keys) is None:
        return "This copy isn't a signed release (a folder made by hand or from the source), so it isn't updated."
    return ""


# ---------------------------------------------------------------------------
# The network
# ---------------------------------------------------------------------------
class _HttpsOnlyRedirects(urllib.request.HTTPRedirectHandler):
    """Redirects only to https on the same host."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        u = urllib.parse.urlsplit(str(newurl))
        if u.scheme.lower() != "https" or (u.hostname or "").lower() != HOST:
            raise UpdateError(f"the download was redirected away from https://{HOST}/; refused")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def fetch(url: str, limit: int) -> bytes:
    """GET over https from HOST (the certificate is checked), at most `limit`
    bytes, and not slower than about 20 KB/s overall."""
    u = urllib.parse.urlsplit(url)
    if u.scheme != "https" or u.hostname != HOST:
        raise UpdateError(f"only https://{HOST}/ is used")
    opener = urllib.request.build_opener(_HttpsOnlyRedirects())
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    deadline = time.monotonic() + max(120.0, limit / 20000)
    chunks: List[bytes] = []
    total = 0
    try:
        with opener.open(req, timeout=30) as r:
            while total <= limit:
                chunk = r.read(min(1 << 16, limit + 1 - total))
                if not chunk:
                    break
                chunks.append(chunk)
                total += len(chunk)
                if time.monotonic() > deadline:
                    raise UpdateError("the download was too slow; stopped")
    except UpdateError:
        raise
    except (OSError, ValueError, http.client.HTTPException) as e:
        raise UpdateError(f"couldn't download from {HOST} ({e})")
    if total > limit:
        raise UpdateError("a download was bigger than the release says; refused")
    return b"".join(chunks)


def _url(keys: Keys, ref: str, path: str) -> str:
    return f"https://{HOST}/{keys.repo}/{ref}/" + urllib.parse.quote(keys.prefix + path, safe="/")


def fetch_latest(keys: Keys, get: Fetch = fetch) -> Release:
    """The newest release.json on GitHub, checked."""
    manifest = get(_url(keys, "HEAD", MANIFEST_PATH), MAX_MANIFEST)
    signature = get(_url(keys, "HEAD", SIGNATURE_PATH), MAX_SIGNATURE)
    return verify_release(manifest, signature, keys)


# ---------------------------------------------------------------------------
# What an update changes
# ---------------------------------------------------------------------------
def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _local(root: Path, path: str) -> Optional[str]:
    p = root / path
    try:
        if _is_link(p) or not p.is_file():
            return None
        return _sha256(p)
    except OSError:
        return None


def _same_file(a: Path, b: Path) -> bool:
    """True when two paths are one file (on Windows and macOS, names that
    differ only in letter case)."""
    try:
        return os.path.samefile(a, b)
    except OSError:
        return False


@dataclass
class Plan:
    """What updating from `installed` to `release` does to this folder."""
    installed: Release
    release: Release
    fetch: List[str]            # new or changed files to download and write
    edited: List[str]           # of those, files this copy had changed itself (the player's edits)
    remove: List[str]           # files the new release drops (only unchanged ones)
    download_bytes: int

    @property
    def summary(self) -> str:
        text = f"{len(self.fetch)} file{'s' if len(self.fetch) != 1 else ''} to download ({_human(self.download_bytes)})"
        if self.remove:
            text += f", {len(self.remove)} to remove"
        return text


def _seen(state: dict, key: str) -> Optional[Tuple[int, int, int]]:
    try:
        return parse_version(state.get(key, ""))
    except UpdateError:
        return None


def check_rules(installed: Release, release: Release, keys: Keys, root: Path,
                highest_seen: Optional[Tuple[int, int, int]] = None,
                highest_recovery: Optional[Tuple[int, int, int]] = None) -> None:
    """Raises UpdateError when `release` mustn't replace `installed`.

    Never older than this copy. A release signed with the signing key: not
    older than the newest release seen (an old one can't be played back). A
    release signed with the recovery key: not older than the newest
    recovery-signed one seen; it may be older than a signing-key release that
    was seen, so the owner can recover from a stolen signing key that put out
    a made-up high version."""
    if release.version <= installed.version:
        raise UpdateError(f"{release.version_text} isn't newer than this copy's {installed.version_text}")
    limit = highest_recovery if release.by_recovery else highest_seen
    if limit and release.version < limit:
        raise UpdateError(f"{release.version_text} is older than a release already seen "
                          f"({'.'.join(map(str, limit))}); refused")
    new_keys = release.files.get(KEYS_PATH)
    if new_keys is None:
        raise UpdateError("the release has no update_keys.json")
    # Against this copy's signed release (the file as it was given out), else the file here.
    local_keys = (installed.files.get(KEYS_PATH) or (None,))[0] or keys.file_sha256 or _local(root, KEYS_PATH)
    if new_keys[0] != local_keys and not release.by_recovery:
        raise UpdateError("the release changes the release keys without the recovery key's signature; refused")


def _renamed_in_case(root: Path, old: str, release: Release) -> bool:
    """`old` is gone from the release only as a change of letter case, and
    this folder's file system treats both names as one file."""
    new = {p.casefold(): p for p in release.files}.get(old.casefold())
    return new is not None and new != old and _same_file(root / old, root / new)


class _Names:
    """The names on disk, letter case and all (cached per folder)."""

    def __init__(self, root: Path):
        self.root = root
        self._dirs: Dict[str, Optional[List[str]]] = {}

    def _list(self, rel: str) -> Optional[List[str]]:
        if rel not in self._dirs:
            try:
                self._dirs[rel] = os.listdir(self.root / rel) if rel else os.listdir(self.root)
            except OSError:
                self._dirs[rel] = None
        return self._dirs[rel]

    def on_disk(self, path: str) -> Optional[str]:
        """`path` as it is named on disk: itself when every part has that
        exact name, the name in another letter case where only that exists
        (Windows, macOS), None when there's no such file."""
        if not os.path.lexists(self.root / path):
            return None   # (on Linux another letter case is another file)
        parts, rel = [], ""
        for part in path.split("/"):
            names = self._list(rel)
            if names is None:
                return None
            if part not in names:
                match = [n for n in names if n.isascii() and n.lower() == part.lower()]
                if not match:
                    return None
                part = match[0]
            parts.append(part)
            rel = "/".join(parts)
        return rel


def disk_name(root: Path, path: str) -> Optional[str]:
    return _Names(root).on_disk(path)


def make_plan(root: Path, installed: Release, release: Release) -> Plan:
    fetch_list, edited, remove = [], [], []
    size = 0
    installed_folded = {p.casefold(): p for p in installed.files}
    names = _Names(root)
    for path, (sha, n) in sorted(release.files.items()):
        have = _local(root, path)
        if have == sha and names.on_disk(path) == path:
            continue   # (a name only in another letter case is written again, with the release's)
        fetch_list.append(path)
        size += n
        before = installed.files.get(path) or installed.files.get(installed_folded.get(path.casefold(), ""))
        if have is not None and (before or (None,))[0] != have:
            edited.append(path)
    for path, (sha, _) in sorted(installed.files.items()):
        if path not in release.files and _local(root, path) == sha and not _renamed_in_case(root, path, release):
            remove.append(path)
    # The updater's own files last (the copy in place rolls back a cut-off update).
    fetch_list.sort(key=lambda p: (_LAST.index(p) + 1) if p in _LAST else 0)
    return Plan(installed, release, fetch_list, edited, remove, size)


def check(root: Optional[Path] = None, get: Fetch = fetch) -> Tuple[Optional[Plan], str]:
    """Looks for a newer release. Returns (plan, '') when there is one,
    (None, message) when there isn't or it can't be used. Raises UpdateError
    only for an update that was refused (a bad signature, a replay)."""
    root = root or release_root()
    reason = why_not_here(root)
    if reason:
        return None, reason
    keys = load_keys(root)
    installed = installed_release(root, keys)
    assert installed is not None
    release = fetch_latest(keys, get)
    state = read_state()
    highest, highest_recovery = _seen(state, "highest_seen"), _seen(state, "highest_recovery_seen")
    write_state(last_check=int(time.time()))
    if release.version <= installed.version:
        return None, f"The setup tool is up to date (release {installed.version_text})."
    check_rules(installed, release, keys, root, highest, highest_recovery)
    if release.by_recovery:
        if not highest_recovery or release.version > highest_recovery:
            write_state(highest_recovery_seen=release.version_text)
    elif not highest or release.version > highest:
        write_state(highest_seen=release.version_text)
    return make_plan(root, installed, release), ""


# ---------------------------------------------------------------------------
# Downloading (into config/update/pending/) and applying (at the next start)
# ---------------------------------------------------------------------------
def _guarded_rmtree(path: Path) -> None:
    """Removes a folder inside config/update/ only."""
    base = update_dir().resolve()
    p = path.resolve()
    if p == base or base not in p.parents:
        raise UpdateError(f"refused to delete {path}")
    shutil.rmtree(p, ignore_errors=True)


class _Lock:
    """One copy of the setup tool at a time downloads, puts in place or undoes
    an update (config/update/lock)."""

    def __init__(self, wait: float):
        self.wait = wait
        self.f = None

    def _try(self) -> bool:
        try:
            if os.name == "nt":
                import msvcrt
                self.f.seek(0)
                msvcrt.locking(self.f.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            return False

    def __enter__(self) -> "_Lock":
        path = update_dir() / "lock"
        path.parent.mkdir(parents=True, exist_ok=True)
        self.f = open(path, "a+b")
        deadline = time.monotonic() + self.wait
        while not self._try():
            if time.monotonic() >= deadline:
                self.f.close()
                raise UpdateError("another copy of the setup tool is updating it right now")
            time.sleep(0.2)
        return self

    def __exit__(self, *exc) -> None:
        try:
            if os.name == "nt":
                import msvcrt
                self.f.seek(0)
                msvcrt.locking(self.f.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.f.fileno(), fcntl.LOCK_UN)
        except OSError:
            pass
        finally:
            self.f.close()


def _fsync_dir(folder: Path) -> None:
    """Makes a rename in `folder` stick on Linux and macOS (Windows does it
    with the file)."""
    if os.name == "nt":
        return
    try:
        fd = os.open(folder, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def _replace(src: Path, dst: Path) -> None:
    """os.replace, a few times if Windows still holds the file (the old
    setup tool may be closing)."""
    for attempt in range(25):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if attempt == 24:
                raise
            time.sleep(0.2)


def _write_durable(target: Path, data: bytes, mode: int = 0o644) -> None:
    """Writes a new temporary file next to `target` (never through a link),
    puts it on the disk, then moves it in place: `target` is either the old
    file or the whole new one, even after a power cut."""
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_name(target.name + ".c3w-update")
    if _is_link(tmp) or tmp.is_file():
        tmp.unlink()
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), mode)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        _replace(tmp, target)
    except BaseException:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise
    _fsync_dir(target.parent)


def _write_file(target: Path, data: bytes) -> None:
    """A file in config/update/pending/ (checked again before it's used)."""
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)


def _mode(p: Path) -> int:
    return stat.S_IMODE(os.stat(p).st_mode)


def _fix_case(root: Path, path: str) -> None:
    """On Windows and macOS a replaced file (or a folder on its way) keeps the
    old name's letter case; give each the release's (Python's imports go by
    it). One rename each: a change of letter case only."""
    folder = root
    for part in path.split("/"):
        try:
            names = os.listdir(folder)
        except OSError:
            return
        if part not in names:
            match = [n for n in names if n.isascii() and n.lower() == part.lower()]
            if match:
                os.replace(folder / match[0], folder / part)
        folder = folder / part


def _install(root: Path, path: str, data: bytes, executable: bool) -> None:
    target = _target(root, path)
    _write_durable(target, data, 0o755 if executable else 0o644)
    _fix_case(root, path)


def download(plan: Plan, root: Optional[Path] = None, get: Fetch = fetch,
             progress: Optional[Progress] = None, cancelled: Callable[[], bool] = lambda: False) -> Path:
    """Downloads the plan's files into config/update/pending/, each checked
    against the signed size and SHA-256, and marks it ready for the next start."""
    root = root or release_root()
    with _Lock(wait=0):
        return _download(plan, root, get, progress, cancelled)


def _download(plan: Plan, root: Path, get: Fetch, progress: Optional[Progress],
              cancelled: Callable[[], bool]) -> Path:
    keys = load_keys(root)
    pending = update_dir() / "pending"
    if pending.exists():
        _guarded_rmtree(pending)
    files = pending / "files"
    done = 0
    try:
        for i, path in enumerate(plan.fetch):
            if cancelled():
                raise UpdateError("cancelled")
            sha, size = plan.release.files[path]
            try:
                data = get(_url(keys, plan.release.ref, path), size)
            except UpdateError as e:
                raise UpdateError(f"{path} couldn't be downloaded ({e}); nothing was changed") from e
            if plan.release.eol.get(path) == "crlf":
                data = _to_crlf(data)   # git keeps it with LF; the file has CRLF (as in a download of the release)
            if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
                raise UpdateError(f"{path} doesn't match the signed release; nothing was changed")
            _write_file(files / path, data)
            done += size
            if progress:
                progress(done / plan.download_bytes if plan.download_bytes else 1.0,
                         f"{i + 1}/{len(plan.fetch)}: {path}")
        pending.mkdir(parents=True, exist_ok=True)   # also when no file changed but the version
        (pending / "release.json").write_bytes(plan.release.manifest)
        (pending / "release.json.sig").write_bytes(plan.release.signature)
        ready = {"version": plan.release.version_text, "from": plan.installed.version_text, "tries": 0}
        (pending / "READY.json").write_text(json.dumps(ready), encoding="utf-8")
    except BaseException:
        if pending.exists():
            _guarded_rmtree(pending)   # never leave half an update behind
        raise
    return pending


def pending_version() -> str:
    try:
        d = json.loads((update_dir() / "pending" / "READY.json").read_text(encoding="utf-8"))
        return d.get("version", "") if isinstance(d, dict) else ""
    except (OSError, ValueError):
        return ""


def _target(root: Path, path: str) -> Path:
    """root/path, refusing a path that leads through a link or junction (or outside)."""
    if not safe_path(path) and path not in (MANIFEST_PATH, SIGNATURE_PATH):
        raise UpdateError(f"refused path {path!r}")
    target = root / path
    p = root
    for part in path.split("/")[:-1]:
        p = p / part
        if _is_link(p) or (p.exists() and not p.is_dir()):
            raise UpdateError(f"{p} isn't a plain folder; refused")
    if _is_link(target) or (target.exists() and not target.is_file()):
        raise UpdateError(f"{target} isn't a plain file; refused")
    return target


def _journal() -> Path:
    """Written before an update (or an undo) changes the first file, removed
    after the last: found at a start, the change was cut off."""
    return update_dir() / "applying.json"


def _raw_version(root: Path) -> str:
    """The version in this folder's release.json, unchecked ('' when none)."""
    try:
        d = json.loads((root / MANIFEST_PATH).read_text(encoding="utf-8"))
        v = d.get("version", "") if isinstance(d, dict) else ""
        return v if isinstance(v, str) else ""
    except (OSError, ValueError, RecursionError):
        return ""


_files_changed = False   # set when this run changed the tool's files (at_start then starts it again)


def _finish_interrupted(root: Path) -> str:
    """An update cut off halfway is rolled back; an undo cut off halfway is
    finished. Both put the files from config/update/previous/ back."""
    global _files_changed
    journal = _journal()
    if not journal.is_file():
        return ""
    previous = update_dir() / "previous"
    try:
        info = json.loads(journal.read_text(encoding="utf-8"))
        if not isinstance(info, dict):
            raise ValueError("not a journal")
        if info.get("format") != JOURNAL_FORMAT:
            raise ValueError(f"{journal} is from another version of the setup tool")
        _restore(root, info, previous)
    except (UpdateError, OSError, ValueError, TypeError) as e:
        return (f"An update was cut off halfway and the earlier files couldn't all be put back yet ({e}); "
                "that's tried again at the next start.")
    _files_changed = True
    journal.unlink()
    _fsync_dir(journal.parent)
    _guarded_rmtree(previous)
    if info.get("doing") == "undo":
        return f"The undo back to {info.get('from', 'the earlier release')} was cut off; it's finished now."
    return (f"The update to {info.get('to', '?')} was cut off halfway; the earlier files "
            f"({info.get('from', '?')}) were put back.")


_applied = ""   # the version this run put in place ('' if none)


def apply_pending(root: Optional[Path] = None) -> str:
    """Called first thing at start: finishes or rolls back a change that was
    cut off, then moves a downloaded update in place after checking it all
    again. Returns what happened ('' when nothing was to do)."""
    if Path(__file__).resolve().parent.parent.name != TOOL_DIR:
        return ""   # the launcher copy in a game folder never updates itself
    root = root or release_root()
    if git_checkout(root):
        return ""
    if not _journal().exists() and not (update_dir() / "pending" / "READY.json").exists():
        return ""
    with _Lock(wait=120):
        return _apply_pending(root)


def _apply_pending(root: Path) -> str:
    global _files_changed, _applied
    said: List[str] = []
    text = _finish_interrupted(root)
    if text:
        said.append(text)
        if _journal().exists():
            return text   # not everything is back: change nothing else
    pending = update_dir() / "pending"
    ready_path = pending / "READY.json"
    if not ready_path.is_file():
        return " ".join(said)
    try:
        ready = json.loads(ready_path.read_text(encoding="utf-8"))
        tries = int(ready.get("tries", 0))
    except (OSError, ValueError, AttributeError, TypeError):
        ready, tries = {}, 9
    if tries >= 3:
        _guarded_rmtree(pending)
        said.append("A downloaded update couldn't be put in place three times and was dropped; "
                    "check for updates again.")
        return " ".join(said)
    ready_path.write_text(json.dumps({**(ready if isinstance(ready, dict) else {}), "tries": tries + 1}),
                          encoding="utf-8")
    keys = load_keys(root)
    installed = installed_release(root, keys)
    state = read_state()
    try:
        release = verify_release((pending / "release.json").read_bytes(), (pending / "release.json.sig").read_bytes(),
                                 keys)
        if installed is None:
            raise UpdateError("this copy's own release information doesn't check out")
        if release.version == installed.version and release.manifest == installed.manifest:
            _guarded_rmtree(pending)   # put in place already (cut off just before this was cleared)
            return " ".join(said)
        check_rules(installed, release, keys, root, _seen(state, "highest_seen"),
                    _seen(state, "highest_recovery_seen"))
        plan = make_plan(root, installed, release)
        for path in plan.fetch:
            staged = pending / "files" / path
            sha, size = release.files[path]
            if _is_link(staged) or not staged.is_file() or staged.stat().st_size != size or _sha256(staged) != sha:
                raise UpdateError(f"the downloaded {path} is missing or changed")
            _target(root, path)
        for path in plan.remove:
            _target(root, path)
    except (UpdateError, OSError) as e:
        _guarded_rmtree(pending)
        said.append(f"A downloaded update wasn't used: {e}.")
        return " ".join(said)

    # 1. Keep what will be replaced (config/update/previous/), then the journal.
    previous = update_dir() / "previous"
    info = {"format": JOURNAL_FORMAT, "doing": "update", "from": installed.version_text,
            "to": release.version_text, "restore": [], "added": []}
    try:
        if (previous / "undo.json").exists():
            (previous / "undo.json").unlink()   # first: an old undo record never sits next to new copies
        if previous.exists():
            _guarded_rmtree(previous)
        if previous.exists():
            raise UpdateError(f"{previous} couldn't be emptied")
        names = _Names(root)
        for path in plan.fetch + plan.remove + [MANIFEST_PATH, SIGNATURE_PATH]:
            actual = names.on_disk(path)   # kept under the name it has here
            src = root / actual if actual else None
            if src is not None and src.is_file() and not _is_link(src):
                if actual not in info["restore"]:
                    _write_durable(previous / "files" / actual, src.read_bytes(), _mode(src))
                    info["restore"].append(actual)
            elif path in plan.fetch:
                info["added"].append(path)
        _write_durable(_journal(), json.dumps(info, indent=1).encode("utf-8"))
    except (UpdateError, OSError) as e:
        said.append(f"The update to {release.version_text} couldn't start ({e}); it's tried again at the next start.")
        return " ".join(said)
    # 2. Change the folder; 3. the undo record; 4. the journal goes.
    try:
        for path in plan.fetch:
            _install(root, path, (pending / "files" / path).read_bytes(), path in release.executable)
        for path in plan.remove:
            if _renamed_in_case(root, path, release):
                continue   # the same file as a new name (letter case): it was just written
            target = _target(root, path)
            if target.is_file():
                target.unlink()
            _prune_empty(root, path)
        _install(root, MANIFEST_PATH, release.manifest, False)
        _install(root, SIGNATURE_PATH, release.signature, False)
        _write_durable(previous / "undo.json", json.dumps({**info, "doing": "undo"}, indent=1).encode("utf-8"))
        _journal().unlink()
        _fsync_dir(update_dir())
    except (UpdateError, OSError) as e:
        try:
            _restore(root, info, previous)
            _journal().unlink()
            _guarded_rmtree(previous)
        except (UpdateError, OSError) as e2:
            said.append(f"The update to {release.version_text} stopped halfway ({e}), and the earlier files couldn't "
                        f"all be put back yet ({e2}); that's tried again at the next start.")
            return " ".join(said)
        said.append(f"The update to {release.version_text} stopped halfway ({e}); the earlier files were put back. "
                    "It's tried again at the next start.")
        return " ".join(said)
    _files_changed = True
    _applied = release.version_text
    _guarded_rmtree(pending)
    highest = _seen(state, "highest_seen")
    changes = {}
    if release.by_recovery:
        changes["highest_recovery_seen"] = release.version_text
        # New keys: what the old signing key put out (perhaps a made-up high
        # version from a stolen key) no longer blocks the real releases.
        if (release.files.get(KEYS_PATH) or (None,))[0] != (installed.files.get(KEYS_PATH) or (None,))[0]:
            changes["highest_seen"] = release.version_text
    if "highest_seen" not in changes and (not highest or release.version > highest):
        changes["highest_seen"] = release.version_text
    write_state(**changes)
    said.append(f"The setup tool was updated from {installed.version_text} to {release.version_text}.")
    return " ".join(said)


def _prune_empty(root: Path, path: str) -> None:
    p = (root / path).parent
    while p != root and root in p.parents:
        try:
            p.rmdir()   # only when empty
        except OSError:
            return
        p = p.parent


def _restore(root: Path, info: dict, previous: Path) -> None:
    """Puts back the files kept in `previous` and takes out the ones the
    update added. Doing it twice does no harm (a cut-off undo is redone).
    The updater's own files go back first, in the reverse order they were put
    in (a cut-off rollback leaves an old caller with a new callee at worst,
    which the next start handles)."""
    restore = [p for p in info.get("restore", []) if isinstance(p, str)]
    order = sorted(restore, key=lambda p: (0, -_LAST.index(p)) if p in _LAST else (1, 0))
    for path in order:
        src = previous / "files" / path
        if not src.is_file() or _is_link(src):
            raise UpdateError(f"the kept copy of {path} is missing")
        _install(root, path, src.read_bytes(), bool(_mode(src) & 0o100))
    for path in info.get("added", []):
        if not isinstance(path, str):
            continue
        target = _target(root, path)
        if any(r.lower() == path.lower() and _same_file(target, root / r) for r in restore):
            continue   # the same file as one just put back (a name in another letter case)
        if target.is_file():
            target.unlink()
            _prune_empty(root, path)


def can_undo() -> str:
    """The version an Undo goes back to, or ''."""
    try:
        undo = json.loads((update_dir() / "previous" / "undo.json").read_text(encoding="utf-8"))
        v = undo.get("from", "") if isinstance(undo, dict) else ""
        return v if isinstance(v, str) else ""
    except (OSError, ValueError):
        return ""


def undo_last(root: Optional[Path] = None) -> str:
    """Puts back the files the last update replaced (and takes out the ones it
    added)."""
    root = root or release_root()
    with _Lock(wait=10):
        return _undo_last(root)


def _undo_last(root: Path) -> str:
    previous = update_dir() / "previous"
    try:
        info = json.loads((previous / "undo.json").read_text(encoding="utf-8"))
        if not isinstance(info, dict):
            raise ValueError
    except (OSError, ValueError):
        return "There's no update to undo."
    if info.get("format") != JOURNAL_FORMAT:
        raise UpdateError("the undo record is from another version of the setup tool; nothing was undone")
    now = _raw_version(root)
    if now and now != info.get("to"):
        raise UpdateError(f"this folder is at {now} now, not {info.get('to')}; nothing was undone")
    pending = update_dir() / "pending"
    if pending.exists():
        _guarded_rmtree(pending)   # a downloaded update would undo the undo at the next start
    _write_durable(_journal(), json.dumps({**info, "doing": "undo"}, indent=1).encode("utf-8"))
    _restore(root, info, previous)
    _journal().unlink()
    _fsync_dir(update_dir())
    _guarded_rmtree(previous)
    return f"Back to {info.get('from', 'the earlier release')} (from {info.get('to', '?')})."


_PROBE = ("import sys\nsys.path.insert(0, sys.argv[1])\nimport cod3_waw_setup\n"
          "try:\n    import tkinter\nexcept ImportError:\n    tkinter = None\n"
          "if tkinter is not None:\n    import c3w.gui\n")


def _probe(script: str) -> Tuple[bool, str]:
    """Whether the tool's code, as it is on disk now, loads (in a new Python)."""
    tool = str(Path(script).resolve().parent)
    try:
        r = subprocess.run([sys.executable, "-c", _PROBE, tool], capture_output=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, str(e)
    last = (r.stderr.decode("utf-8", "replace").strip().splitlines() or [""])[-1]
    return r.returncode == 0, last[:300]


def at_start(script: str, argv: List[str]) -> Optional[int]:
    """For cod3_waw_setup.py, before it loads the rest of the tool:

    - `update --undo` is done here, so it works even when a bad release keeps
      the rest of the tool from loading;
    - a change that was cut off is finished or rolled back, and a downloaded
      update is put in place; when its code doesn't even load, it's undone at
      once;
    - when the tool's files changed, the tool starts again on them and that
      run's exit code is returned. None: go on as usual."""
    global _files_changed, _applied
    _files_changed, _applied = False, ""
    args = list(argv[1:])
    if args[:1] == ["update"] and "--undo" in args[1:]:
        try:
            print(undo_last())
        except (UpdateError, OSError, ValueError) as e:
            print(f"It couldn't be undone: {e}")
            return 1
        return 0
    try:
        text = apply_pending()
    except Exception as e:   # an update must never keep the tool from starting
        text = f"A downloaded update couldn't be put in place ({type(e).__name__}: {e})."
    if _journal().exists():
        # Half-changed files must not be loaded: stop here.
        print(text or "")
        print("The setup tool's files are being changed by another copy, or a change was cut off and couldn't be "
              "finished; start it again in a moment (or: update --undo).")
        return 1
    if not text:
        return None
    print(text)
    if _applied:
        ok, why = _probe(script)
        if not ok:
            try:
                back = undo_last()
            except (UpdateError, OSError, ValueError) as e:
                back = f"It couldn't be undone ({e}): run  update --undo."
            text = f"Setup tool {_applied} didn't start ({why}), so the update was undone. {back}"
            print(text)
    os.environ["C3W_UPDATED"] = text   # the window shows it
    if not _files_changed:
        return None
    return subprocess.call([sys.executable, str(Path(script).resolve())] + args)


# ---------------------------------------------------------------------------
# For tools/sign_release.py
# ---------------------------------------------------------------------------
def build_manifest(root: Path, paths: List[str], version_text: str, notes: str, date: str,
                   contents: Optional[Dict[str, Tuple[bytes, str]]] = None,
                   executable: Optional[set] = None) -> bytes:
    """release.json for the files `paths`: their bytes as a player gets them
    (`contents`: path -> (bytes, eol), else read from `root`) and which are
    executable (else from the file's mode here)."""
    parse_version(version_text)
    files = {}
    for path in sorted(set(paths)):
        if not safe_path(path):
            raise UpdateError(f"{path!r} can't be in a release (see update.safe_path)")
        if contents is not None:
            data, eol = contents[path]
        else:
            p = root / path
            if _is_link(p) or not p.is_file():
                raise UpdateError(f"{path} isn't a plain file")
            data, eol = p.read_bytes(), ""
        if eol not in EOLS:
            raise UpdateError(f"{path}: unknown line ending conversion {eol!r}")
        entry = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
        if eol:
            entry["eol"] = eol
        if executable is not None:
            x = path in executable
        else:
            x = os.name != "nt" and bool(_mode(root / path) & 0o100)
        if x:
            entry["exec"] = True
        files[path] = entry
    doc = {"format": FORMAT, "product": PRODUCT, "version": version_text, "ref": f"v{version_text}",
           "date": date, "notes": notes, "files": files}
    return (json.dumps(doc, indent=1, sort_keys=True, ensure_ascii=True) + "\n").encode("ascii")

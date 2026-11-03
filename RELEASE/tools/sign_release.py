#!/usr/bin/env python3
"""Signs a release of this project folder for the setup tool's updater
(docs/UPDATES.md). Run on your own PC, in the folder you publish (your GitHub
clone's RELEASE/): the keys never leave this PC.

    python3 tools/sign_release.py keygen     # once: makes the signing and recovery keys
    python3 tools/sign_release.py sign       # each release: sign, commit, tag
    python3 tools/sign_release.py sign --push
    python3 tools/sign_release.py verify     # check a folder's release.json against its files

keygen writes two passphrase-protected keys into your own settings folder
(~/.config/cod3-release-keys on Linux, %APPDATA%\\cod3-release-keys on
Windows), never into the project:
  signing-key.json    signs every release;
  recovery-key.json   only for replacing the signing key (if it's lost or
                      stolen): move it to a USB stick or another safe place.
It writes the two public keys and the repository's name into
COD_3_WAW_Setup-tool/c3w/update_keys.json, which ships with the release.

sign hashes every file git would publish here (the player's folders input/,
output/, work/, config/ and dev/ never), refuses files that tools/check_repo.py
flags (game data, personal paths) and any private key file, writes
COD_3_WAW_Setup-tool/release.json with the version from c3w/pipeline.py and
its signature release.json.sig, checks the result, then commits this folder
and tags it v<version>. --push pushes the commit and the tag. Players' setup
tools fetch release.json from the repository's default branch and the files
from that tag.
"""
from __future__ import annotations

import argparse
import datetime
import getpass
import hashlib
import hmac
import io
import json
import os
import re
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "COD_3_WAW_Setup-tool"))
from c3w import ed25519, update  # noqa: E402

KEY_FORMAT = 1
SCRYPT_N, SCRYPT_R, SCRYPT_P = 1 << 17, 8, 1   # 128 MB of memory per try


def key_dir() -> Path:
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming")
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    return base / "cod3-release-keys"


# ---------------------------------------------------------------------------
# Passphrase-protected key files (scrypt; a SHA-256 key stream; HMAC-SHA-256)
# ---------------------------------------------------------------------------
def _derive(passphrase: str, salt: bytes, n: int, r: int, p: int) -> bytes:
    return hashlib.scrypt(passphrase.encode("utf-8"), salt=salt, n=n, r=r, p=p, maxmem=1 << 28, dklen=64)


def _stream(key: bytes, nonce: bytes, length: int) -> bytes:
    out = b""
    counter = 0
    while len(out) < length:
        out += hashlib.sha256(key + nonce + counter.to_bytes(8, "little")).digest()
        counter += 1
    return out[:length]


def _header(d: dict) -> bytes:
    fields = {k: d[k] for k in ("format", "role", "public_key", "kdf", "salt", "nonce")}
    return json.dumps(fields, sort_keys=True).encode("ascii")


def seal(secret: bytes, passphrase: str, role: str) -> dict:
    d = {"format": KEY_FORMAT, "role": role, "public_key": ed25519.public_key(secret).hex(),
         "kdf": {"name": "scrypt", "n": SCRYPT_N, "r": SCRYPT_R, "p": SCRYPT_P},
         "salt": os.urandom(16).hex(), "nonce": os.urandom(16).hex()}
    k = _derive(passphrase, bytes.fromhex(d["salt"]), SCRYPT_N, SCRYPT_R, SCRYPT_P)
    ct = bytes(a ^ b for a, b in zip(secret, _stream(k[:32], bytes.fromhex(d["nonce"]), len(secret))))
    d["ciphertext"] = ct.hex()
    d["tag"] = hmac.new(k[32:], _header(d) + ct, hashlib.sha256).hexdigest()
    return d


def unseal(d: dict, passphrase: str) -> bytes:
    try:
        kdf = d["kdf"]
        if d.get("format") != KEY_FORMAT or kdf.get("name") != "scrypt":
            raise ValueError
        k = _derive(passphrase, bytes.fromhex(d["salt"]), int(kdf["n"]), int(kdf["r"]), int(kdf["p"]))
        ct = bytes.fromhex(d["ciphertext"])
        tag = hmac.new(k[32:], _header(d) + ct, hashlib.sha256).hexdigest()
    except (KeyError, ValueError, TypeError, AttributeError):
        sys.exit("That key file is damaged or not one of these keys.")
    if not isinstance(d.get("tag"), str) or not hmac.compare_digest(tag, d["tag"]):
        sys.exit("Wrong passphrase (or the key file was changed).")
    secret = bytes(a ^ b for a, b in zip(ct, _stream(k[:32], bytes.fromhex(d["nonce"]), len(ct))))
    if ed25519.public_key(secret).hex() != d.get("public_key"):
        sys.exit("The key file doesn't match its public key.")
    return secret


def _write_private(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def _ask_new_passphrase(what: str) -> str:
    while True:
        a = getpass.getpass(f"New passphrase for the {what} (at least 12 characters): ")
        if len(a) < 12:
            print("Too short.")
            continue
        if getpass.getpass("The same again: ") != a:
            print("They don't match; again.")
            continue
        return a


def fingerprint(pub: bytes) -> str:
    h = hashlib.sha256(pub).hexdigest()[:24]
    return " ".join(h[i:i + 4] for i in range(0, len(h), 4))


# ---------------------------------------------------------------------------
# git
# ---------------------------------------------------------------------------
def git(*args: str, cwd: Path = ROOT, check: bool = True, stdin: Optional[str] = None) -> str:
    try:
        r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, input=stdin)
    except OSError:
        sys.exit("git isn't installed; it's needed to know what is published.")
    if check and r.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{r.stderr.strip()}")
    return r.stdout


def repo_and_prefix(root: Path) -> Tuple[str, str]:
    """('owner/name', 'RELEASE/') of the GitHub repository this folder is in."""
    prefix = git("rev-parse", "--show-prefix", cwd=root).strip()
    url = git("remote", "get-url", "origin", cwd=root).strip()
    m = re.search(r"github\.com[:/]([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+?)(\.git)?/?$", url)
    if not m:
        sys.exit(f"The 'origin' remote isn't a GitHub repository ({url}).")
    return f"{m.group(1)}/{m.group(2)}", prefix


_SEALED_KEY = re.compile(rb'"ciphertext"\s*:\s*"[0-9a-f]{64}"')


def private_key_files(root: Path, paths: List[str]) -> List[str]:
    """Published files that are (or hold) one of these key files: by name, or
    by the sealed key's contents, whatever the file is called."""
    found = []
    for path in paths:
        name = path.rsplit("/", 1)[-1].casefold()
        if "signing-key" in name or "recovery-key" in name:
            found.append(path)
            continue
        try:
            data = (root / path).read_bytes()
        except OSError:
            continue
        if _SEALED_KEY.search(data):
            found.append(path)
    return found


def git_bytes(*args: str, cwd: Path, stdin: Optional[bytes] = None) -> bytes:
    try:
        r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, input=stdin)
    except OSError:
        sys.exit("git isn't installed; it's needed to know what is published.")
    if r.returncode != 0:
        sys.exit(f"git {args[0]} failed:\n{r.stderr.decode('utf-8', 'replace').strip()}")
    return r.stdout


def cat_blobs(top: Path, names: List[str]) -> List[bytes]:
    """The contents of git objects named like <tree or tag>:<path>."""
    out = git_bytes("cat-file", "--batch", cwd=top, stdin=("\n".join(names) + "\n").encode("utf-8"))
    blobs, i = [], 0
    for name in names:
        nl = out.index(b"\n", i)
        header = out[i:nl].decode("utf-8", "replace").split()
        if len(header) < 3 or header[1] != "blob":
            sys.exit(f"git has no file {name}")
        size = int(header[2])
        blobs.append(out[nl + 1:nl + 1 + size])
        i = nl + 1 + size + 1
    return blobs


def stage_release(root: Path, paths: List[str], with_manifest: bool) -> List[str]:
    """Stages exactly the release (its files, the removal of tracked files
    that are gone, and with_manifest release.json and its signature): a
    settings file, a log or anything in the player's folders stays out."""
    gone = [p for p in git("ls-files", "-z", "--deleted", cwd=root).split("\0") if p]
    spec = paths + gone + ([update.MANIFEST_PATH, update.SIGNATURE_PATH] if with_manifest else [])
    git("--literal-pathspecs", "add", "-A", "--pathspec-from-file=-", "--pathspec-file-nul", cwd=root,
        stdin="\0".join(spec) + "\0")
    return spec


def commit_release(root: Path, paths: List[str], version: str) -> None:
    spec = stage_release(root, paths, with_manifest=True)
    git("--literal-pathspecs", "commit", "-q", "-m", f"Release {version}", "--pathspec-from-file=-",
        "--pathspec-file-nul", cwd=root, stdin="\0".join(spec) + "\0")


def _archive(top: Path, treeish: str, prefix: str) -> Dict[str, bytes]:
    """Each file of `treeish` under `prefix` as git gives it out in a download
    (line endings by the repository's .gitattributes only: this PC's own git
    settings are left out, as GitHub doesn't have them)."""
    git_dir = Path(git("rev-parse", "--absolute-git-dir", cwd=top).strip())
    info = git_dir / "info" / "attributes"
    if info.is_file() and info.read_text(encoding="utf-8", errors="replace").strip():
        sys.exit(f"{info} changes line endings on this PC only; empty it (or move it) before signing.")
    env = {**os.environ, "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull}
    try:
        r = subprocess.run(["git", "archive", "--format=tar", treeish, *([prefix] if prefix else [])], cwd=top,
                           capture_output=True, env=env)
    except OSError:
        sys.exit("git isn't installed; it's needed to know what is published.")
    if r.returncode != 0:
        sys.exit(f"git archive failed:\n{r.stderr.decode('utf-8', 'replace').strip()}")
    out = {}
    with tarfile.open(fileobj=io.BytesIO(r.stdout)) as t:
        for m in t.getmembers():
            if m.isfile() and m.name.startswith(prefix):
                f = t.extractfile(m)
                out[m.name[len(prefix):]] = f.read() if f else b""
    return out


def release_contents(root: Path, prefix: str, paths: List[str]):
    """Each file as a player gets it: as git gives it out in a download of the
    release (`git archive`, line endings by .gitattributes), and how that
    comes from git's own copy, which is what an update downloads ('' the same,
    'crlf' LF to CRLF). Returns (contents, executable, differ_here, bad)."""
    top = Path(git("rev-parse", "--show-toplevel", cwd=root).strip())
    tree = git("write-tree", cwd=root).strip()
    blobs = cat_blobs(top, [f"{tree}:{prefix}{p}" for p in paths])
    archived = _archive(top, tree, prefix)
    executable = set()
    for line in git("ls-files", "-s", "-z", cwd=root).split("\0"):
        if line.startswith("100755 "):
            executable.add(line.split("\t", 1)[1])
    contents, differ, bad = {}, [], []
    for path, blob in zip(paths, blobs):
        data = archived.get(path)
        if data is None:
            bad.append(f"{path} (not in a download of the release: export-ignore?)")
            continue
        if data == blob:
            contents[path] = (data, "")
        elif data == update._to_crlf(blob):
            contents[path] = (data, "crlf")
        else:
            bad.append(f"{path} (git's copy and the download differ: Git LFS or a filter?)")
            continue
        try:
            if (root / path).read_bytes() != data:
                differ.append(path)
        except OSError:
            differ.append(path)
    return contents, executable & set(paths), differ, bad


def verify_tag(root: Path, prefix: str, tag: str, release: update.Release) -> List[str]:
    """What an update would download from `tag`, checked against the signed
    release (the files that wouldn't check out)."""
    top = Path(git("rev-parse", "--show-toplevel", cwd=root).strip())
    paths = sorted(release.files)
    blobs = cat_blobs(top, [f"{tag}:{prefix}{p}" for p in paths] +
                      [f"{tag}:{prefix}{update.MANIFEST_PATH}", f"{tag}:{prefix}{update.SIGNATURE_PATH}"])
    bad = []
    for path, blob in zip(paths, blobs):
        data = update._to_crlf(blob) if release.eol.get(path) == "crlf" else blob
        sha, size = release.files[path]
        if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
            bad.append(path)
    if blobs[-2] != release.manifest or blobs[-1] != release.signature:
        bad.append(update.MANIFEST_PATH)
    # And what a download of the tag (a zip from GitHub) gives a new player.
    archived = _archive(top, tag, prefix)
    for path in paths:
        data = archived.get(path)
        sha, size = release.files[path]
        if data is None or len(data) != size or hashlib.sha256(data).hexdigest() != sha:
            bad.append(f"{path} (download)")
    if archived.get(update.MANIFEST_PATH) != release.manifest or \
            archived.get(update.SIGNATURE_PATH) != release.signature:
        bad.append(f"{update.MANIFEST_PATH} (download)")
    return bad


def unpushed_check(root: Path, tag: str) -> List[str]:
    """Before a push: the commits it would publish besides the release's, and
    any key file in anything it would publish (also files a later commit
    deleted, and files outside this folder). Exits when a key is found."""
    upstream = git("rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}", cwd=root, check=False).strip()
    if not upstream:
        sys.exit("This branch has no upstream on GitHub yet; push it yourself once (git push -u origin HEAD).")
    top = Path(git("rev-parse", "--show-toplevel", cwd=root).strip())
    objects = [line.split(" ", 1) for line in git("rev-list", "--objects", f"{upstream}..{tag}", cwd=top).splitlines()]
    ids = [o[0] for o in objects]
    if ids:
        types = git_bytes("cat-file", "--batch-check", cwd=top, stdin=("\n".join(ids) + "\n").encode())
        blob_ids = [line.split()[0] for line in types.decode().splitlines() if line.split()[1:2] == ["blob"]]
        if blob_ids:
            for name, data in zip(blob_ids, cat_blobs(top, blob_ids)):
                if _SEALED_KEY.search(data):
                    where = next((o[1] for o in objects if o[0] == name and len(o) > 1), name)
                    sys.exit(f"Not pushed: a key file ({where}) is in a commit that would be published. "
                             "Remove it from the history first.")
    commits = git("log", "--format=%h %s", f"{upstream}..{tag}~1", cwd=top, check=False).splitlines()
    return commits


def _inside_repository(folder: Path, root: Path) -> bool:
    folder, root = folder.resolve(), root.resolve()
    top = git("rev-parse", "--show-toplevel", cwd=root, check=False).strip()
    for base in [root] + ([Path(top).resolve()] if top else []):
        if folder == base or base in folder.parents:
            return True
    return False


def _confirm(question: str) -> bool:
    try:
        return input(question).strip().lower() in ("y", "yes")
    except EOFError:
        return False


def published_files(root: Path) -> List[str]:
    """What git publishes from this folder (tracked plus new files it doesn't
    ignore), minus what is never in a release."""
    out = git("ls-files", "-z", "--cached", "--others", "--exclude-standard", cwd=root)
    paths = []
    for path in sorted(set(p for p in out.split("\0") if p)):
        parts = path.casefold().split("/")
        if parts[0] in update.USER_DIRS or ".git" in parts or "__pycache__" in parts or path.casefold().endswith(".pyc"):
            continue
        if path in (update.MANIFEST_PATH, update.SIGNATURE_PATH):
            continue
        p = root / path
        if not p.is_file() or update._is_link(p):
            continue   # deleted here (the commit removes it from git too)
        paths.append(path)
    return paths


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def cmd_keygen(a: argparse.Namespace) -> int:
    root = Path(a.root).resolve()
    folder = Path(a.dir) if a.dir else key_dir()
    signing_path, recovery_path = folder / "signing-key.json", folder / "recovery-key.json"
    if _inside_repository(folder, root):
        sys.exit(f"{folder} is inside the repository; the keys must stay outside it (use the default or --dir).")
    if signing_path.exists() or recovery_path.exists():
        sys.exit(f"Keys are already in {folder}. Remove them first only if you really mean to replace them.")
    if a.repo:
        repo, prefix = a.repo, a.prefix or ""
    else:
        repo, prefix = repo_and_prefix(root)
    keys_file = root / update.KEYS_PATH
    print(f"Release folder: {root}\nGitHub repository: {repo}, folder '{prefix or '(top)'}'\n")
    sp = _ask_new_passphrase("signing key")
    rp = _ask_new_passphrase("recovery key (a different one)")
    if rp == sp:
        print("Note: the same passphrase for both keys; a different one would be safer.")
    signing, recovery = os.urandom(32), os.urandom(32)
    _write_private(signing_path, seal(signing, sp, "signing"))
    _write_private(recovery_path, seal(recovery, rp, "recovery"))
    sp_pub, rp_pub = ed25519.public_key(signing), ed25519.public_key(recovery)
    keys = {"format": update.FORMAT, "source": {"repo": repo, "prefix": prefix},
            "signing": [sp_pub.hex()], "recovery": [rp_pub.hex()]}
    keys_file.write_bytes((json.dumps(keys, indent=2) + "\n").encode("ascii"))   # LF, as git gives it out
    print(f"\nSigning key:  {signing_path}\n  fingerprint {fingerprint(sp_pub)}")
    print(f"Recovery key: {recovery_path}\n  fingerprint {fingerprint(rp_pub)}")
    print(f"\nThe public keys are in {keys_file}.")
    print("Next:\n  1. Copy recovery-key.json to a USB stick (or another offline place), then delete it here.\n"
          "  2. Write the passphrases down somewhere safe; a lost passphrase can't be recovered.\n"
          "  3. Release: python3 tools/sign_release.py sign --push")
    return 0


def _tool_version(root: Path) -> Tuple[str, str]:
    text = (root / update.TOOL_DIR / "c3w" / "pipeline.py").read_text(encoding="utf-8")
    m = re.search(r'^TOOL_VERSION = "([0-9.]+)"(?:\s*#\s*(.*))?$', text, re.M)
    if not m:
        sys.exit("TOOL_VERSION isn't in c3w/pipeline.py.")
    version, comment = m.group(1), m.group(2) or ""
    # The notes: this version's entry of the comment, without the chats' [tags].
    notes = ""
    m2 = re.match(rf"\s*{re.escape(version)}:\s*(.*?)(?=\s\d+\.\d+\.\d+:\s|$)", comment)
    if m2:
        notes = re.sub(r"\[[^\]]{1,40}\]\s*", "", m2.group(1)).strip()
    return version, notes[:3000]


def _load_key(path: Path, role: str) -> bytes:
    try:
        d = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        sys.exit(f"Can't read the {role} key {path}.")
    return unseal(d, getpass.getpass(f"Passphrase for the {role} key ({path}): "))


def cmd_sign(a: argparse.Namespace) -> int:
    root = Path(a.root).resolve()
    keys = update.load_keys(root)
    if not keys.configured:
        sys.exit("COD_3_WAW_Setup-tool/c3w/update_keys.json has no keys yet: run  python3 tools/sign_release.py keygen")
    repo, prefix = repo_and_prefix(root)
    if (repo, prefix) != (keys.repo, keys.prefix):
        sys.exit(f"update_keys.json says the releases come from {keys.repo} '{keys.prefix}', but this folder is "
                 f"{repo} '{prefix}'. Sign in the folder you publish.")
    if _inside_repository(key_dir(), root):
        sys.exit(f"The keys folder {key_dir()} is inside the repository; move it out.")
    version, notes = _tool_version(root)
    notes = a.notes or notes
    if git("tag", "--list", f"v{version}", cwd=root).strip():
        sys.exit(f"v{version} is released already: bump TOOL_VERSION in c3w/pipeline.py first.")
    # The last release signed here (read as it is: after a key change the
    # current keys may not verify it).
    previous = {}
    try:
        previous = json.loads((root / update.MANIFEST_PATH).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        pass
    if previous and update.parse_version(version) <= update.parse_version(previous.get("version", "0.0.0")):
        sys.exit(f"{version} isn't newer than the last signed release here ({previous.get('version')}).")
    paths = published_files(root)
    if update.KEYS_PATH not in paths:
        sys.exit(f"{update.KEYS_PATH} isn't in git; add it.")
    keys_found = private_key_files(root, paths)
    if keys_found:
        sys.exit("A private key file is in the release folder; move it out (it must never be published):\n  "
                 + "\n  ".join(keys_found))
    try:
        import check_repo   # tools/check_repo.py, as CI runs it
    except ImportError:
        sys.exit("tools/check_repo.py is missing next to this script.")
    problems = check_repo.check_files(root, [root / p for p in paths])
    if problems:
        sys.exit("Not signed: these files must not be published:\n  " + "\n  ".join(problems[:50]))
    bad_names = [p for p in paths if not update.safe_path(p)]
    if bad_names:
        sys.exit("Not signed: these names can't be in a release (plain ASCII names that Windows allows):\n  "
                 + "\n  ".join(bad_names[:50]))
    tracked = set(p for p in git("ls-files", "-z", cwd=root).split("\0") if p)
    new = [p for p in paths if p not in tracked]
    if new:
        print(f"{len(new)} new file(s) will be published (not in git yet):")
        for p in new[:40]:
            print(f"  {p}")
        if len(new) > 40:
            print(f"  ... and {len(new) - 40} more")
        if not a.yes and not _confirm("Publish them? [y/N] "):
            sys.exit("Nothing was signed. (Add files you don't want published to .gitignore, or move them out.)")
    contents = executable = None
    if not a.no_git:
        stage_release(root, paths, with_manifest=False)
        contents, executable, differ, bad = release_contents(root, prefix, paths)
        if bad:
            sys.exit("Not signed: these files wouldn't reach players as they are signed:\n  " + "\n  ".join(bad[:50]))
        if differ:
            print(f"{len(differ)} file(s) here differ from how git gives them out (line endings); the release has "
                  "git's version: " + ", ".join(differ[:10]) + (" ..." if len(differ) > 10 else ""))
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    try:
        manifest = update.build_manifest(root, paths, version, notes, date, contents, executable)
    except update.UpdateError as e:
        sys.exit(str(e))
    # A release that changes the keys (or where releases come from) needs the
    # recovery key, as the setup tools will check.
    old_keys = (previous.get("files") or {}).get(update.KEYS_PATH, {}) if previous else {}
    new_keys_sha = json.loads(manifest)["files"][update.KEYS_PATH]["sha256"]   # as git gives the file out
    keys_changed = bool(previous) and old_keys.get("sha256") != new_keys_sha
    # The keys players have now: the last release's (from its tag), else these.
    players_keys = keys
    if previous and not a.no_git:
        old_tag = f"v{previous.get('version', '')}"
        if git("tag", "--list", old_tag, cwd=root).strip():
            top = Path(git("rev-parse", "--show-toplevel", cwd=root).strip())
            try:
                players_keys = update.parse_keys(cat_blobs(top, [f"{old_tag}:{prefix}{update.KEYS_PATH}"])[0])
            except update.UpdateError as e:
                sys.exit(f"The last release's update_keys.json can't be read ({e}).")
    role = "recovery" if keys_changed or a.recovery else "signing"
    key_path = Path(a.key) if a.key else key_dir() / f"{role}-key.json"
    if keys_changed:
        print("update_keys.json changed since the last release: this release must be signed with the recovery key.")
    print(f"Release {version}: {len(paths)} files from {root}")
    secret = _load_key(key_path, role)
    pub = ed25519.public_key(secret)
    if pub not in (players_keys.recovery if role == "recovery" else players_keys.signing):
        sys.exit(f"That {role} key isn't one of the {role} keys players' setup tools trust now "
                 "(the last release's update_keys.json).")
    signature = update.signature_file(pub, ed25519.sign(secret, update.SIGNED_PREFIX + manifest))
    try:
        release = update.verify_release(manifest, signature, players_keys)   # as the setup tools will
    except update.UpdateError as e:
        sys.exit(f"The signed release didn't check out ({e}); nothing was written.")
    (root / update.MANIFEST_PATH).write_bytes(manifest)
    (root / update.SIGNATURE_PATH).write_bytes(signature)
    print(f"Signed with the {role} key ({fingerprint(pub)}).")
    if a.no_git:
        return 0
    commit_release(root, paths, version)
    tag = f"v{version}"
    try:
        git("tag", "-a", tag, "-m", f"Release {version}", cwd=root)
        bad = verify_tag(root, prefix, tag, release)
        if bad:
            sys.exit(f"What players would get from {tag} doesn't match the signed release ({len(bad)} file(s): "
                     f"{', '.join(bad[:10])}); nothing was pushed.")
        print(f"Committed and tagged {tag}; every file at the tag checks out (updates and downloads).")
        if a.push:
            others = unpushed_check(root, tag)
            if others:
                print(f"The push also publishes {len(others)} earlier commit(s) of yours:")
                for c in others[:20]:
                    print(f"  {c}")
                if not a.yes and not _confirm("Push them too? [y/N] "):
                    sys.exit("Nothing was pushed.")
            git("push", "--atomic", "origin", "HEAD", f"refs/tags/{tag}", cwd=root)
            print("Pushed. Players' setup tools see it at their next check.")
            return 0
    except SystemExit:
        # Leave nothing a later push could publish: no tag, no release commit
        # (its changes stay staged).
        git("tag", "-d", tag, cwd=root, check=False)
        git("reset", "-q", "--soft", "HEAD~1", cwd=root, check=False)
        print(f"The release commit and the tag {tag} were taken back; the files are as they were, staged.")
        raise
    print(f"To publish:  git push --atomic origin HEAD refs/tags/{tag}")
    return 0


def cmd_verify(a: argparse.Namespace) -> int:
    root = Path(a.root).resolve()
    keys = update.load_keys(root)
    try:
        release = update.verify_release((root / update.MANIFEST_PATH).read_bytes(),
                                        (root / update.SIGNATURE_PATH).read_bytes(), keys)
    except OSError:
        print("No release.json / release.json.sig here.")
        return 1
    except update.UpdateError as e:
        print(f"The release information doesn't check out: {e}")
        return 1
    bad = []
    for path, (sha, size) in release.files.items():
        p = root / path
        if not p.is_file() or p.stat().st_size != size or update._sha256(p) != sha:
            bad.append(path)
    if bad:
        print(f"{len(bad)} file(s) differ from release {release.version_text}:")
        for p in bad[:50]:
            print(f"  {p}")
        return 1
    if not getattr(a, "quiet", False):
        print(f"Release {release.version_text}: signature and all {len(release.files)} files check out "
              f"({'recovery' if release.by_recovery else 'signing'} key {fingerprint(release.key)}).")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Sign releases for the setup tool's updater.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--root", default=str(ROOT), help="the release folder (default: the one this script is in)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen", help="make the signing and recovery keys (once)")
    k.add_argument("--dir", help=f"where the key files go (default {key_dir()})")
    k.add_argument("--repo", help="owner/name on GitHub (default: from git's origin)")
    k.add_argument("--prefix", help="the release folder inside the repository, like RELEASE/ (with --repo)")
    k.set_defaults(func=cmd_keygen)
    s = sub.add_parser("sign", help="sign this folder as the release in c3w/pipeline.py's TOOL_VERSION")
    s.add_argument("--key", help="the key file (default: signing-key.json in the keys folder)")
    s.add_argument("--recovery", action="store_true", help="sign with the recovery key")
    s.add_argument("--notes", help="release notes (default: TOOL_VERSION's comment for this version)")
    s.add_argument("--push", action="store_true", help="also push the commit and the tag")
    s.add_argument("--no-git", action="store_true", help="only write release.json and its signature")
    s.add_argument("--yes", action="store_true", help="don't ask before publishing files that are new to git")
    s.set_defaults(func=cmd_sign)
    v = sub.add_parser("verify", help="check release.json, its signature and every file")
    v.set_defaults(func=cmd_verify, quiet=False)
    a = ap.parse_args()
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())

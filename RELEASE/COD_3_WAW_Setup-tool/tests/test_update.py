"""Tests for the setup tool's updater (c3w/update.py, c3w/ed25519.py,
tools/sign_release.py): the signature code against RFC 8032 and OpenSSL, and
every way a release is refused (another key, a changed byte, an old version,
a key change without the recovery key, paths out of the folder, files that
don't match), then a whole update and its undo on a made-up project folder.
Nothing is fetched from the network: the downloads come from a dict.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent.parent / "tools"))

from c3w import ed25519, update  # noqa: E402

# RFC 8032 section 7.1, TEST 1 and TEST SHA(abc).
RFC_VECTORS = [
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
     "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
     "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"),
]

SIGNING = bytes(range(1, 33))
RECOVERY = bytes(range(101, 133))
OTHER = bytes(range(201, 233))


class Ed25519Tests(unittest.TestCase):
    def test_rfc8032_vectors(self):
        for secret, public, message, signature in RFC_VECTORS:
            secret, public = bytes.fromhex(secret), bytes.fromhex(public)
            message, signature = bytes.fromhex(message), bytes.fromhex(signature)
            self.assertEqual(ed25519.public_key(secret), public)
            self.assertEqual(ed25519.sign(secret, message), signature)
            self.assertTrue(ed25519.verify(public, message, signature))

    def test_refusals(self):
        pub = ed25519.public_key(SIGNING)
        sig = ed25519.sign(SIGNING, b"release")
        self.assertTrue(ed25519.verify(pub, b"release", sig))
        self.assertFalse(ed25519.verify(pub, b"releasE", sig))
        self.assertFalse(ed25519.verify(ed25519.public_key(OTHER), b"release", sig))
        for i in (0, 31, 32, 63):
            bad = bytearray(sig)
            bad[i] ^= 1
            self.assertFalse(ed25519.verify(pub, b"release", bytes(bad)))
        self.assertFalse(ed25519.verify(pub, b"release", sig[:63]))
        self.assertFalse(ed25519.verify(pub[:31], b"release", sig))
        # S + L: the same S modulo the group order, which RFC 8032 refuses.
        s = int.from_bytes(sig[32:], "little") + ed25519.Q
        if s < 1 << 256:
            self.assertFalse(ed25519.verify(pub, b"release", sig[:32] + s.to_bytes(32, "little")))
        # A key that isn't a point (y >= p).
        self.assertFalse(ed25519.verify(b"\xff" * 32, b"release", sig))

    def test_openssl_agrees(self):
        """Signatures made here check out in OpenSSL and the other way round
        (skipped when this PC has no openssl with Ed25519)."""
        openssl = shutil.which("openssl")
        if not openssl:
            self.skipTest("no openssl")
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            # PKCS#8 for an Ed25519 seed: a fixed 16-byte header, then the 32 bytes.
            der = bytes.fromhex("302e020100300506032b657004220420") + SIGNING
            (t / "key.der").write_bytes(der)
            (t / "msg").write_bytes(b"cod3 release test")
            r = subprocess.run([openssl, "pkeyutl", "-sign", "-rawin", "-inkey", str(t / "key.der"), "-keyform",
                                "DER", "-in", str(t / "msg"), "-out", str(t / "sig")], capture_output=True)
            if r.returncode != 0:
                self.skipTest("this openssl can't sign Ed25519")
            self.assertEqual((t / "sig").read_bytes(), ed25519.sign(SIGNING, b"cod3 release test"))
            r = subprocess.run([openssl, "pkey", "-in", str(t / "key.der"), "-inform", "DER", "-pubout", "-outform",
                                "DER", "-out", str(t / "pub.der")], capture_output=True)
            self.assertEqual(r.returncode, 0)
            self.assertEqual((t / "pub.der").read_bytes()[-32:], ed25519.public_key(SIGNING))


def write(root: Path, rel: str, data: bytes) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)


def keys_file(repo="owner/cod3", prefix="RELEASE/", signing=SIGNING, recovery=RECOVERY) -> bytes:
    return json.dumps({"format": 1, "source": {"repo": repo, "prefix": prefix},
                       "signing": [ed25519.public_key(signing).hex()],
                       "recovery": [ed25519.public_key(recovery).hex()]}).encode()


def signed(root: Path, version: str, secret=SIGNING, paths=None):
    """(release.json, its signature) for the files of `root`."""
    if paths is None:
        paths = sorted(p.relative_to(root).as_posix() for p in root.rglob("*")
                       if p.is_file() and update.safe_path(p.relative_to(root).as_posix()))
    manifest = update.build_manifest(root, paths, version, f"notes for {version}", "2026-11-03T00:00:00Z")
    sig = update.signature_file(ed25519.public_key(secret), ed25519.sign(secret, update.SIGNED_PREFIX + manifest))
    return manifest, sig


class Folder:
    """A made-up project folder (the installed copy) and a server (a dict of
    URL -> bytes) with the next release."""

    def __init__(self, tmp: Path):
        self.tmp = tmp
        self.root = tmp / "project"
        self.server_tree = tmp / "server"
        self.config = tmp / "config"
        self.urls = {}
        self.fetched = []
        update.update_dir = lambda: self.config / "update"
        update._state_path = lambda: self.config / "updates.json"
        for r in (self.root, self.server_tree):
            write(r, "setup.sh", b"#!/bin/sh\n")
            write(r, update.KEYS_PATH, keys_file())
            write(r, "COD_3_WAW_Setup-tool/c3w/gui.py", b"old gui\n")
            write(r, "COD_3_WAW_Setup-tool/patches/0001-old.patch", b"old patch\n")
            write(r, "docs/README.md", b"docs\n")
        manifest, sig = signed(self.root, "2.0.0")
        write(self.root, update.MANIFEST_PATH, manifest)
        write(self.root, update.SIGNATURE_PATH, sig)
        # The player's own folders.
        write(self.root, "output/game.bin", b"game")
        write(self.root, "config/settings.json", b"{}")

    def publish(self, version: str, secret=SIGNING):
        """The server's tree as release `version`."""
        manifest, sig = signed(self.server_tree, version, secret)
        keys = update.parse_keys(keys_file())
        self.urls = {update._url(keys, "HEAD", update.MANIFEST_PATH): manifest,
                     update._url(keys, "HEAD", update.SIGNATURE_PATH): sig}
        doc = json.loads(manifest)
        for path in doc["files"]:
            self.urls[update._url(keys, doc["ref"], path)] = (self.server_tree / path).read_bytes()
        return manifest, sig

    def get(self, url: str, limit: int) -> bytes:
        self.fetched.append(url)
        if url not in self.urls:
            raise update.UpdateError(f"404 {url}")
        data = self.urls[url]
        if len(data) > limit:
            raise update.UpdateError("too big")
        return data


class UpdaterTests(unittest.TestCase):
    def setUp(self):
        self._saved = (update.update_dir, update._state_path)
        self._tmp = tempfile.TemporaryDirectory()
        self.f = Folder(Path(self._tmp.name))

    def tearDown(self):
        update.update_dir, update._state_path = self._saved
        self._tmp.cleanup()

    def test_checks_are_off_until_allowed(self):
        self.assertIsNone(update.checks_allowed())
        update.set_checks_allowed(False)
        self.assertIs(update.checks_allowed(), False)
        update.set_checks_allowed(True)
        self.assertIs(update.checks_allowed(), True)

    def test_paths(self):
        for good in ("COD_3_WAW_Setup-tool/c3w/gui.py", "docs/a b.md", ".gitignore", "engine/src/lib.rs"):
            self.assertTrue(update.safe_path(good), good)
        for bad in ("", "/etc/passwd", "../x", "a/../b", "a//b", "./a", "a/./b", "C:/x", "a\\b", "input/x.iso",
                    "output/bin/cod3.exe", "work/x", "config/settings.json", "dev/agents/BOARD.md", ".git/config",
                    "a/__pycache__/b.py", "x.pyc", "con.txt", "docs/AUX", "a/b.", "a/b ", update.MANIFEST_PATH,
                    update.SIGNATURE_PATH, "a\x00b", "a|b"):
            self.assertFalse(update.safe_path(bad), bad)

    def test_whole_update_and_undo(self):
        f = self.f
        write(f.server_tree, "COD_3_WAW_Setup-tool/c3w/gui.py", b"new gui\n")
        write(f.server_tree, "COD_3_WAW_Setup-tool/c3w/new.py", b"new module\n")
        (f.server_tree / "COD_3_WAW_Setup-tool/patches/0001-old.patch").unlink()
        write(f.server_tree, "COD_3_WAW_Setup-tool/patches/0002-new.patch", b"new patch\n")
        f.publish("2.1.0")
        plan, message = update.check(f.root, f.get)
        self.assertEqual(message, "")
        self.assertEqual(plan.release.version_text, "2.1.0")
        self.assertEqual(sorted(plan.fetch), ["COD_3_WAW_Setup-tool/c3w/gui.py", "COD_3_WAW_Setup-tool/c3w/new.py",
                                              "COD_3_WAW_Setup-tool/patches/0002-new.patch"])
        self.assertEqual(plan.remove, ["COD_3_WAW_Setup-tool/patches/0001-old.patch"])
        self.assertEqual(plan.edited, [])
        update.download(plan, f.root, f.get)
        # Nothing in the folder changed yet.
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"old gui\n")
        self.assertEqual(update.pending_version(), "2.1.0")
        text = update.apply_pending(f.root)
        self.assertIn("updated from 2.0.0 to 2.1.0", text)
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"new gui\n")
        self.assertTrue((f.root / "COD_3_WAW_Setup-tool/patches/0002-new.patch").is_file())
        self.assertFalse((f.root / "COD_3_WAW_Setup-tool/patches/0001-old.patch").exists())
        self.assertEqual(update.installed_release(f.root, update.load_keys(f.root)).version_text, "2.1.0")
        self.assertEqual((f.root / "output/game.bin").read_bytes(), b"game")
        self.assertEqual((f.root / "config/settings.json").read_bytes(), b"{}")
        self.assertEqual(update.apply_pending(f.root), "")   # nothing pending any more
        plan, message = update.check(f.root, f.get)
        self.assertIsNone(plan)
        self.assertIn("up to date", message)
        # Undo.
        self.assertEqual(update.can_undo(), "2.0.0")
        update.undo_last(f.root)
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"old gui\n")
        self.assertTrue((f.root / "COD_3_WAW_Setup-tool/patches/0001-old.patch").is_file())
        self.assertFalse((f.root / "COD_3_WAW_Setup-tool/c3w/new.py").exists())
        self.assertEqual(update.installed_release(f.root, update.load_keys(f.root)).version_text, "2.0.0")

    def test_players_edits_are_kept_in_previous(self):
        f = self.f
        write(f.root, "docs/README.md", b"my own notes\n")
        write(f.server_tree, "docs/README.md", b"docs, new\n")
        f.publish("2.1.0")
        plan, _ = update.check(f.root, f.get)
        self.assertEqual(plan.edited, ["docs/README.md"])
        update.download(plan, f.root, f.get)
        update.apply_pending(f.root)
        self.assertEqual((f.config / "update/previous/files/docs/README.md").read_bytes(), b"my own notes\n")

    def test_another_key_is_refused(self):
        f = self.f
        write(f.server_tree, "docs/README.md", b"evil\n")
        f.publish("9.0.0", secret=OTHER)
        with self.assertRaisesRegex(update.UpdateError, "doesn't trust"):
            update.check(f.root, f.get)

    def test_changed_byte_is_refused(self):
        f = self.f
        manifest, sig = f.publish("2.1.0")
        keys = update.load_keys(f.root)
        f.urls[update._url(keys, "HEAD", update.MANIFEST_PATH)] = manifest.replace(b"2.1.0", b"2.2.0", 1)
        with self.assertRaisesRegex(update.UpdateError, "signature"):
            update.check(f.root, f.get)

    def test_file_that_doesnt_match_is_refused(self):
        f = self.f
        write(f.server_tree, "COD_3_WAW_Setup-tool/c3w/gui.py", b"new gui\n")
        f.publish("2.1.0")
        keys = update.load_keys(f.root)
        url = update._url(keys, "v2.1.0", "COD_3_WAW_Setup-tool/c3w/gui.py")
        plan, _ = update.check(f.root, f.get)
        # Same size, other bytes: the SHA-256 doesn't match. Longer: refused before it's all read.
        for evil, why in ((b"evil gu\n", "doesn't match"), (b"evil gui, longer\n", "couldn't be downloaded")):
            f.urls[url] = evil
            with self.assertRaisesRegex(update.UpdateError, why):
                update.download(plan, f.root, f.get)
            self.assertEqual(update.pending_version(), "")
            self.assertFalse((f.config / "update/pending").exists())
            self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"old gui\n")

    def test_old_and_replayed_releases(self):
        f = self.f
        f.publish("2.0.0")
        plan, message = update.check(f.root, f.get)
        self.assertIsNone(plan)
        f.publish("1.9.0")
        plan, message = update.check(f.root, f.get)
        self.assertIsNone(plan)
        # Seen 2.5.0, then offered 2.1.0 (older than one seen): refused.
        f.publish("2.5.0")
        update.check(f.root, f.get)
        f.publish("2.1.0")
        with self.assertRaisesRegex(update.UpdateError, "older than a release already seen"):
            update.check(f.root, f.get)

    def test_key_change_needs_the_recovery_key(self):
        f = self.f
        write(f.server_tree, update.KEYS_PATH, keys_file(signing=OTHER))
        f.publish("2.1.0", secret=SIGNING)
        with self.assertRaisesRegex(update.UpdateError, "recovery key"):
            update.check(f.root, f.get)
        f.publish("2.1.0", secret=RECOVERY)
        plan, _ = update.check(f.root, f.get)
        self.assertTrue(plan.release.by_recovery)
        update.download(plan, f.root, f.get)
        self.assertIn("updated", update.apply_pending(f.root))
        self.assertEqual(update.load_keys(f.root).signing, [ed25519.public_key(OTHER)])

    def test_pending_update_is_checked_again(self):
        f = self.f
        write(f.server_tree, "COD_3_WAW_Setup-tool/c3w/gui.py", b"new gui\n")
        f.publish("2.1.0")
        plan, _ = update.check(f.root, f.get)
        pending = update.download(plan, f.root, f.get)
        (pending / "files/COD_3_WAW_Setup-tool/c3w/gui.py").write_bytes(b"changed after download\n")
        text = update.apply_pending(f.root)
        self.assertIn("wasn't used", text)
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"old gui\n")

    def test_link_on_the_way_is_refused(self):
        if os.name == "nt":
            self.skipTest("symlinks need rights on Windows")
        f = self.f
        outside = f.tmp / "outside"
        outside.mkdir()
        shutil.rmtree(f.root / "docs")
        os.symlink(outside, f.root / "docs")
        write(f.server_tree, "docs/README.md", b"docs, new\n")
        f.publish("2.1.0")
        plan, _ = update.check(f.root, f.get)
        update.download(plan, f.root, f.get)
        self.assertIn("wasn't used", update.apply_pending(f.root))
        self.assertEqual(list(outside.iterdir()), [])

    def test_bad_manifests(self):
        keys = update.parse_keys(keys_file())

        def sign_doc(doc) -> tuple:
            manifest = json.dumps(doc).encode()
            return manifest, update.signature_file(ed25519.public_key(SIGNING),
                                                   ed25519.sign(SIGNING, update.SIGNED_PREFIX + manifest))
        good = {"format": 1, "product": update.PRODUCT, "version": "2.1.0", "ref": "v2.1.0", "date": "", "notes": "",
                "files": {"a/b.txt": {"sha256": "0" * 64, "size": 1}}}
        update.verify_release(*sign_doc(good), keys)
        noisy = update.verify_release(*sign_doc({**good, "notes": "a\x1b[31mred\x07\nb"}), keys)
        self.assertEqual(noisy.notes, "a[31mred\nb")
        for change in ({"product": "else"}, {"version": "2.1"}, {"ref": "main; rm -rf"},
                       {"files": {"../x": {"sha256": "0" * 64, "size": 1}}},
                       {"files": {"a/B": {"sha256": "0" * 64, "size": 1}, "a/b": {"sha256": "0" * 64, "size": 1}}},
                       {"files": {"a": {"sha256": "xyz", "size": 1}}}, {"files": {"a": {"sha256": "0" * 64, "size": -1}}},
                       {"files": {"a": {"sha256": "0" * 64, "size": 1, "eol": "cr"}}},
                       {"files": {"a": {"sha256": "0" * 64, "size": 1, "exec": "yes"}}},
                       {"files": {}}):
            with self.assertRaises(update.UpdateError, msg=str(change)):
                update.verify_release(*sign_doc({**good, **change}), keys)
        # Signed for something else (no prefix): refused.
        manifest = json.dumps(good).encode()
        sig = update.signature_file(ed25519.public_key(SIGNING), ed25519.sign(SIGNING, manifest))
        with self.assertRaisesRegex(update.UpdateError, "signature"):
            update.verify_release(manifest, sig, keys)

    def test_not_a_release_copy(self):
        f = self.f
        (f.root / update.MANIFEST_PATH).unlink()
        plan, message = update.check(f.root, f.get)
        self.assertIsNone(plan)
        self.assertIn("isn't a signed release", message)
        self.assertEqual(f.fetched, [])   # nothing was fetched
        write(f.root, update.KEYS_PATH, json.dumps({"format": 1, "source": {"repo": "", "prefix": ""},
                                                    "signing": [], "recovery": []}).encode())
        plan, message = update.check(f.root, f.get)
        self.assertIn("no release key", message)

    def test_git_checkout_is_left_to_git(self):
        f = self.f
        (f.tmp / ".git").mkdir()   # the project folder is inside a clone
        plan, message = update.check(f.root, f.get)
        self.assertIsNone(plan)
        self.assertIn("git checkout", message)
        self.assertEqual(f.fetched, [])

    def test_more_paths(self):
        for bad in ("con .txt", "docs/CONIN$", "COM0.txt", "COD_3_~1/c3w/update_keys.json", "Config/updates.json",
                    "OUTPUT/x", ".GIT/hooks/x", "a/.Git/x", "a/__PyCache__/b", "X.PYC", "docs/caf\u00e9.md",
                    "docs/COM\u00b9.txt", "cod_3_waw_setup-tool/c3w/update_keys.json",
                    "COD_3_WAW_Setup-tool/RELEASE.json", " a", "a/ b"):
            self.assertFalse(update.safe_path(bad), bad)
        for good in ("docs/a~b.md", "engine/src/my_file-2.rs", "docs/con-notes.md", "docs/console.md",
                     update.KEYS_PATH):
            self.assertTrue(update.safe_path(good), good)

    def test_only_https_from_github(self):
        h = update._HttpsOnlyRedirects()
        for url in ("https://evil.example/x", "http://raw.githubusercontent.com/x", "file:///etc/passwd",
                    "https://raw.githubusercontent.com.evil.example/x"):
            with self.assertRaises(update.UpdateError, msg=url):
                h.redirect_request(None, None, 302, "Found", {}, url)
            with self.assertRaises(update.UpdateError, msg=url):
                update.fetch(url, 10)   # refused before anything is sent

    def test_recovery_releases_cant_be_played_back(self):
        f = self.f
        f.publish("2.3.0", secret=RECOVERY)
        update.check(f.root, f.get)
        f.publish("2.2.0", secret=RECOVERY)   # an older recovery-signed release, played back
        with self.assertRaisesRegex(update.UpdateError, "older than a release already seen"):
            update.check(f.root, f.get)
        # A made-up high version (as from a stolen signing key) was seen: the
        # signing key's real releases are refused, the recovery key's get through.
        f.publish("9.0.0")
        update.check(f.root, f.get)
        f.publish("2.4.0")
        with self.assertRaisesRegex(update.UpdateError, "older than a release already seen"):
            update.check(f.root, f.get)
        # The owner moves to a new signing key with the recovery key's release.
        write(f.server_tree, update.KEYS_PATH, keys_file(signing=OTHER))
        f.publish("2.4.0", secret=RECOVERY)
        plan, _ = update.check(f.root, f.get)
        update.download(plan, f.root, f.get)
        self.assertIn("updated from 2.0.0 to 2.4.0", update.apply_pending(f.root))
        self.assertEqual(update.read_state()["highest_seen"], "2.4.0")   # the made-up 9.0.0 no longer counts
        f.publish("2.5.0", secret=OTHER)
        plan, _ = update.check(f.root, f.get)
        self.assertEqual(plan.release.version_text, "2.5.0")

    def test_recovery_release_without_new_keys_keeps_the_newest_seen(self):
        f = self.f
        f.publish("3.5.0")
        update.check(f.root, f.get)          # seen 3.5.0, not installed
        f.publish("3.1.0", secret=RECOVERY)  # an old recovery-signed release, played back
        plan, _ = update.check(f.root, f.get)
        update.download(plan, f.root, f.get)
        update.apply_pending(f.root)
        self.assertEqual(update.read_state()["highest_seen"], "3.5.0")
        f.publish("3.2.0")
        with self.assertRaisesRegex(update.UpdateError, "older than a release already seen"):
            update.check(f.root, f.get)

    def test_letter_case_rename(self):
        f = self.f
        one_file = (f.root / "docs/readme.md").exists()   # this file system ignores letter case
        (f.server_tree / "docs/README.md").rename(f.server_tree / "docs/ReadMe.md")
        f.publish("2.1.0")
        plan, _ = update.check(f.root, f.get)
        self.assertEqual(plan.remove, [] if one_file else ["docs/README.md"])
        # Where both names are one file, the old name is never removed (that would delete the new file).
        saved = update._same_file
        update._same_file = lambda a, b: a.parent == b.parent and a.name.casefold() == b.name.casefold()
        try:
            self.assertEqual(update.make_plan(f.root, plan.installed, plan.release).remove, [])
        finally:
            update._same_file = saved
        update.download(plan, f.root, f.get)
        self.assertIn("updated", update.apply_pending(f.root))
        self.assertIn("ReadMe.md", os.listdir(f.root / "docs"))
        self.assertNotIn("README.md", os.listdir(f.root / "docs"))
        update.undo_last(f.root)
        self.assertEqual(sorted(os.listdir(f.root / "docs")), ["README.md"])

    def test_names_on_disk(self):
        f = self.f
        write(f.root, "Docs2/Sub/File.txt", b"x")
        self.assertEqual(update.disk_name(f.root, "Docs2/Sub/File.txt"), "Docs2/Sub/File.txt")
        one_file = (f.root / "docs2/sub/file.txt").exists()   # this file system ignores letter case
        self.assertEqual(update.disk_name(f.root, "docs2/sub/file.txt"), "Docs2/Sub/File.txt" if one_file else None)
        self.assertIsNone(update.disk_name(f.root, "docs2/sub/other.txt"))
        update._fix_case(f.root, "docs2/sub/file.txt")   # the release's letter case, folders too
        self.assertEqual(update.disk_name(f.root, "docs2/sub/file.txt"), "docs2/sub/file.txt")
        self.assertEqual((f.root / "docs2/sub/file.txt").read_bytes(), b"x")

    def _crash_on_second_file(self):
        real, calls = update._install, []

        def crash(root, path, data, executable):
            calls.append(path)
            if len(calls) == 2:
                raise KeyboardInterrupt   # the power goes off
            real(root, path, data, executable)
        update._install = crash
        try:
            with self.assertRaises(KeyboardInterrupt):
                update.apply_pending(self.f.root)
        finally:
            update._install = real

    def _two_file_update(self):
        f = self.f
        write(f.server_tree, "COD_3_WAW_Setup-tool/c3w/gui.py", b"new gui\n")
        write(f.server_tree, "docs/README.md", b"docs, new\n")
        f.publish("2.1.0")
        plan, _ = update.check(f.root, f.get)
        update.download(plan, f.root, f.get)

    def test_cut_off_update_is_rolled_back(self):
        f = self.f
        self._two_file_update()
        self._crash_on_second_file()
        self.assertTrue((f.config / "update/applying.json").is_file())
        changed = [(f.root / p).read_bytes() for p in ("COD_3_WAW_Setup-tool/c3w/gui.py", "docs/README.md")]
        self.assertEqual(changed, [b"new gui\n", b"docs\n"])   # half new
        shutil.rmtree(f.config / "update/pending")
        text = update.apply_pending(f.root)
        self.assertIn("cut off halfway", text)
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"old gui\n")
        self.assertEqual((f.root / "docs/README.md").read_bytes(), b"docs\n")
        self.assertEqual(update.installed_release(f.root, update.load_keys(f.root)).version_text, "2.0.0")
        self.assertFalse((f.config / "update/applying.json").exists())
        self.assertEqual(update.can_undo(), "")

    def test_cut_off_update_is_redone(self):
        f = self.f
        self._two_file_update()
        self._crash_on_second_file()
        text = update.apply_pending(f.root)   # the next start: back, then the update again
        self.assertIn("cut off halfway", text)
        self.assertIn("updated from 2.0.0 to 2.1.0", text)
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"new gui\n")
        self.assertEqual((f.root / "docs/README.md").read_bytes(), b"docs, new\n")
        self.assertEqual(update.can_undo(), "2.0.0")
        # An undo cut off halfway is finished at the next start.
        real, calls = update._install, []

        def crash(root, path, data, executable):
            calls.append(path)
            if len(calls) == 2:
                raise KeyboardInterrupt
            real(root, path, data, executable)
        update._install = crash
        try:
            with self.assertRaises(KeyboardInterrupt):
                update.undo_last(f.root)
        finally:
            update._install = real
        self.assertIn("finished now", update.apply_pending(f.root))
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"old gui\n")
        self.assertEqual((f.root / "docs/README.md").read_bytes(), b"docs\n")
        self.assertEqual(update.installed_release(f.root, update.load_keys(f.root)).version_text, "2.0.0")

    def _at_start(self, argv_tail, probe_ok=True):
        f = self.f
        saved = (update.release_root, update._probe, update.subprocess.call)
        calls = []
        update.release_root = lambda: f.root
        update._probe = lambda script: (probe_ok, "" if probe_ok else "ImportError: no module named nothing")
        update.subprocess.call = lambda args, **kw: calls.append(args) or 7
        try:
            script = str(f.root / "COD_3_WAW_Setup-tool/cod3_waw_setup.py")
            return update.at_start(script, [script] + argv_tail), calls
        finally:
            update.release_root, update._probe, update.subprocess.call = saved
            os.environ.pop("C3W_UPDATED", None)

    def test_at_start_puts_it_in_place_and_starts_again(self):
        f = self.f
        self._two_file_update()
        rc, calls = self._at_start(["--launcher"])
        self.assertEqual(rc, 7)
        self.assertEqual(calls[0][-1], "--launcher")
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"new gui\n")
        rc, calls = self._at_start([])   # nothing to do any more
        self.assertIsNone(rc)
        self.assertEqual(calls, [])

    def test_a_release_that_doesnt_load_is_undone(self):
        f = self.f
        self._two_file_update()
        rc, calls = self._at_start([], probe_ok=False)
        self.assertEqual(rc, 7)   # started again, on the earlier files
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"old gui\n")
        self.assertEqual(update.installed_release(f.root, update.load_keys(f.root)).version_text, "2.0.0")

    def test_undo_works_before_the_rest_of_the_tool_loads(self):
        f = self.f
        self._two_file_update()
        update.apply_pending(f.root)
        rc, calls = self._at_start(["update", "--undo"])
        self.assertEqual(rc, 0)
        self.assertEqual(calls, [])
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"old gui\n")

    def test_half_changed_files_are_never_loaded(self):
        f = self.f
        write(f.config, "update/applying.json", json.dumps({"format": 99}).encode())
        rc, calls = self._at_start([])
        self.assertEqual(rc, 1)
        self.assertEqual(calls, [])

    def test_one_copy_at_a_time(self):
        f = self.f
        write(f.server_tree, "COD_3_WAW_Setup-tool/c3w/gui.py", b"new gui\n")
        f.publish("2.1.0")
        plan, _ = update.check(f.root, f.get)
        with update._Lock(wait=0):
            with self.assertRaisesRegex(update.UpdateError, "another copy"):
                update.download(plan, f.root, f.get)
        update.download(plan, f.root, f.get)

    def test_a_missing_kept_copy_stops_the_undo(self):
        f = self.f
        self._two_file_update()
        update.apply_pending(f.root)
        (f.config / "update/previous/files/docs/README.md").unlink()
        with self.assertRaisesRegex(update.UpdateError, "missing"):
            update.undo_last(f.root)
        self.assertTrue((f.config / "update/applying.json").is_file())   # the next start won't load half of it

    def test_undo_drops_a_downloaded_update(self):
        f = self.f
        self._two_file_update()
        update.apply_pending(f.root)
        write(f.server_tree, "docs/README.md", b"docs, newer\n")
        f.publish("2.2.0")
        plan, _ = update.check(f.root, f.get)
        update.download(plan, f.root, f.get)
        update.undo_last(f.root)
        self.assertEqual(update.pending_version(), "")
        self.assertEqual(update.apply_pending(f.root), "")

    def test_undo_only_from_the_version_it_was_made_for(self):
        f = self.f
        self._two_file_update()
        update.apply_pending(f.root)
        m = json.loads((f.root / update.MANIFEST_PATH).read_bytes())
        m["version"] = "2.2.0"
        (f.root / update.MANIFEST_PATH).write_text(json.dumps(m))
        with self.assertRaisesRegex(update.UpdateError, "nothing was undone"):
            update.undo_last(f.root)
        self.assertEqual((f.root / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"new gui\n")


class SigningToolTests(unittest.TestCase):
    """tools/sign_release.py for real, in a made-up GitHub clone with RELEASE/
    inside: keygen, sign and tag; a player's copy made from the tag; the next
    releases (one with a new signing key, signed with the recovery key), each
    downloaded the way GitHub serves it: release.json from the branch, the
    files from the tag. Skipped without git."""

    def setUp(self):
        if not shutil.which("git"):
            self.skipTest("no git")
        import sign_release
        self.sr = sign_release
        self._saved = (update.update_dir, update._state_path, sign_release.SCRYPT_N, sign_release.getpass.getpass,
                       dict(os.environ))
        self._tmp = tempfile.TemporaryDirectory()
        self.t = Path(self._tmp.name)
        if update.git_checkout(self.t):
            self.skipTest("the temporary folder is inside a git checkout")
        sign_release.SCRYPT_N = 1 << 10
        os.environ.update({"GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
                           "GIT_AUTHOR_NAME": "test", "GIT_AUTHOR_EMAIL": "test@example.invalid",
                           "GIT_COMMITTER_NAME": "test", "GIT_COMMITTER_EMAIL": "test@example.invalid",
                           "XDG_CONFIG_HOME": str(self.t / "home"), "APPDATA": str(self.t / "home")})
        answers = {"last": ""}

        def ask(prompt=""):
            if prompt.startswith("The same again"):
                return answers["last"]
            answers["last"] = "recovery passphrase!" if "recovery" in prompt else "signing passphrase!"
            return answers["last"]
        sign_release.getpass.getpass = ask
        self.clone = self.t / "clone"
        self.rel = self.clone / "RELEASE"
        write(self.rel, "setup.sh", b"#!/bin/sh\n")
        write(self.rel, update.KEYS_PATH, json.dumps({"format": 1, "source": {"repo": "", "prefix": ""},
                                                      "signing": [], "recovery": []}).encode())
        write(self.rel, "COD_3_WAW_Setup-tool/c3w/gui.py", b"gui 1\n")
        write(self.rel, "docs/old.md", b"old doc\n")
        # As in the project: git keeps .bat files with LF, a checkout (and a download of the release) has CRLF.
        write(self.rel, ".gitattributes", b"* text=auto eol=lf\n*.bat text eol=crlf\n")
        write(self.rel, "setup.bat", b"@echo off\r\nrem one\r\n")
        write(self.rel, "tools/run.sh", b"#!/bin/sh\necho 1\n")
        os.chmod(self.rel / "tools/run.sh", 0o755)
        self.version("3.0.0", "3.0.0: [claude-x] the first signed release. 2.9.0: [claude-y] older.")
        self.git("init", "-q")
        self.git("remote", "add", "origin", "https://github.com/example/cod3.git")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "start")
        # GitHub stand-in for pushes (the fetch address stays github.com: it names the repository).
        self.remote = self.t / "remote.git"
        subprocess.run(["git", "init", "-q", "--bare", str(self.remote)], check=True, capture_output=True)
        self.git("remote", "set-url", "--push", "origin", str(self.remote))
        self.git("push", "-q", "-u", "origin", "HEAD")
        # The owner ran the setup tool here once: never published, never committed.
        write(self.rel, "config/settings.json", b'{"mine": 1}')
        write(self.rel, "work/big.bin", b"work")
        write(self.clone, "notes-outside-release.txt", b"not part of it")
        self.player = self.t / "player"
        update.update_dir = lambda: self.player / "config" / "update"
        update._state_path = lambda: self.player / "config" / "updates.json"

    def tearDown(self):
        (update.update_dir, update._state_path, self.sr.SCRYPT_N, self.sr.getpass.getpass, env) = self._saved
        os.environ.clear()
        os.environ.update(env)
        self._tmp.cleanup()

    def git(self, *args) -> bytes:
        r = subprocess.run(["git", *args], cwd=self.clone, capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode(errors="replace"))
        return r.stdout

    def version(self, v: str, comment: str) -> None:
        write(self.rel, "COD_3_WAW_Setup-tool/c3w/pipeline.py", f'TOOL_VERSION = "{v}"  # {comment}\n'.encode())

    def sign(self, **kw):
        import argparse
        a = dict(root=str(self.rel), key=None, recovery=False, notes=None, push=False, no_git=False, yes=True)
        a.update(kw)
        return self.sr.cmd_sign(argparse.Namespace(**a))

    def github(self, url: str, limit: int) -> bytes:
        """raw.githubusercontent.com/<repo>/<ref>/<path>, from the clone."""
        head = "https://raw.githubusercontent.com/example/cod3/"
        self.assertTrue(url.startswith(head), url)
        ref, path = url[len(head):].split("/", 1)
        data = self.git("cat-file", "blob", f"{ref}:{path}")   # raw.githubusercontent.com serves git's copy
        if len(data) > limit:
            raise update.UpdateError("too big")
        return data

    def player_update(self, to: str) -> None:
        plan, message = update.check(self.player, self.github)
        self.assertEqual(message, "")
        self.assertEqual(plan.release.version_text, to)
        update.download(plan, self.player, self.github)
        self.assertIn(f"to {to}", update.apply_pending(self.player))

    def test_keygen_sign_and_update_from_the_tag(self):
        import argparse
        import io
        import tarfile
        sr = self.sr
        self.assertEqual(sr.cmd_keygen(argparse.Namespace(root=str(self.rel), dir=None, repo=None, prefix=None)), 0)
        keys = update.load_keys(self.rel)
        self.assertEqual((keys.repo, keys.prefix), ("example/cod3", "RELEASE/"))
        key_folder = self.t / "home" / "cod3-release-keys"
        for name in ("signing-key.json", "recovery-key.json"):
            self.assertTrue((key_folder / name).is_file())
            if os.name != "nt":
                self.assertEqual((key_folder / name).stat().st_mode & 0o077, 0)
        self.assertFalse(list(self.rel.rglob("*-key.json")))   # no private key in the project

        # 3.0.0, pushed, and the player's copy of it (as from a download of the tag).
        self.assertEqual(self.sign(push=True), 0)
        self.assertIn(b"v3.0.0", subprocess.run(["git", "--git-dir", str(self.remote), "tag", "--list"],
                                                 capture_output=True).stdout)
        self.assertTrue(self.git("tag", "--list", "v3.0.0").strip())
        manifest = json.loads((self.rel / update.MANIFEST_PATH).read_bytes())
        self.assertEqual(manifest["notes"], "the first signed release.")
        self.assertNotIn("config/settings.json", manifest["files"])
        self.assertNotIn("work/big.bin", manifest["files"])
        in_git = self.git("ls-files").decode()
        self.assertNotIn("config/settings.json", in_git)
        self.assertNotIn("work/big.bin", in_git)
        self.assertNotIn("notes-outside-release.txt", in_git)
        self.assertIn("RELEASE/COD_3_WAW_Setup-tool/release.json.sig", in_git)
        self.assertEqual(manifest["files"]["setup.bat"].get("eol"), "crlf")
        self.assertEqual(self.git("cat-file", "blob", "v3.0.0:RELEASE/setup.bat"), b"@echo off\nrem one\n")
        self.assertTrue(manifest["files"]["tools/run.sh"].get("exec"))
        self.assertNotIn("exec", manifest["files"]["setup.bat"])
        with self.assertRaises(SystemExit):
            self.sign()   # the same version again
        archive = self.git("archive", "--format=tar", "v3.0.0:RELEASE")
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            try:
                tar.extractall(self.player, filter="data")
            except TypeError:   # an older Python without filters
                tar.extractall(self.player)
        write(self.player, "config/settings.json", b'{"player": 1}')
        self.assertEqual(update.why_not_here(self.player), "")
        plan, message = update.check(self.player, self.github)
        self.assertIsNone(plan)
        self.assertIn("up to date", message)

        # A key file in an unpushed commit (even one deleted again): the push is refused, nothing is left.
        self.version("3.1.0", "3.1.0: leak test.")
        write(self.clone, "leak.json", json.dumps(sr.seal(os.urandom(32), "a long passphrase", "signing")).encode())
        self.git("add", "leak.json")
        self.git("commit", "-q", "-m", "oops")
        self.git("rm", "-q", "leak.json")
        self.git("commit", "-q", "-m", "removed")
        with self.assertRaises(SystemExit):
            self.sign(push=True)
        self.assertFalse(self.git("tag", "--list", "v3.1.0").strip())
        self.assertEqual(self.git("log", "-1", "--format=%s").strip(), b"removed")
        self.git("reset", "-q", "--hard", "@{u}")   # the owner takes the two commits out

        # 3.1.0, then an unsigned commit on the branch: the player gets 3.1.0's files.
        write(self.rel, "COD_3_WAW_Setup-tool/c3w/gui.py", b"gui 2\n")
        write(self.rel, "COD_3_WAW_Setup-tool/c3w/added.py", b"added\n")
        write(self.rel, "setup.bat", b"@echo off\r\nrem two\r\n")
        write(self.rel, "tools/run.sh", b"#!/bin/sh\necho 2\n")
        (self.rel / "docs/old.md").unlink()
        self.version("3.1.0", "3.1.0: [claude-x] second. 3.0.0: the first signed release.")
        saved = sr._confirm
        sr._confirm = lambda question: False   # "Publish them?" (added.py is new): no
        try:
            with self.assertRaises(SystemExit):
                self.sign(yes=False)
        finally:
            sr._confirm = saved
        self.assertFalse(self.git("tag", "--list", "v3.1.0").strip())
        self.assertEqual(self.sign(), 0)
        write(self.rel, "COD_3_WAW_Setup-tool/c3w/gui.py", b"gui, not released\n")
        self.git("commit", "-q", "-am", "work after the release")
        self.player_update("3.1.0")
        self.assertEqual((self.player / "COD_3_WAW_Setup-tool/c3w/gui.py").read_bytes(), b"gui 2\n")
        self.assertTrue((self.player / "COD_3_WAW_Setup-tool/c3w/added.py").is_file())
        self.assertFalse((self.player / "docs/old.md").exists())
        self.assertEqual((self.player / "config/settings.json").read_bytes(), b'{"player": 1}')
        self.assertEqual((self.player / "setup.bat").read_bytes(), b"@echo off\r\nrem two\r\n")   # CRLF kept
        if os.name != "nt":
            self.assertTrue(os.stat(self.player / "tools/run.sh").st_mode & 0o100)

        # A key file or game data in the folder: not signed.
        self.version("3.2.0", "3.2.0: a new signing key.")
        sealed = json.dumps(sr.seal(os.urandom(32), "a long passphrase", "signing"), indent=2).encode()
        for bad, data in (("tools/signing-key.json", b"{}"), ("docs/disc.iso", b"disc"), ("docs/notes.txt", sealed)):
            write(self.rel, bad, data)
            with self.assertRaises(SystemExit):
                self.sign()
            (self.rel / bad).unlink()
        self.assertFalse(self.git("tag", "--list", "v3.2.0").strip())

        # 3.2.0 puts in a new signing key: the tool signs it with the recovery key by itself.
        new_secret = os.urandom(32)
        sr._write_private(key_folder / "new-signing-key.json", sr.seal(new_secret, "signing passphrase!", "signing"))
        k = json.loads((self.rel / update.KEYS_PATH).read_bytes())
        k["signing"] = [ed25519.public_key(new_secret).hex()]
        (self.rel / update.KEYS_PATH).write_text(json.dumps(k, indent=2) + "\n", encoding="utf-8")
        self.version("3.2.0", "3.2.0: a new signing key.")
        self.assertEqual(self.sign(), 0)
        self.assertEqual(json.loads((self.rel / update.SIGNATURE_PATH).read_bytes())["key"],
                         update.load_keys(self.rel).recovery[0].hex())
        self.player_update("3.2.0")
        # 3.3.0 with the new key; the old key isn't trusted any more.
        self.version("3.3.0", "3.3.0: signed with the new key.")
        with self.assertRaises(SystemExit):
            self.sign()   # signing-key.json is the old one
        self.assertEqual(self.sign(key=str(key_folder / "new-signing-key.json")), 0)
        self.player_update("3.3.0")
        self.assertEqual(self.sr.cmd_verify(argparse.Namespace(root=str(self.player), quiet=True)), 0)


class KeyFileTests(unittest.TestCase):
    def test_sealed_key(self):
        import sign_release
        sign_release.SCRYPT_N = 1 << 10   # fast for the test
        sealed = sign_release.seal(SIGNING, "a long passphrase", "signing")
        self.assertNotIn(SIGNING.hex(), json.dumps(sealed))
        self.assertEqual(sign_release.unseal(sealed, "a long passphrase"), SIGNING)
        with self.assertRaises(SystemExit):
            sign_release.unseal(sealed, "a wrong passphrase")
        tampered = dict(sealed, public_key=ed25519.public_key(OTHER).hex())
        with self.assertRaises(SystemExit):
            sign_release.unseal(tampered, "a long passphrase")

    def test_notes_from_tool_version(self):
        import sign_release
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "COD_3_WAW_Setup-tool/c3w/pipeline.py",
                  b'TOOL_VERSION = "2.27.0"  # 2.27.0: [claude-5d] the updater. 2.26.0: [x] older.\n')
            self.assertEqual(sign_release._tool_version(root), ("2.27.0", "the updater."))


if __name__ == "__main__":
    unittest.main()

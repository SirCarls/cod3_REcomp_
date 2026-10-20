"""The SDK comes from the project's fork (patches/fork.json) with the official
commit plus the patch files as the backup, and both routes give the same
tree. These tests build a tiny "official" repository, a patch file, and a
"fork" holding that patch as a commit, all local, and check which route a
checkout takes and what it ends up with. They need git; no network.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import common, pipeline  # noqa: E402

GIT = shutil.which("git")
SHADERS = "s" * 40   # stands in for sdk_shaders.version()


def git(*args, cwd, out=False):
    r = subprocess.run([GIT, "-c", "protocol.file.allow=always", *args], cwd=cwd, check=True, text=True,
                       stdout=subprocess.PIPE if out else subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return (r.stdout or "").strip()


PATCH_A = """diff --git a/a.txt b/a.txt
index 7898192..4ac7d3c 100644
--- a/a.txt
+++ b/a.txt
@@ -1 +1 @@
-a
+a, patched
"""
PATCH_B = """diff --git a/b.txt b/b.txt
new file mode 100644
index 0000000..ffcf7d0
--- /dev/null
+++ b/b.txt
@@ -0,0 +1 @@
+b, from a patch file the fork doesn't hold yet
"""


class PlanTests(unittest.TestCase):
    """plan_sdk_source alone: which route, and the build key."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.patches = self.tmp / "patches"
        self.patches.mkdir()
        (self.patches / "0001-a.patch").write_text(PATCH_A, newline="\n")
        self.commit = "f" * 40

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_fork(self, **over):
        data = {"repo": "https://github.com/someone/sdk-fork.git", "branch": "cod3", "commit": self.commit,
                "base": pipeline.SDK_COMMIT, "shaders": SHADERS,
                "patches": {"0001-a.patch": pipeline.patch_hash(self.patches / "0001-a.patch")}}
        data.update(over)
        (self.patches / pipeline.FORK_FILE).write_text(json.dumps(data))

    def plan(self, cross=False):
        return pipeline.plan_sdk_source(self.patches, cross, SHADERS)

    def test_no_fork_file_means_the_official_route(self):
        src = self.plan()
        self.assertIsNone(src.fork_commit)
        self.assertIn("fork.json", src.why_not_fork)
        self.assertEqual(src.key["commit"], pipeline.SDK_COMMIT)

    def test_fork_route_with_the_newer_patch_files_on_top(self):
        self.write_fork()
        (self.patches / "0002-b.patch").write_text(PATCH_B, newline="\n")
        src = self.plan()
        self.assertEqual(src.fork_commit, self.commit)
        self.assertEqual([p.name for p in src.fork_patches], ["0002-b.patch"])
        self.assertEqual([p.name for p in src.official_patches], ["0001-a.patch", "0002-b.patch"])
        self.assertEqual(src.key, {"fork": self.commit, "patches": [pipeline.patch_hash(self.patches / "0002-b.patch")]})

    def test_a_changed_patch_file_means_the_official_route(self):
        self.write_fork()
        (self.patches / "0001-a.patch").write_text(PATCH_A.replace("patched", "patched again"), newline="\n")
        src = self.plan()
        self.assertIsNone(src.fork_commit)
        self.assertIn("0001-a.patch changed", src.why_not_fork)
        self.assertIn("commit", src.key)

    def test_a_removed_patch_file_means_the_official_route(self):
        self.write_fork()
        (self.patches / "0001-a.patch").unlink()
        self.assertIn("gone", self.plan().why_not_fork)

    def test_new_shader_edits_mean_the_official_route(self):
        self.write_fork(shaders="t" * 40)
        self.assertIn("sdk_shaders", self.plan().why_not_fork)

    def test_another_base_means_the_official_route(self):
        self.write_fork(base="0" * 40)
        self.assertIn("another official commit", self.plan().why_not_fork)

    def test_crlf_copy_of_a_held_patch_still_counts(self):
        self.write_fork()
        p = self.patches / "0001-a.patch"
        p.write_bytes(p.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(self.plan().fork_commit, self.commit)

    def test_windows_patches_go_on_top_of_the_fork(self):
        self.write_fork()
        (self.patches / "mingw").mkdir()
        (self.patches / "mingw" / "0005-w.patch").write_text(PATCH_B, newline="\n")
        self.assertEqual(self.plan().fork_patches, [])
        self.assertEqual([p.name for p in self.plan(cross=True).fork_patches], ["0005-w.patch"])

    def test_suspicious_fork_files_are_ignored(self):
        for bad in ({"repo": "https://example.com/x.git"}, {"repo": "file:///tmp/x"}, {"commit": "abc"},
                    {"patches": {"../../evil.patch": "0" * 40}}, {"patches": {"0001-a.patch": "zz"}},
                    {"patches": []}):
            with self.subTest(bad=bad):
                self.write_fork(**bad)
                self.assertIsNone(pipeline.read_fork_file(self.patches))
                self.assertIsNone(self.plan().fork_commit)


@unittest.skipUnless(GIT, "git is not installed")
class CheckoutTests(unittest.TestCase):
    """_checkout_sdk on real (local) repositories."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        ident = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
                 "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid"}
        self.env = dict(os.environ, **ident, GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="protocol.file.allow",
                        GIT_CONFIG_VALUE_0="always")
        self._old_env = dict(os.environ)
        os.environ.update(ident)
        # The official SDK.
        self.official = self.tmp / "official"
        self.official.mkdir()
        git("init", "-q", cwd=self.official)
        (self.official / "a.txt").write_text("a\n", newline="\n")
        git("add", ".", cwd=self.official)
        git("commit", "-q", "-m", "official", cwd=self.official)
        self.official_commit = git("rev-parse", "HEAD", cwd=self.official, out=True)
        # The patch files.
        self.patches = self.tmp / "tool" / "patches"
        self.patches.mkdir(parents=True)
        (self.patches / "0001-a.patch").write_text(PATCH_A, newline="\n")
        # The fork: the official commit + 0001 as a commit.
        self.fork = self.tmp / "fork"
        git("clone", "-q", str(self.official), str(self.fork), cwd=self.tmp)
        git("apply", str(self.patches / "0001-a.patch"), cwd=self.fork)
        git("commit", "-q", "-am", "cod3 0001: a", cwd=self.fork)
        self.fork_commit = git("rev-parse", "HEAD", cwd=self.fork, out=True)
        self.work = self.tmp / "work"
        self.work.mkdir()
        self.mocks = [mock.patch.object(pipeline, "SDK_COMMIT", self.official_commit),
                      mock.patch.object(pipeline, "SDK_REPO", self.official.as_uri()),
                      mock.patch.object(pipeline, "_GITHUB_REPO_URL", re.compile(r"^file://")),
                      mock.patch.object(pipeline.sdk_shaders, "edit_sdk", return_value=0),
                      mock.patch.object(pipeline.sdk_shaders, "version", return_value=SHADERS)]
        for m in self.mocks:
            m.start()
        common.APPROVAL.reset()
        self._old_ask = common.APPROVAL.ask
        common.APPROVAL.ask = lambda items: True

    def tearDown(self):
        for m in self.mocks:
            m.stop()
        common.APPROVAL.ask = self._old_ask
        common.APPROVAL.reset()
        os.environ.clear()
        os.environ.update(self._old_env)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_fork(self, repo_uri):
        data = {"repo": repo_uri, "branch": "cod3", "commit": self.fork_commit, "base": self.official_commit,
                "shaders": SHADERS, "patches": {"0001-a.patch": pipeline.patch_hash(self.patches / "0001-a.patch")}}
        (self.patches / pipeline.FORK_FILE).write_text(json.dumps(data))

    def fake_pipeline(self):
        log = common.Log()
        runner = common.Runner(log, threading.Event())
        p = SimpleNamespace(log=log, runner=runner, sdk_src=self.work / "rexglue-sdk", work=self.work,
                            work_real=self.work, protected=[], patches=self.patches,
                            tc=SimpleNamespace(git=Path(GIT), env=self.env, cxx=None),
                            native=True, cross_windows=False, _sdk_built=lambda cxx: False,
                            FORK_REMOTE=pipeline.Pipeline.FORK_REMOTE,
                            _SUBMODULE_ERROR=pipeline.Pipeline._SUBMODULE_ERROR)
        for name in ("_git", "_git_config", "_repair_submodules", "_fetch_submodules", "_clean_checkout",
                     "_apply_patches", "_sdk_source", "_sdk_has_commit", "_fetch_fork", "_checkout_sdk",
                     "_sdk_install", "_ask_sdk_download_early"):
            setattr(p, name, getattr(pipeline.Pipeline, name).__get__(p))
        return p

    def tree(self, p):
        return {f.name: f.read_text() for f in sorted(p.sdk_src.glob("*.txt"))}

    def test_fork_route(self):
        self.write_fork(self.fork.as_uri())
        (self.patches / "0002-b.patch").write_text(PATCH_B, newline="\n")
        p = self.fake_pipeline()
        self.assertEqual(p._checkout_sdk(p._sdk_source()), self.fork_commit)
        self.assertEqual(self.tree(p), {"a.txt": "a, patched\n",
                                        "b.txt": "b, from a patch file the fork doesn't hold yet\n"})

    def test_fork_unreachable_falls_back_to_the_same_tree(self):
        self.write_fork((self.tmp / "no-such-fork").as_uri())
        (self.patches / "0002-b.patch").write_text(PATCH_B, newline="\n")
        p = self.fake_pipeline()
        self.assertEqual(p._checkout_sdk(p._sdk_source()), self.official_commit)
        self.assertEqual(self.tree(p), {"a.txt": "a, patched\n",
                                        "b.txt": "b, from a patch file the fork doesn't hold yet\n"})

    def test_both_routes_have_one_build_key(self):
        self.write_fork(self.fork.as_uri())
        p = self.fake_pipeline()
        key = p._sdk_source().key
        p._checkout_sdk(p._sdk_source())
        self.write_fork((self.tmp / "no-such-fork").as_uri())
        self.assertEqual(p._sdk_source().key, key)

    def test_switching_route_in_the_same_checkout(self):
        # A work folder that was built the old way (official + patches) moves to the fork, and back.
        p = self.fake_pipeline()
        self.assertEqual(p._checkout_sdk(p._sdk_source()), self.official_commit)   # no fork.json yet
        self.write_fork(self.fork.as_uri())
        self.assertEqual(p._checkout_sdk(p._sdk_source()), self.fork_commit)
        (self.patches / "0001-a.patch").write_text(PATCH_A.replace("patched", "patched again"), newline="\n")
        self.assertEqual(p._checkout_sdk(p._sdk_source()), self.official_commit)
        self.assertEqual(self.tree(p), {"a.txt": "a, patched again\n"})

    def test_declined_download_uses_the_official_commit_already_there(self):
        p = self.fake_pipeline()
        p._checkout_sdk(p._sdk_source())          # the official commit is now in the checkout
        self.write_fork(self.fork.as_uri())
        common.APPROVAL.reset()
        common.APPROVAL.ask = lambda items: False
        self.assertEqual(p._checkout_sdk(p._sdk_source()), self.official_commit)
        self.assertEqual(self.tree(p), {"a.txt": "a, patched\n"})

    def test_declined_download_with_nothing_there_stops(self):
        self.write_fork(self.fork.as_uri())
        common.APPROVAL.ask = lambda items: False
        p = self.fake_pipeline()
        with self.assertRaises(common.SetupError):
            p._checkout_sdk(p._sdk_source())


    def counting_ask(self, answer):
        asked = []
        common.APPROVAL.reset()
        common.APPROVAL.ask = lambda items: asked.append([i.key for i in items]) or answer
        return asked

    def test_fork_download_asked_at_the_start_and_only_once(self):
        p = self.fake_pipeline()
        p._checkout_sdk(p._sdk_source())          # an older work folder: the official commit only
        self.write_fork(self.fork.as_uri())
        asked = self.counting_ask(True)
        p._ask_sdk_download_early()
        self.assertEqual(asked, [["git:rexglue-sdk"]])
        self.assertEqual(p._checkout_sdk(p._sdk_source()), self.fork_commit)
        self.assertEqual(len(asked), 1)

    def test_no_at_the_start_means_the_official_sdk_without_asking_again(self):
        p = self.fake_pipeline()
        p._checkout_sdk(p._sdk_source())
        self.write_fork(self.fork.as_uri())
        asked = self.counting_ask(False)
        p._ask_sdk_download_early()
        self.assertEqual(p._checkout_sdk(p._sdk_source()), self.official_commit)
        self.assertEqual(len(asked), 1)
        self.assertEqual(self.tree(p), {"a.txt": "a, patched\n"})

    def test_nothing_asked_when_the_fork_commit_is_there_or_the_sdk_is_built(self):
        self.write_fork(self.fork.as_uri())
        p = self.fake_pipeline()
        p._checkout_sdk(p._sdk_source())          # fetches the fork commit
        asked = self.counting_ask(True)
        p._ask_sdk_download_early()
        self.assertEqual(asked, [])
        q = self.fake_pipeline()
        q.sdk_src = self.work / "other"
        (q.sdk_src / ".git").mkdir(parents=True)
        q._sdk_built = lambda cxx: True
        q._ask_sdk_download_early()
        self.assertEqual(asked, [])


if __name__ == "__main__":
    unittest.main()

"""The SDK's git submodules are downloaded with repair: a clone that was cut
off leaves a broken copy in .git/modules, after which git only says "could
not get a repository handle for submodule" (seen on a Windows build,
2026-10-09). These tests break a submodule that way in a small local repo and
check the download recovers. They need git; no network.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import common, pipeline  # noqa: E402

GIT = shutil.which("git")


def git(*args, cwd):
    subprocess.run([GIT, "-c", "protocol.file.allow=always", *args], cwd=cwd, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


@unittest.skipUnless(GIT, "git is not installed")
class SubmoduleRepairTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        ident = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
                 "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid"}
        self.env = dict(os.environ, **ident, GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="protocol.file.allow",
                        GIT_CONFIG_VALUE_0="always")
        os.environ.update(ident)
        lib = self.tmp / "lib"
        lib.mkdir()
        git("init", "-q", cwd=lib)
        (lib / "a.txt").write_text("a")
        git("add", ".", cwd=lib)
        git("commit", "-q", "-m", "lib", cwd=lib)
        self.sdk = self.tmp / "work" / "sdk"
        self.sdk.mkdir(parents=True)
        git("init", "-q", cwd=self.sdk)
        git("submodule", "add", "-q", lib.as_uri(), "thirdparty/lib", cwd=self.sdk)
        git("commit", "-q", "-m", "sdk", cwd=self.sdk)
        # A fresh checkout of the superproject, like the setup tool's.
        self.clone = self.tmp / "work" / "clone"
        git("clone", "-q", str(self.sdk), str(self.clone), cwd=self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fake_pipeline(self):
        log = common.Log()
        runner = common.Runner(log, threading.Event())
        p = SimpleNamespace(log=log, runner=runner, sdk_src=self.clone, protected=[],
                            tc=SimpleNamespace(git=Path(GIT), env=self.env))
        for name in ("_git", "_git_config", "_repair_submodules", "_fetch_submodules"):
            setattr(p, name, getattr(pipeline.Pipeline, name).__get__(p))
        p._SUBMODULE_ERROR = pipeline.Pipeline._SUBMODULE_ERROR
        return p

    def test_fresh_download(self):
        self.fake_pipeline()._fetch_submodules()
        self.assertTrue((self.clone / "thirdparty" / "lib" / "a.txt").is_file())

    def test_repairs_a_clone_that_was_cut_off(self):
        # What an interrupted clone leaves: a module folder that isn't a repo.
        broken = self.clone / ".git" / "modules" / "thirdparty" / "lib"
        broken.mkdir(parents=True)
        (broken / "HEAD").write_text("garbage")
        (self.clone / "thirdparty" / "lib").mkdir(parents=True, exist_ok=True)
        (self.clone / "thirdparty" / "lib" / ".git").write_text("gitdir: ../../.git/modules/thirdparty/lib\n")
        self.fake_pipeline()._fetch_submodules()
        self.assertTrue((self.clone / "thirdparty" / "lib" / "a.txt").is_file())

    def test_repair_stays_inside_the_checkout(self):
        p = self.fake_pipeline()
        outside = self.tmp / "keep"
        outside.mkdir()
        self.assertEqual(p._repair_submodules([str(outside), "../keep", str(self.clone)]), [])
        self.assertTrue(outside.is_dir())
        self.assertTrue(self.clone.is_dir())


if __name__ == "__main__":
    unittest.main()

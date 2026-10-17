"""Builds and runs tests/voice_paths_test.cpp: which sound banks the game
registers for the chosen voices (recomp/src/game/voice_paths.h). Skipped
when there is no C++ compiler.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent.parent
TEST = TOOL / "tests" / "voice_paths_test.cpp"


def compiler():
    for name in (os.environ.get("CXX"), "c++", "g++", "clang++"):
        if name and shutil.which(name):
            return shutil.which(name)
    return None


class VoicePathsTest(unittest.TestCase):
    def test_plan(self):
        cxx = compiler()
        if not cxx:
            self.skipTest("no C++ compiler")
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / ("voice_paths_test.exe" if os.name == "nt" else "voice_paths_test")
            build = subprocess.run([cxx, "-std=c++17", "-I", str(TOOL / "recomp" / "src"), str(TEST), "-o", str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()

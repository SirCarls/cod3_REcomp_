"""[waw-decomp] 2026-11-16 Builds and runs tests/command_guard_rules_test.cpp: which map names and which exec'd
files cod3mp lets reach the game's command buffer (recomp/src/game/mp/command_guard_rules.h). Skipped when there is
no C++ compiler.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent.parent
TEST = TOOL / "tests" / "command_guard_rules_test.cpp"


def compiler():
    for name in (os.environ.get("CXX"), "c++", "g++", "clang++"):
        if name and shutil.which(name):
            return shutil.which(name)
    return None


class CommandGuardRulesTest(unittest.TestCase):
    def test_rules(self):
        cxx = compiler()
        if not cxx:
            self.skipTest("no C++ compiler")
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / ("command_guard_rules_test.exe" if os.name == "nt" else "command_guard_rules_test")
            build = subprocess.run([cxx, "-std=c++17", "-I", str(TOOL / "recomp" / "src"), str(TEST), "-o", str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()

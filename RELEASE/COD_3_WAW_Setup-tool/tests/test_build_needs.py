"""[acct2-decomp] Tests for c3w/build_needs.py (2026-11-05, owner: "refresh the dependency checks and options
offered"): the Set up page's line and "Check this PC" say what a build with the chosen options needs, by the
build's own rules. Nothing is installed or detected for real: the toolchain and the plan are stand-ins.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import build_needs, pipeline  # noqa: E402
from c3w.common import Install  # noqa: E402
from c3w.pipeline import BuildOptions  # noqa: E402


class FakeToolchain:
    def __init__(self, problems, rust_problems=()):
        self._problems, self._rust = list(problems), list(rust_problems)

    def problems(self, recomp=True):
        return list(self._rust) + (list(self._problems) if recomp else [])

    def rust_problems(self):
        return list(self._rust)


class LineTests(unittest.TestCase):
    def test_short_names(self):
        self.assertEqual(build_needs.short("CMake 3.31.6 (build tool)"), "CMake 3.31.6")
        self.assertEqual(build_needs.short("Rust (compiles the ripper), with rustup"), "Rust")

    def test_the_line(self):
        self.assertEqual(build_needs.Needs().line(), "Everything this build needs is on this PC.")
        line = build_needs.Needs(installs=["CMake 3.31.6", "Ninja 1.12.1"]).line()
        self.assertTrue(line.startswith("Build first downloads or installs (you approve the list): CMake 3.31.6 "
                                        "and Ninja 1.12.1."), line)
        line = build_needs.Needs(problems=["git", "ninja"], install_deps=False).line()
        self.assertIn("Missing for this build: git and ninja.", line)
        self.assertIn("Install anything that's missing", line)
        line = build_needs.Needs(problems=["Visual Studio 2022 Build Tools"]).line()
        self.assertIn("Check this PC says how", line)


class CheckTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.opts = dict(output_dir=str(root / "out"), work_dir=str(root / "work"), cod3_source="")

    def tearDown(self):
        self._tmp.cleanup()

    def check(self, tc, plan=(), needs_rust=True, linux=True, **opts):
        with mock.patch.object(pipeline, "IS_LINUX", linux), mock.patch.object(pipeline, "IS_WINDOWS", not linux), \
                mock.patch.object(build_needs, "IS_LINUX", linux), \
                mock.patch.object(pipeline.Pipeline, "_needs_rust", new_callable=mock.PropertyMock,
                                  return_value=needs_rust), \
                mock.patch.object(pipeline.Pipeline, "install_plan", return_value=list(plan)) as ip:
            needs = build_needs.check(BuildOptions(**self.opts, **opts), tc=tc)
        return needs, ip

    def test_rust_isnt_asked_for_when_the_build_needs_none(self):
        tc = FakeToolchain(["cmake >= 3.25"], rust_problems=["Rust >= 1.85 (for the ripper)"])
        needs, _ = self.check(tc, needs_rust=False)
        self.assertEqual(needs.problems, ["cmake >= 3.25"])
        self.assertFalse(needs.rust)
        needs, _ = self.check(tc, needs_rust=True)
        self.assertIn("Rust >= 1.85 (for the ripper)", needs.problems)

    def test_the_recomp_follows_the_versions_ticked(self):
        tc = FakeToolchain(["ninja"])
        needs, ip = self.check(tc, build_linux=True, build_windows=False)
        self.assertTrue(needs.recomp)
        ip.assert_called_once_with(True)
        needs, ip = self.check(tc, build_linux=False, build_windows=False)
        self.assertFalse(needs.recomp)
        self.assertEqual(needs.problems, [])           # nothing compiled: the C++ tools aren't needed

    def test_the_plan_becomes_the_install_list(self):
        plan = [Install("cmake-linux", "CMake 3.31.6 (build tool)", "/w/tools/cmake-linux"),
                Install("ninja-linux", "Ninja 1.12.1 (build tool)", "/w/tools/ninja-linux"),
                Install("cmake-linux", "CMake 3.31.6 (build tool)", "/w/tools/cmake-linux")]
        needs, _ = self.check(FakeToolchain([]), plan=plan)
        self.assertEqual(needs.installs, ["CMake 3.31.6", "Ninja 1.12.1"])

    def test_tools_only_some_options_need_are_rows(self):
        tc = FakeToolchain([])
        needs, _ = self.check(tc, build_linux=True, build_windows=True, appimage=True, upscale_textures=True)
        names = [r[0] for r in needs.option_rows]
        self.assertTrue(any(n.startswith("llvm-mingw") for n in names), names)
        self.assertTrue(any(n.startswith("appimagetool") for n in names), names)
        self.assertTrue(any(n.startswith("waifu2x") for n in names), names)
        self.assertTrue(all(not found and detail.startswith(build_needs.AT_BUILD)
                            for _n, found, detail in needs.option_rows))
        needs, _ = self.check(tc, build_linux=True, build_windows=False, appimage=False)
        self.assertEqual(needs.option_rows, [])

    def test_tracy_changes_nothing_that_is_installed(self):
        tc = FakeToolchain([])
        a, _ = self.check(tc, tracy=False)
        b, _ = self.check(tc, tracy=True)
        self.assertEqual((a.problems, a.installs, a.option_rows), (b.problems, b.installs, b.option_rows))


if __name__ == "__main__":
    unittest.main()

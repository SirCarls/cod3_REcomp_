"""Tests for the decomp trace version of the game (BuildOptions.trace_build,
--trace-build; docs/DECOMP_TRACE.md): its step, its own build folder, what it
packs into <output>/bin-trace/ and how the record notes it. Nothing is
compiled: a stand-in runner plays CMake and writes the files a build would.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import pipeline  # noqa: E402
from c3w.common import Log  # noqa: E402
from c3w.pipeline import LEVELS, TRACE_BIN_DIR, BuildOptions, Pipeline  # noqa: E402


def make_pipeline(tmp: Path, host: str = "linux", **opts) -> Pipeline:
    o = BuildOptions(output_dir=str(tmp / "out"), work_dir=str(tmp / "work"), **opts)
    with mock.patch.object(pipeline, "IS_LINUX", host == "linux"), \
            mock.patch.object(pipeline, "IS_WINDOWS", host == "windows"):
        return Pipeline(o, Log(sinks=[]), threading.Event())


def titles(host: str = "linux", **opts) -> list:
    with tempfile.TemporaryDirectory() as tmp, \
            mock.patch.object(pipeline, "IS_LINUX", host == "linux"), \
            mock.patch.object(pipeline, "IS_WINDOWS", host == "windows"):
        return [s.title for s in make_pipeline(Path(tmp), host, **opts).steps()]


class FakeCMake:
    """Stands in for Runner.run: records each command; a --build of a folder
    writes the game's files into it, as a real build would."""

    def __init__(self):
        self.commands = []

    def run(self, cmd, **kwargs):
        cmd = [str(c) for c in cmd]
        self.commands.append(cmd)
        if "--build" in cmd:
            build = Path(cmd[cmd.index("--build") + 1])
            build.mkdir(parents=True, exist_ok=True)
            (build / "cod3").write_bytes(b"\x7fELF trace")
            for level in LEVELS:
                (build / f"libcod3_{level}.so").write_bytes(b"\x7fELF")
        return 0


class TraceStepTests(unittest.TestCase):
    def test_off_by_default(self):
        self.assertNotIn("Decomp trace version", titles())

    def test_right_after_the_normal_game_is_packed(self):
        t = titles(trace_build=True)
        self.assertEqual(t[t.index("Packaging") + 1], "Decomp trace version")

    def test_only_with_this_systems_game(self):
        self.assertNotIn("Decomp trace version", titles(trace_build=True, build_recomp=False))
        # Linux building only the Windows version: no trace version (it is this system's game only).
        self.assertNotIn("Decomp trace version", titles(trace_build=True, build_linux=False, build_windows=True))

    def test_a_repair_leaves_it_alone_unless_asked(self):
        self.assertNotIn("Decomp trace version",
                         titles(trace_build=True, only_steps=["sdk", "recompile", "package"]))
        t = titles(trace_build=True, only_steps=["trace"])
        self.assertIn("Decomp trace version", t)
        self.assertIn("Recompiling Call of Duty 3", t)   # what it needs runs too (STEP_NEEDS)

    def test_the_flag_reaches_the_build_from_the_command_line(self):
        import cod3_waw_setup
        seen = []

        class FakePipeline:
            def __init__(self, opts, log, cancel):
                seen.append(opts)

            def run(self):
                pass
        ask = pipeline.APPROVAL.ask
        try:
            for extra, want in (([], False), (["--trace-build"], True)):
                argv = ["setup", "build", "--iso", "game.iso", "--no-waw", "--yes"] + extra
                with mock.patch.object(sys, "argv", argv), \
                        mock.patch.object(cod3_waw_setup, "Pipeline", FakePipeline), mock.patch("builtins.print"):
                    self.assertEqual(cod3_waw_setup.main(), 0)
                self.assertEqual(seen[-1].trace_build, want)
        finally:
            pipeline.APPROVAL.ask = ask


class TraceBuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.patches = [mock.patch.object(pipeline, "IS_LINUX", True), mock.patch.object(pipeline, "IS_WINDOWS", False)]
        for p in self.patches:
            p.start()
        self.p = make_pipeline(self.root, trace_build=True)
        self.p.tc = SimpleNamespace(cmake=Path("cmake"), cc=Path("clang"), cxx=Path("clang++"), ninja=Path("ninja"),
                                    env={})
        self.fake = FakeCMake()
        self.p.runner = self.fake
        # The game's files are gathered from the build folder as they are (no SDK here).
        self.gather = mock.patch.object(Pipeline, "_binaries",
                                        lambda s, build=None: sorted(p for p in (build or s.proj_build).iterdir()))
        self.gather.start()

    def tearDown(self):
        self.gather.stop()
        for p in self.patches:
            p.stop()
        self.tmp.cleanup()

    def test_own_build_folder_with_the_trace_option(self):
        self.p.trace_game()
        configure = next(c for c in self.fake.commands if "-S" in c)
        self.assertIn("-DCOD3_DECOMP_TRACE=ON", configure)
        build = Path(configure[configure.index("-B") + 1])
        self.assertEqual(build, self.p.trace_build_dir)
        self.assertNotEqual(build, self.p.proj_build)
        self.assertFalse(self.p.proj_build.exists())   # the normal build folder isn't touched

    def test_packed_next_to_the_normal_game(self):
        self.p.trace_game()
        bindir = self.p.out / TRACE_BIN_DIR
        self.assertTrue((bindir / "cod3").is_file())
        self.assertTrue((bindir / "libcod3_saint_lo.so").is_file())
        self.assertEqual(self.p.trace_exe_rel, f"{TRACE_BIN_DIR}/cod3")
        launcher = self.p.out / "Call of Duty 3 (decomp trace).sh"
        self.assertIn(f"./{TRACE_BIN_DIR}/cod3", launcher.read_text())
        self.assertIn(f"{TRACE_BIN_DIR}/cod3", self.p.packed["trace"])
        self.assertFalse((self.p.out / "bin").exists())   # the normal game's folder isn't touched

    def test_a_failed_trace_build_only_warns(self):
        def failing(cmd, **kwargs):
            if "--build" in [str(c) for c in cmd]:
                raise pipeline.SetupError("compiler crashed")
            return 0
        self.p.runner = SimpleNamespace(run=failing)
        warnings = []
        self.p.log = SimpleNamespace(info=lambda *a, **k: None, warn=warnings.append)
        self.p.trace_game()
        self.assertEqual(len(warnings), 1)
        self.assertIn("compiler crashed", warnings[0])
        self.assertIsNone(self.p.trace_exe_rel)

    def test_recorded_in_the_manifest(self):
        self.p.trace_game()
        self.p.out.mkdir(parents=True, exist_ok=True)
        with mock.patch.object(self.p, "tool_root", self.root / "no-tool"):
            self.p.finish()
        record = pipeline.read_json(self.p.out / "cod3_waw.json")
        self.assertEqual(record["cod3_trace_exe"], f"{TRACE_BIN_DIR}/cod3")
        self.assertIn(f"{TRACE_BIN_DIR}/libcod3_saint_lo.so", record["cod3_files_trace"])


if __name__ == "__main__":
    unittest.main()

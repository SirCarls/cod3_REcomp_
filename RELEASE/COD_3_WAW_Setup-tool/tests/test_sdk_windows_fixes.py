"""[acct2-decomp] Tests for the SDK build's fixes of 2026-11-05 (a user's Windows
build stopped at the SDK's install: "file INSTALL cannot find ...TracyClient.dll"):
the SDK is configured without Tracy and without AMD's FidelityFX SDK, a file the
install can't find is noted and explained, and the fork's remote is asked about
quietly. Nothing is built: the commands are only recorded.

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
from c3w.common import Log, SetupError  # noqa: E402
from c3w.pipeline import BuildOptions, Pipeline  # noqa: E402

DLL = "C:/w/rexglue-sdk/out/win-amd64/Release/TracyClient.dll"
# CMake's own words, as the user's log has them.
INSTALL_OUTPUT = [
    "-- Installing: C:/w/rexglue-sdk/out/install/win-amd64/lib/TracyClient.lib",
    "CMake Error at cmake_install.cmake:321 (file):",
    "  file INSTALL cannot find",
    f'  "{DLL}":',
    "  File exists.",
    "FAILED: CMakeFiles/Release/install.util",
    "ninja: build stopped: subcommand failed.",
]


class SdkBuildTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def pipe(self) -> Pipeline:
        o = BuildOptions(output_dir=str(self.root / "out"), work_dir=str(self.root / "work"))
        with mock.patch.object(pipeline, "IS_LINUX", True), mock.patch.object(pipeline, "IS_WINDOWS", False):
            p = Pipeline(o, Log(sinks=[]), threading.Event())
        p.tc = SimpleNamespace(cmake="cmake", cc="clang", cxx="clang++", ninja="ninja", git="git", env={})
        return p

    def test_the_install_lines_name_the_missing_file(self):
        shown, vanished = [], []
        note = Pipeline._note_vanished(shown.append, vanished)
        for line in INSTALL_OUTPUT:
            note(line)
        self.assertEqual(vanished, [DLL])
        self.assertEqual(shown, INSTALL_OUTPUT)          # the log still gets every line
        one_line = []
        note = Pipeline._note_vanished(lambda _l: None, one_line)
        note('CMake Error: file INSTALL cannot find "/w/out/libfoo.so": No such file or directory.')
        note('  "/w/out/not-this-one.so"')               # a later quoted path isn't taken
        self.assertEqual(one_line, ["/w/out/libfoo.so"])
        handle = shown.append
        self.assertIs(Pipeline._note_vanished(handle, None), handle)   # nothing to keep: the handler itself

    def test_the_explanation_names_the_file_once_and_the_work_folder(self):
        p = self.pipe()
        with mock.patch.object(pipeline, "IS_WINDOWS", True):
            text = p._vanished_message([DLL.replace("/", "\\"), DLL])
        self.assertEqual(text.count("TracyClient.dll"), 1)
        for words in ("antivirus", "Protection history", "Exclusions", "Build again", str(p.work_real),
                      "Kaspersky", "Quarantine"):   # [acct2-decomp] the user's antivirus was Kaspersky
            self.assertIn(words, text)
        with mock.patch.object(pipeline, "IS_WINDOWS", False):
            text = p._vanished_message(["/w/out/libfoo.so"])
        self.assertIn("libfoo.so", text)
        self.assertNotIn("Windows Security", text)

    def test_the_sdk_is_configured_without_tracy_and_fidelityfx(self):
        p = self.pipe()
        commands = []
        p.runner = SimpleNamespace(run=lambda cmd, **kw: commands.append([str(c) for c in cmd]) or 0)
        p._build_sdk([])
        configure, build = commands
        self.assertIn("-DREXGLUE_ENABLE_TRACY=OFF", configure)
        self.assertIn("-DREXGLUE_ENABLE_FIDELITYFX=OFF", configure)
        self.assertFalse(any("FIDELITYFX=ON" in c for c in configure))
        self.assertEqual(build[build.index("--target") + 1], "install")

    def _sdk_run(self, p: Pipeline, lines):
        def run(cmd, **kw):
            if "--build" in [str(c) for c in cmd]:
                for line in lines:
                    kw["line_filter"](line)
                raise SetupError("Command failed (exit code 1): cmake --build ...")
            return 0
        p.runner = SimpleNamespace(run=run)
        with mock.patch.object(p, "_sdk_built", return_value=False), mock.patch.object(p, "_checkout_sdk"), \
                mock.patch.object(p, "_sdk_source"), mock.patch.object(p, "_sdk_stamp_data", return_value={}):
            with self.assertRaises(SetupError) as e:
                p.sdk()
        return str(e.exception)

    def test_a_vanished_file_becomes_the_explanation(self):
        text = self._sdk_run(self.pipe(), INSTALL_OUTPUT)
        self.assertIn("TracyClient.dll", text)
        self.assertNotIn("Command failed", text)
        self.assertFalse((self.root / "work" / "sdk.stamp.json").exists())

    def test_any_other_failure_stays_as_it_was(self):
        text = self._sdk_run(self.pipe(), ["FAILED: src/foo.cpp.obj", "foo.cpp:1:1: error: expected ';'"])
        self.assertIn("Command failed", text)

    def test_the_fork_remote_is_asked_about_quietly(self):
        p = self.pipe()
        ran, asked = [], []
        p.runner = SimpleNamespace(run=lambda cmd, **kw: ran.append([str(c) for c in cmd]) or 0,
                                   capture=lambda cmd, **kw: (asked.append([str(c) for c in cmd]), (2, ""))[1])
        source = SimpleNamespace(fork_repo="https://example.invalid/fork.git", fork_commit="0" * 40)
        with mock.patch.object(p, "_sdk_has_commit", return_value=True):
            self.assertTrue(p._fetch_fork(source))
        self.assertTrue(any("get-url" in c for c in asked))
        self.assertFalse(any("get-url" in c for c in ran))     # not printed in the log any more
        self.assertTrue(any(c[-3:] == ["add", "cod3-fork", source.fork_repo] for c in ran))

    # [acct2-decomp] 2026-11-05 (owner 10:31: "Make this tracy setting a toggle within the setup tool")
    def test_the_tracy_box_turns_it_on_and_rebuilds_the_sdk(self):
        self.assertFalse(BuildOptions().tracy)                       # off by default
        p = self.pipe()
        default = p._sdk_stamp_data("clang++")
        self.assertNotIn("tracy", default)                           # an SDK built before the box still counts
        p.o.tracy = True
        commands = []
        p.runner = SimpleNamespace(run=lambda cmd, **kw: commands.append([str(c) for c in cmd]) or 0)
        p._build_sdk([])
        self.assertIn("-DREXGLUE_ENABLE_TRACY=ON", commands[0])
        self.assertNotIn("-DREXGLUE_ENABLE_TRACY=OFF", commands[0])
        self.assertEqual(p._sdk_stamp_data("clang++"), {**default, "tracy": True})   # a change rebuilds it
        from c3w import cross
        self.assertIn("-DREXGLUE_ENABLE_TRACY=OFF", cross.sdk_configure_args(Path("t.cmake"), Path("i")))
        self.assertIn("-DREXGLUE_ENABLE_TRACY=ON", cross.sdk_configure_args(Path("t.cmake"), Path("i"), tracy=True))

    def test_tracy_is_kept_in_profiles_and_offered_on_the_command_line(self):
        from c3w import profiles
        self.assertEqual(profiles.clean_settings({"tracy": True}, []), {"tracy": True})
        import subprocess
        tool = Path(__file__).resolve().parent.parent / "cod3_waw_setup.py"
        out = subprocess.run([sys.executable, str(tool), "build", "--help"], capture_output=True, text=True,
                             timeout=120).stdout
        self.assertIn("--tracy", out)

    def test_build_options_have_no_fidelityfx(self):
        self.assertFalse(hasattr(BuildOptions(), "fidelityfx"))
        self.assertFalse(hasattr(pipeline, "fidelityfx_available"))


if __name__ == "__main__":
    unittest.main()

"""Tests for "Check this PC" on Windows and Linux (detection and the table it
shows), Wine, and the setup scripts. Windows is simulated: nothing is
installed or run.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

TOOL = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOL))

from c3w import common, deps  # noqa: E402
from c3w.common import Log, Runner  # noqa: E402


def windows(**extra):
    """Patches that make deps behave as on Windows (nothing found)."""
    patches = [mock.patch.object(deps, "IS_WINDOWS", True), mock.patch.object(deps, "IS_LINUX", False),
               mock.patch.object(deps, "refresh_windows_path", lambda: None),
               mock.patch.object(deps, "_windows_extra_dirs", lambda: []),
               mock.patch.object(deps, "find_vs", lambda runner, *a, **k: None),
               mock.patch.object(deps, "_which", lambda names, extra, env_path=None: None),
               mock.patch.object(deps, "find_cargo", lambda tools_dir=None: None),
               mock.patch.object(deps.shutil, "which", lambda *a, **k: None)]
    patches += [mock.patch.object(*p) for p in extra.get("more", [])]
    return patches


class CheckThisPcTests(unittest.TestCase):
    def run_patched(self, patches, func):
        for p in patches:
            p.start()
        try:
            return func()
        finally:
            for p in reversed(patches):
                p.stop()

    def test_detect_on_windows_with_nothing_installed(self):
        with tempfile.TemporaryDirectory() as tmp:
            tc = self.run_patched(windows(), lambda: deps.detect(Path(tmp), Runner(Log(sinks=[]),
                                                                                    threading.Event())))
            rows = self.run_patched(windows(), tc.rows)
            problems = self.run_patched(windows(), lambda: tc.problems(recomp=True))
        names = [r[0] for r in rows]
        self.assertIn("Visual Studio tools", names)
        self.assertIn("Windows SDK", names)
        self.assertNotIn("Development libraries", names)   # Linux only
        self.assertTrue(all(not found for name, found, _ in rows if "optional" not in name))
        self.assertTrue(any("Visual Studio" in p for p in problems))

    def test_rows_on_linux(self):
        tc = deps.Toolchain(cargo=Path("/usr/bin/cargo"), rust_version=(1, 90))
        with mock.patch.object(deps, "IS_WINDOWS", False), mock.patch.object(deps, "IS_LINUX", True):
            rows = tc.rows()
            summary = tc.summary()
        self.assertEqual(rows[0], ("Rust", True, "/usr/bin/cargo (1.90)"))
        self.assertIn("Development libraries", [r[0] for r in rows])
        self.assertIn("MISSING", summary)   # git etc. in the plain-text log version

    def test_wine_row_and_no_visual_studio_installer(self):
        tc = deps.Toolchain()
        with mock.patch.object(deps, "IS_WINDOWS", True), mock.patch.object(deps, "IS_LINUX", False), \
                mock.patch.object(common, "UNDER_WINE", True), \
                mock.patch.object(common, "wine_version", lambda: "wine-9.0"):
            self.assertIn("Wine (optional)", [r[0] for r in tc.rows()])
            with mock.patch.object(deps, "detect", lambda *a: deps.Toolchain()), \
                    mock.patch.object(deps, "fetch_windows_fallback"), \
                    mock.patch.object(deps.shutil, "which", lambda *a, **k: None), \
                    mock.patch.object(deps, "refresh_windows_path", lambda: None), \
                    mock.patch.object(deps, "install_vs_build_tools") as vs:
                with tempfile.TemporaryDirectory() as tmp:
                    deps.install_windows(tc, Path(tmp), Runner(Log(sinks=[]), threading.Event()), Log(sinks=[]),
                                         threading.Event())
                vs.assert_not_called()   # Microsoft's installer doesn't run under Wine

    def test_not_wine_on_linux(self):
        if not common.IS_WINDOWS:
            self.assertEqual(common.wine_version(), "")
            self.assertFalse(common.UNDER_WINE)


class SetupScriptTests(unittest.TestCase):
    def test_bat_is_windows_only_and_crlf(self):
        for bat in (TOOL / "setup.bat", TOOL.parent / "setup.bat"):
            data = bat.read_bytes()
            self.assertNotIn(b"\n", data.replace(b"\r\n", b""), bat)   # cmd wants CRLF
            self.assertTrue(data.startswith(b":; echo"), bat)            # a label to cmd, a stop to sh
            if not common.IS_WINDOWS:
                r = subprocess.run(["sh", str(bat)], capture_output=True, text=True, timeout=30)
                self.assertEqual(r.returncode, 1)
                self.assertIn("sh setup.sh", r.stdout)

    def test_bat_knows_wine(self):
        text = (TOOL / "setup.bat").read_text()
        self.assertIn(r'reg query "HKCU\Software\Wine"', text)

    def test_sh_runs_on_windows_shells_too(self):
        text = (TOOL / "setup.sh").read_text()
        for marker in ("MINGW*|MSYS*|CYGWIN*", "py -3", "cmd.exe //c", "cygpath -w"):
            self.assertIn(marker, text)
        if not common.IS_WINDOWS:
            for sh in (TOOL / "setup.sh", TOOL.parent / "setup.sh"):
                self.assertEqual(subprocess.run(["sh", "-n", str(sh)]).returncode, 0, sh)


class ApprovalTests(unittest.TestCase):
    """Nothing is downloaded or installed without a yes (owner, 2026-10-10)."""

    def setUp(self):
        self.saved = common.APPROVAL.ask
        common.APPROVAL.reset()

    def tearDown(self):
        common.APPROVAL.ask = self.saved
        common.APPROVAL.reset()

    def test_no_answer_means_no(self):
        common.APPROVAL.ask = None
        with self.assertRaises(common.SetupError):
            common.APPROVAL.require([common.Install("x", "X", "/somewhere")], Log(sinks=[]))

    def test_asked_once_per_item(self):
        asked = []
        common.APPROVAL.ask = lambda items: asked.append([i.key for i in items]) or True
        items = [common.Install("a", "A", "/a"), common.Install("b", "B", "/b"), common.Install("a", "A", "/a")]
        common.APPROVAL.require(items, Log(sinks=[]))
        common.APPROVAL.require([common.Install("b", "B", "/b")], Log(sinks=[]))   # already approved
        self.assertEqual(asked, [["a", "b"]])

    def test_refused_download_never_starts(self):
        common.APPROVAL.ask = lambda items: False
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(common.urllib.request, "urlopen") as fetch:
                with self.assertRaises(common.SetupError):
                    common.download("https://example.invalid/tool.zip", Path(tmp) / "tool.zip", Log(sinks=[]),
                                    threading.Event())
                fetch.assert_not_called()

    def test_plan_lists_tools_with_their_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            tools = Path(tmp) / "tools"
            tc = deps.Toolchain()   # nothing installed
            with mock.patch.object(deps, "IS_WINDOWS", False), mock.patch.object(deps, "IS_LINUX", True), \
                    mock.patch.object(deps, "_linux_package_manager", lambda: None), \
                    mock.patch.object(deps, "_which", lambda names, extra, env_path=None: None):
                items = deps.plan(tc, tools, True, recomp=True)
        keys = {i.key: i for i in items}
        for key in ("cmake-linux", "ninja-linux", "7zip-linux", "llvm-linux", "rust"):
            self.assertIn(key, keys)
        self.assertEqual(keys["cmake-linux"].where, str(tools / "cmake-linux"))
        self.assertEqual(keys["rust"].where, str(tools / "rust"))   # in the project folder, not ~/.cargo

    def test_local_rust_sets_its_homes(self):
        with tempfile.TemporaryDirectory() as tmp:
            tools = Path(tmp)
            cargo = deps.rust_dir(tools) / "cargo" / "bin" / ("cargo.exe" if common.IS_WINDOWS else "cargo")
            cargo.parent.mkdir(parents=True)
            cargo.write_text("")
            with mock.patch.dict("os.environ", {}, clear=False):
                self.assertEqual(deps.find_cargo(tools), cargo)
                import os
                self.assertEqual(os.environ["RUSTUP_HOME"], str(deps.rust_dir(tools) / "rustup"))

    def test_settings_in_the_project_folder(self):
        self.assertEqual(common.config_dir(), common.project_dir() / "config")


if __name__ == "__main__":
    unittest.main()

"""Tests for the Linux version's two layouts (one AppImage, the default, or a
plain bin/ folder from More options), their launcher scripts, and the
removal of what a build made the other way; and for the build options'
defaults (the AppImage on, AMD's FidelityFX SDK off). Made-up files only;
nothing is built.

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
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import pipeline, profiles  # noqa: E402
from c3w.common import Log  # noqa: E402
from c3w.pipeline import APPIMAGE_NAME, BuildOptions, Pipeline  # noqa: E402

TOOL = Path(__file__).resolve().parent.parent
SH = shutil.which("sh")


def write(path: Path, data: bytes = b"\x7fELF") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


class DefaultsTests(unittest.TestCase):
    def test_appimage_is_the_default_and_fidelityfx_is_off(self):
        o = BuildOptions()
        self.assertTrue(o.appimage)
        self.assertFalse(o.fidelityfx)

    def test_command_line_has_the_bin_folder_and_fidelityfx_switches(self):
        out = subprocess.run([sys.executable, str(TOOL / "cod3_waw_setup.py"), "build", "--help"],
                             capture_output=True, text=True, timeout=60).stdout
        self.assertIn("--bin-folder", out)
        self.assertIn("--fidelityfx", out)
        self.assertNotIn("--no-fidelityfx", out)   # still taken, but no longer shown

    def test_old_fidelityfx_setting_is_left_out_quietly(self):
        notes = []
        out = profiles.clean_settings({"fidelityfx": True, "fidelityfx_sdk": True, "appimage": False}, notes)
        self.assertEqual(out, {"fidelityfx_sdk": True, "appimage": False})
        self.assertEqual(notes, [])


class LauncherTests(unittest.TestCase):
    def test_bin_launcher_is_the_one_older_builds_wrote(self):
        self.assertEqual(pipeline.linux_launcher("cod3", appimage=False),
                         '#!/bin/sh\ncd "$(dirname "$0")" && exec ./bin/cod3 "$@"\n')

    def test_appimage_launchers_name_their_program(self):
        for program in ("cod3", "cod3mp"):
            text = pipeline.linux_launcher(program, appimage=True)
            self.assertIn(f"COD3_PROGRAM={program} exec ./{APPIMAGE_NAME} \"$@\"", text)
            self.assertIn("APPIMAGE_EXTRACT_AND_RUN=1", text)

    @unittest.skipUnless(SH and os.name == "posix", "needs a POSIX shell")
    def test_launchers_start_the_right_program(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out dir"   # a space, as in the owner's folders
            fake = '#!/bin/sh\necho "$(basename "$0") program=${COD3_PROGRAM:-} args=$*"\n'
            for rel in (APPIMAGE_NAME, "bin/cod3", "bin/cod3mp"):
                p = write(out / rel, fake.encode())
                p.chmod(0o755)
            for appimage, program, expected in ((True, "cod3", f"{APPIMAGE_NAME} program=cod3"),
                                                (True, "cod3mp", f"{APPIMAGE_NAME} program=cod3mp"),
                                                (False, "cod3", "cod3 program="),
                                                (False, "cod3mp", "cod3mp program=")):
                launcher = out / "launch.sh"
                launcher.write_text(pipeline.linux_launcher(program, appimage), encoding="utf-8")
                launcher.chmod(0o755)
                env = {k: v for k, v in os.environ.items() if k != "COD3_PROGRAM"}
                env["COD3_PROGRAM"] = "cod3mp" if program == "cod3" else "cod3"   # an inherited value loses
                if not appimage:
                    env.pop("COD3_PROGRAM")
                res = subprocess.run([SH, str(launcher), "--a", "b c"], capture_output=True, text=True,
                                     cwd=tmp, env=env, timeout=30)
                self.assertEqual(res.stdout.strip(), f"{expected} args=--a b c", (appimage, program, res.stderr))

    def test_own_launcher_is_recognized(self):
        with tempfile.TemporaryDirectory() as tmp:
            own = Path(tmp) / "own.sh"
            own.write_text(pipeline.linux_launcher("cod3mp", True), encoding="utf-8")
            other = Path(tmp) / "other.sh"
            other.write_text("#!/bin/sh\necho mine\n", encoding="utf-8")
            self.assertTrue(pipeline.is_own_launcher(own))
            self.assertFalse(pipeline.is_own_launcher(other))
            self.assertFalse(pipeline.is_own_launcher(Path(tmp) / "missing.sh"))


class StaleLayoutTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.out = self.root / "out"
        self.out.mkdir()

    def tearDown(self):
        self._tmp.cleanup()

    def pipe(self) -> Pipeline:
        o = BuildOptions(output_dir=str(self.out), work_dir=str(self.root / "work"))
        with mock.patch.object(pipeline, "IS_LINUX", True), mock.patch.object(pipeline, "IS_WINDOWS", False):
            return Pipeline(o, Log(sinks=[]), threading.Event())

    def test_appimage_build_removes_the_old_bin_game_only(self):
        for name in ("cod3", "cod3mp", "libcod3_forest.so", "librexruntime.so", "libSDL3.so.0"):
            write(self.out / "bin" / name)
        keep = [write(self.out / "bin" / n) for n in ("cod3.exe", "librexruntime.dll", "notes.txt")]
        write(self.out / "bin-trace" / "cod3")
        stale = pipeline.stale_linux_files(self.out, appimage=True)
        self.assertEqual(sorted(p.name for p in stale),
                         ["cod3", "cod3mp", "libSDL3.so.0", "libcod3_forest.so", "librexruntime.so"])
        self.pipe()._remove_other_linux_layout(appimage=True)
        self.assertEqual(sorted(p.name for p in (self.out / "bin").iterdir()), sorted(k.name for k in keep))
        self.assertTrue((self.out / "bin-trace" / "cod3").exists())

    def test_appimage_build_removes_an_emptied_bin_folder(self):
        write(self.out / "bin" / "cod3")
        write(self.out / "bin" / "libcod3_forest.so")
        self.pipe()._remove_other_linux_layout(appimage=True)
        self.assertFalse((self.out / "bin").exists())

    def test_bin_build_removes_an_old_appimage(self):
        write(self.out / APPIMAGE_NAME)
        self.pipe()._remove_other_linux_layout(appimage=False)
        self.assertFalse((self.out / APPIMAGE_NAME).exists())

    @unittest.skipUnless(os.name == "posix", "symlinks")
    def test_links_are_left_alone(self):
        target = write(self.root / "elsewhere" / "cod3")
        (self.out / "bin").mkdir()
        (self.out / "bin" / "cod3").symlink_to(target)
        (self.out / APPIMAGE_NAME).symlink_to(target)
        self.assertEqual(pipeline.stale_linux_files(self.out, appimage=True), [])
        self.assertEqual(pipeline.stale_linux_files(self.out, appimage=False), [])
        self.assertTrue(target.exists())

    def test_launchers_follow_the_layout(self):
        p = self.pipe()
        p._write_linux_launchers(appimage=True, mp=True)
        self.assertIn(APPIMAGE_NAME, (self.out / "Call of Duty 3.sh").read_text(encoding="utf-8"))
        self.assertIn("COD3_PROGRAM=cod3mp",
                      (self.out / "Call of Duty 3 Multiplayer.sh").read_text(encoding="utf-8"))
        p._write_linux_launchers(appimage=False, mp=False)
        self.assertIn("./bin/cod3 ", (self.out / "Call of Duty 3.sh").read_text(encoding="utf-8"))
        self.assertFalse((self.out / "Call of Duty 3 Multiplayer.sh").exists())   # no multiplayer now

    def test_someone_elses_multiplayer_script_is_kept(self):
        mine = self.out / "Call of Duty 3 Multiplayer.sh"
        mine.write_text("#!/bin/sh\necho my own script\n", encoding="utf-8")
        self.pipe()._write_linux_launchers(appimage=True, mp=False)
        self.assertTrue(mine.exists())


if __name__ == "__main__":
    unittest.main()

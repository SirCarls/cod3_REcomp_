"""Tests for c3w/disc_hd.py: HD textures for Finest Hour and Big Red One, made from the disc's own textures
[setup-layout]. No game, ripper or waifu2x needed: their parts are stood in for, and nothing is built.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
from __future__ import annotations

import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import disc_hd, fh, upscale  # noqa: E402
from c3w.common import Log  # noqa: E402


def _game(tmp: str) -> Path:
    """<output>/fh with the disc's COD.PAK copied in, in the disc's own letter case."""
    out = Path(tmp) / "fh"
    (out / "game").mkdir(parents=True)
    (out / "game" / "cod.pak").write_bytes(b"Spark Pack")
    return out


def _pack(out: Path, files: int) -> None:
    pack = disc_hd.pack_dir(out)
    pack.mkdir(parents=True, exist_ok=True)
    for i in range(files):
        (pack / f"64x64_{i:016x}.png").write_bytes(b"png")


class FolderTests(unittest.TestCase):
    def test_sources_are_found_in_any_letter_case(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = _game(tmp)
            self.assertEqual(disc_hd.GAMES["fh"].source(out), out / "game" / "cod.pak")
            self.assertIsNone(disc_hd.GAMES["bro"].source(out))
            (out / "game" / "Textures").mkdir()
            self.assertEqual(disc_hd.GAMES["bro"].source(out), out / "game" / "Textures")

    def test_the_variable_is_set_only_with_a_pack_and_the_option_on(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = _game(tmp)
            self.assertEqual(disc_hd.play_variables("fh", out, True), {})
            self.assertIn("No HD pack yet", disc_hd.status(out))
            _pack(out, 3)
            self.assertEqual(disc_hd.play_variables("fh", out, True),
                             {"FH_HD_TEXTURES": str(disc_hd.pack_dir(out).resolve())})
            self.assertEqual(disc_hd.play_variables("fh", out, False), {})
            self.assertEqual(disc_hd.play_variables("bro", out, True),
                             {"BRO_HD_TEXTURES": str(disc_hd.pack_dir(out).resolve())})
            self.assertEqual(disc_hd.status(out), "HD pack: 3 textures.")

    def test_finest_hours_start_sets_it_and_clears_an_old_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = _game(tmp)
            base = {"FH_HD_TEXTURES": "/somewhere/else"}
            self.assertNotIn("FH_HD_TEXTURES", fh.play_env(fh.PlayOptions(), base, game_out=out))
            _pack(out, 1)
            env = fh.play_env(fh.PlayOptions(), base, game_out=out)
            self.assertEqual(env["FH_HD_TEXTURES"], str(disc_hd.pack_dir(out).resolve()))
            self.assertNotIn("FH_HD_TEXTURES", fh.play_env(fh.PlayOptions(hd_textures=False), base, game_out=out))

    def test_the_play_choice_is_saved(self):
        s = fh.PlayOptions(hd_textures=False).to_settings()
        self.assertEqual(s["fh_hd_textures"], False)
        self.assertFalse(fh.PlayOptions.from_settings(s).hd_textures)
        self.assertTrue(fh.PlayOptions.from_settings({"fh_hd_textures": "yes"}).hd_textures)   # wrong type: default


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = _game(self.tmp.name)
        self.calls = []
        self.log = mock.Mock(spec=Log)

    def tearDown(self):
        self.tmp.cleanup()

    def runner(self, rip_rc: int = 0):
        test = self

        class Runner:
            def run(self, cmd, **kw):
                cmd = [str(c) for c in cmd]
                test.calls.append(cmd)
                textures = Path(cmd[3]) / disc_hd.TEXTURES_DIR
                textures.mkdir(parents=True, exist_ok=True)
                (textures / upscale.KEYS_FILE).write_text("# key\n")
                return rip_rc
        return Runner()

    def build(self, rip_rc: int = 0, upscaled: int = 2, cancel: bool = False):
        def upscale_folder(tool, src, dst, staging, settings, runner, log, cancel_event):
            _pack(self.out, upscaled)
            return upscaled, 0
        event = threading.Event()
        if cancel:
            event.set()
        with mock.patch.object(disc_hd, "_ripper", return_value=Path("rip")), \
                mock.patch.object(disc_hd.deps, "fetch_portable", return_value=Path("waifu2x")), \
                mock.patch.object(disc_hd.upscale, "upscale_folder", side_effect=upscale_folder):
            return disc_hd.build("fh", self.out, Path("tool"), Path("tools"), Path(self.tmp.name) / "scratch",
                                 upscale.Settings(), 1, self.runner(rip_rc), self.log, event)

    def test_rips_the_pak_and_upscales_into_the_pack(self):
        self.assertEqual(self.build(), 2)
        self.assertEqual(self.calls, [["rip", "fh-hd-textures", str(self.out / "game" / "cod.pak"),
                                       str(disc_hd.hd_dir(self.out))]])

    def test_a_failing_ripper_is_a_warning_and_keeps_the_earlier_pack(self):
        _pack(self.out, 5)
        self.assertIsNone(self.build(rip_rc=2))
        warning = self.log.warn.call_args[0][0]
        self.assertIn("exit code 2", warning)
        self.assertIn("earlier build stays in use", warning)
        self.assertEqual(disc_hd.pack_size(self.out), 5)

    def test_without_the_disc_files_nothing_runs(self):
        (self.out / "game" / "cod.pak").unlink()
        self.assertIsNone(self.build())
        self.assertEqual(self.calls, [])

    def test_waifu2x_is_on_the_approval_list_until_it_is_there(self):
        with mock.patch.object(disc_hd.deps, "_portable_ok", return_value=False):
            self.assertEqual([i.key for i in disc_hd.downloads(Path("tools"))], [upscale.portable_key()])
        with mock.patch.object(disc_hd.deps, "_portable_ok", return_value=True):
            self.assertEqual(disc_hd.downloads(Path("tools")), [])


if __name__ == "__main__":
    unittest.main()

"""[setup-layout] 2026-11-24 (owner: "it should be portable."; chose "Both"): Call of Duty 3 keeps its settings, saves
and log in its own folder, always.

- the game: recomp/src/user_root.h Pick no longer moves them to a folder of the player's own; a folder it can't write
  ends the game with a message (the source is read; nothing is built);
- the setup tool: settings are always the game folder's, and Build and Play say up front when the folder can't be
  written (game_settings.portable_problem), naming where an earlier version put such files.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import game_settings  # noqa: E402

TOOL = Path(__file__).resolve().parent.parent


class ToolKeepsItPortable(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.game = self.base / "game"
        self.game.mkdir()
        self.own = self.base / "own" / "cod3-recomp"

    def test_settings_are_the_game_folders(self):
        self.assertEqual(game_settings.settings_file(self.game), self.game / "cod3.toml")
        self.assertEqual(game_settings.settings_file(self.game, "cod3mp.toml"), self.game / "cod3mp.toml")

    def test_a_folder_it_can_write_is_fine_and_left_as_it_was(self):
        self.assertIsNone(game_settings.portable_problem(self.game))
        self.assertIsNone(game_settings.portable_problem(self.base / "new" / "game"))   # not made yet: its parent
        self.assertEqual(sorted(p.name for p in self.game.iterdir()), [])
        self.assertFalse((self.base / "new").exists())

    def test_a_folder_it_cant_write_says_so(self):
        with mock.patch.object(game_settings, "_writable", return_value=False), \
                mock.patch.object(game_settings, "fallback_roots", return_value=[self.own]):
            why = game_settings.portable_problem(self.game)
        self.assertIn("can't be written", why)
        self.assertIn(str(self.game), why)
        self.assertNotIn("earlier version", why)

    def test_files_an_earlier_version_moved_are_named(self):
        (self.own / "userdata").mkdir(parents=True)
        with mock.patch.object(game_settings, "_writable", return_value=False), \
                mock.patch.object(game_settings, "fallback_roots", return_value=[self.base / "none", self.own]):
            why = game_settings.portable_problem(self.game)
        self.assertIn(f"earlier version are in {self.own}", why)

    def test_the_earlier_folders_match_the_game(self):
        src = (TOOL / "recomp" / "src" / "user_root.h").read_text(encoding="utf-8")
        for name in ('L"CoD3-recomp"', '"cod3-recomp"', '".local" / "share" / "cod3-recomp"',
                     '"XDG_DATA_HOME"', '"XDG_RUNTIME_DIR"', '"cod3-recomp-"', 'L"LOCALAPPDATA"', 'L"TEMP"'):
            self.assertIn(name, src)
        if os.name != "nt":
            env = {"XDG_DATA_HOME": "/x/data", "HOME": "/h", "XDG_RUNTIME_DIR": "relative"}
            with mock.patch.dict(os.environ, env, clear=False):
                roots = game_settings.fallback_roots()
            self.assertEqual(roots[:2], [Path("/x/data/cod3-recomp"), Path("/h/.local/share/cod3-recomp")])
            self.assertEqual(roots[2], Path("/tmp") / f"cod3-recomp-{os.getuid()}")   # the relative one skipped


class GameKeepsItPortable(unittest.TestCase):
    def setUp(self):
        self.src = (TOOL / "recomp" / "src" / "user_root.h").read_text(encoding="utf-8")
        start = self.src.index("inline std::filesystem::path Pick(")
        self.pick = self.src[start:self.src.index("\n}\n", start)]

    def test_pick_never_returns_another_folder(self):
        self.assertIn('if (Writable(root / "userdata" / "logs")) return root;', self.pick)
        returns = [line.strip() for line in self.pick.splitlines() if line.strip().startswith("return")]
        self.assertTrue(returns)
        self.assertTrue(all(r.startswith("return root;") for r in returns), returns)
        self.assertNotIn("DataRoot() = candidate", self.pick)
        self.assertNotIn("Seed(", self.src)

    def test_a_folder_it_cant_write_ends_with_a_message(self):
        self.assertIn("ShowSimpleMessageBox", self.pick)
        self.assertIn("std::exit(1);", self.pick)
        self.assertIn("Move the game to a folder of your own", self.pick)
        self.assertIn("from an earlier version are in", self.pick)


if __name__ == "__main__":
    unittest.main()

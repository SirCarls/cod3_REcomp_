"""Settings copied between Linux and Windows: paths from the other system are
dropped instead of used (a Linux "/srv/..." work folder became
C:\\srv\\... on a Windows PC, 2026-10-13)."""

import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import common, profiles  # noqa: E402


class ForeignPathTest(unittest.TestCase):
    def test_on_windows(self):
        with mock.patch.object(common, "IS_WINDOWS", True):
            self.assertTrue(common.foreign_path("/srv/games/work"))
            self.assertTrue(common.foreign_path("/opt/x"))
            for fine in ("C:\\Games\\out", "D:/cod3", "\\\\server\\share\\x", "//server/share", "work", "", None, 3):
                self.assertFalse(common.foreign_path(fine), fine)

    def test_on_linux(self):
        with mock.patch.object(common, "IS_WINDOWS", False):
            for foreign in ("C:\\Games\\out", "d:/cod3", "\\\\server\\share"):
                self.assertTrue(common.foreign_path(foreign), foreign)
            for fine in ("/opt/x/out", "relative/dir", "C_drive/x", "", None):
                self.assertFalse(common.foreign_path(fine), fine)

    def test_drop_foreign_paths(self):
        settings = {"work_dir": "/srv/someone/work", "output_dir": "C:\\Games\\out", "jobs": 8,
                    "waw_map_packs": ["/tmp/a.exe", "C:\\b.exe"]}
        with mock.patch.object(common, "IS_WINDOWS", True):
            out, dropped = common.drop_foreign_paths(settings, profiles.PATH_KEYS)
        self.assertNotIn("work_dir", out)
        self.assertEqual(out["output_dir"], "C:\\Games\\out")
        self.assertEqual(out["waw_map_packs"], ["C:\\b.exe"])
        self.assertEqual(out["jobs"], 8)
        self.assertEqual(sorted(dropped), ["waw_map_packs", "work_dir"])
        self.assertEqual(settings["work_dir"], "/srv/someone/work")  # the input is left alone


if __name__ == "__main__":
    unittest.main()

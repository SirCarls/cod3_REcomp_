"""Tests for copying another disc's voices into a game (c3w/voices.py). The
discs are made-up folders with the real releases' layouts; nothing is read
from a real disc.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import voices  # noqa: E402
from c3w.common import Log, SetupError  # noqa: E402


def write(path: Path, data: bytes = b"x") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def make_disc(root: Path, xex: bytes, files) -> None:
    write(root / "default.xex", xex)
    for rel in files:
        write(root / rel, rel.encode())


def known(*discs):
    """KNOWN_DISCS with the made-up discs' default.xex in place of the real ones."""
    table = {}
    for xex, name, langs in discs:
        table[hashlib.sha1(xex).hexdigest()] = (name, langs)
    return mock.patch.object(voices, "KNOWN_DISCS", table)


GERMAN = [
    "sp/blkbrn/blkbrn.wde", "sp/blkbrn/blkbrn.cod", "sp/bcen_usa.wde", "sp/global.wde", "mp/global.wde",
    "movies/blkbrn-de.wma", "movies/blkbrn.wmv", "movies/legal-de.wma",
]
EUROPE = [
    "sp/blkbrn/sound/blkbrn.wul", "sp/blkbrn/sound/blkbrn.wfr", "sp/blkbrn/sound/blkbrn.wes",
    "sp/blkbrn/sound/blkbrn.wit", "sp/bcen_usa.wfr", "sp/bcen_usa.wes", "sp/bcen_usa.wit",
    "sp/sound/global.wul", "movies/blkbrn-fr.wma", "movies/blkbrn-es.wma", "movies/blkbrn-it.wma",
]
USA = ["sp/blkbrn/blkbrn.wbk", "sp/blkbrn/blkbrn.cod", "sp/global.wbk", "movies/blkbrn-en.wma"]


class ClassifyTests(unittest.TestCase):
    def test_voice_files(self):
        for rel in ("sp/blkbrn/blkbrn.wde", "sp/blkbrn/sound/blkbrn.wul", "SP\\Global.WBK",
                    "mp/global.wfr", "movies/blkbrn-it.wma"):
            self.assertTrue(voices.is_voice_file(rel), rel)
        for rel in ("sp/blkbrn/blkbrn.cod", "movies/blkbrn.wmv", "default.xex", "movies/legal.wma",
                    "config/default.cfg"):
            self.assertFalse(voices.is_voice_file(rel), rel)

    def test_languages(self):
        self.assertEqual(voices.language_of("sp/blkbrn/blkbrn.wbk"), "en")
        self.assertEqual(voices.language_of("sp/blkbrn/sound/blkbrn.wes"), "es")
        self.assertEqual(voices.language_of("movies/finale-de.wma"), "de")
        self.assertIsNone(voices.language_of("sp/sound/global.wul"))


class AddVoicesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.game = self.root / "out"
        make_disc(self.game / "assets", b"usa", USA)
        make_disc(self.root / "de", b"german", GERMAN)
        make_disc(self.root / "eu", b"europe", EUROPE)
        self.known = known((b"usa", "USA", ("en",)), (b"german", "Germany", ("de",)),
                           (b"europe", "Europe", ("fr", "es", "it")))
        self.known.start()
        self.log = Log(sinks=[])

    def tearDown(self):
        self.known.stop()
        self.tmp.cleanup()

    def test_german_disc_adds_its_banks_and_movie_sound(self):
        r = voices.add_voices(self.root / "de", self.game, self.log)
        a = self.game / "assets"
        for rel in ("sp/blkbrn/blkbrn.wde", "sp/bcen_usa.wde", "sp/global.wde", "mp/global.wde",
                    "movies/blkbrn-de.wma"):
            self.assertTrue((a / rel).is_file(), rel)
        # the German disc's other files stay out
        self.assertEqual((a / "sp/blkbrn/blkbrn.cod").read_bytes(), b"sp/blkbrn/blkbrn.cod")
        self.assertFalse((a / "movies/blkbrn.wmv").exists())
        self.assertFalse((a / "movies/legal-de.wma").exists())
        self.assertEqual(r["copied"], 5)
        self.assertEqual(r["languages"]["de"], 3)
        self.assertEqual(r["languages"]["en"], 2)

    def test_nothing_is_overwritten_and_a_second_run_copies_nothing(self):
        a = self.game / "assets"
        write(a / "sp/global.wde", b"mine")
        voices.add_voices(self.root / "de", self.game, self.log)
        self.assertEqual((a / "sp/global.wde").read_bytes(), b"mine")
        r = voices.add_voices(self.root / "de", self.game, self.log)
        self.assertEqual(r["copied"], 0)

    def test_one_language_of_a_split_disc_brings_its_sounds(self):
        voices.add_voices(self.root / "eu", self.game, self.log, languages=["fr"])
        a = self.game / "assets"
        for rel in ("sp/blkbrn/sound/blkbrn.wul", "sp/blkbrn/sound/blkbrn.wfr", "sp/bcen_usa.wfr",
                    "sp/sound/global.wul", "movies/blkbrn-fr.wma"):
            self.assertTrue((a / rel).is_file(), rel)
        for rel in ("sp/blkbrn/sound/blkbrn.wes", "sp/bcen_usa.wit", "movies/blkbrn-es.wma"):
            self.assertFalse((a / rel).exists(), rel)

    def test_a_language_the_disc_lacks_copies_nothing(self):
        r = voices.add_voices(self.root / "eu", self.game, self.log, languages=["de"])
        self.assertEqual(r["copied"], 0)
        self.assertFalse((self.game / "assets/sp/blkbrn/sound/blkbrn.wul").exists())

    def test_assets_folder_itself_works_as_the_target(self):
        r = voices.add_voices(self.root / "de", self.game / "assets", self.log)
        self.assertEqual(r["copied"], 5)

    def test_not_a_game_folder(self):
        with self.assertRaises(SetupError):
            voices.add_voices(self.root / "de", self.root / "empty", self.log)

    def test_not_a_known_disc(self):
        make_disc(self.root / "other", b"another game", ["sp/x/x.wde"])
        with self.assertRaises(SetupError):
            voices.add_voices(self.root / "other", self.game, self.log)

    def test_unknown_language(self):
        with self.assertRaises(SetupError):
            voices.add_voices(self.root / "de", self.game, self.log, languages=["jp"])

    def test_the_real_releases_are_known(self):
        self.known.stop()
        try:
            names = [n for n, _ in voices.KNOWN_DISCS.values()]
            self.assertIn("Call of Duty 3 (Germany)", names)
            self.assertIn("Call of Duty 3 (Europe) (Fr,Es,It)", names)
            self.assertIn("681724c40e250e584e6585dd903e05afa00a8634", voices.KNOWN_DISCS)
        finally:
            self.known.start()


if __name__ == "__main__":
    unittest.main()

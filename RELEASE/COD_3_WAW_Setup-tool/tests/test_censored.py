"""[acct2-decomp] Tests for c3w/censored.py: the censored campaign's swap.
Made-up files only; no game data (the shipped lists are only counted).

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import hashlib
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import censored, disc_files  # noqa: E402
from c3w.common import Log, SetupError  # noqa: E402


def _md5(b: bytes) -> str:
    return hashlib.md5(b).hexdigest()


def _sha1(b: bytes) -> str:
    return hashlib.sha1(b).hexdigest()


class ShippedLists(unittest.TestCase):
    def test_the_84(self):
        censored._pairs = None
        ps = censored.pairs()
        self.assertEqual(len(ps), censored.EXPECTED)
        for p in ps:
            low = p.rel.lower()
            self.assertTrue(low.startswith("sp/") and low.endswith(".cod"), p.rel)
            self.assertFalse(censored.has_language(p.rel), p.rel)
            self.assertNotEqual(p.uncut.md5, p.german.md5)
        # the Fr/Es/It disc has the same uncut files as the USA/Europe one
        fr = disc_files.known_files(disc_files.disc_by_key("europe_fr_es_it"))
        self.assertTrue(all(fr[p.rel.lower()].md5 == p.uncut.md5 for p in ps))

    def test_language_archives(self):
        for rel in ("sp/global.cod", "sp/frontend.cod", "sp/stbert/stbert.cod", "SP/Mace2/Mace2.cod"):
            self.assertTrue(censored.has_language(rel), rel)
        for rel in ("sp/stbert/chars.cod", "sp/island/z2/zn02ab.cod", "mp/global.cod"):
            self.assertFalse(censored.has_language(rel), rel)


XEX = {"usa_europe": b"XEX2 usa", "germany": b"XEX2 de", "europe_fr_es_it": b"XEX2 fr", "japan": b"XEX2 jp"}
UNCUT = {"sp/lvl/chars.cod": b"U" * 64, "sp/lvl/zn01.cod": b"u" * 40,
         "sp/lvl/lvl.cod": b"english text", "mp/x.cod": b"mp one"}
GERMAN = {"sp/lvl/chars.cod": b"G" * 64, "sp/lvl/zn01.cod": b"g" * 48,
          "sp/lvl/lvl.cod": b"deutscher text", "mp/x.cod": b"mp two"}


class Swap(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        lists = self.tmp / "lists"
        lists.mkdir()
        self.discs = {"usa_europe": UNCUT, "germany": GERMAN, "europe_fr_es_it": UNCUT, "japan": {}}
        for key, files in self.discs.items():
            rows = dict(files, **{"default.xex": XEX[key]})
            (lists / f"{key}.tsv").write_text("md5\tsize\tpath\n" + "".join(
                f"{_md5(b)}\t{len(b)}\t{p}\n" for p, b in rows.items()), encoding="utf-8")
        self.saved = (disc_files.DATA_DIR, disc_files.DISCS, dict(disc_files._lists), censored._pairs)
        disc_files.DATA_DIR = lists
        disc_files.DISCS = tuple(disc_files.Disc(k, f"Disc {k}", _sha1(XEX[k])) for k in self.discs)
        disc_files._lists.clear()
        censored._pairs = None
        self.game = self.make("out/assets", "usa_europe")
        self.german_disc = self.make("de_disc", "germany")
        self.usa_disc = self.make("us_disc", "usa_europe")
        self.log = Log()

    def make(self, rel: str, key: str) -> Path:
        root = self.tmp / rel
        for p, b in dict(self.discs[key], **{"default.xex": XEX[key]}).items():
            (root / p).parent.mkdir(parents=True, exist_ok=True)
            (root / p).write_bytes(b)
        return root

    def tearDown(self):
        disc_files.DATA_DIR, disc_files.DISCS, lists, censored._pairs = self.saved
        disc_files._lists.clear()
        disc_files._lists.update(lists)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def live(self, rel: str) -> bytes:
        return (self.game / rel).read_bytes()

    def test_pairs(self):
        self.assertEqual([p.rel for p in censored.pairs()], ["sp/lvl/chars.cod", "sp/lvl/zn01.cod"])

    def test_add_on_off(self):
        st = censored.status(self.tmp / "out")
        self.assertFalse(st.censored)
        self.assertFalse(st.ready(censored.GERMAN))
        r = censored.set_censored(self.tmp / "out", True, self.log)     # nothing to switch to yet
        self.assertEqual((r["moved"], len(r["lacking"])), (0, 2))
        r = censored.add(self.german_disc, self.tmp / "out", self.log)
        self.assertEqual(r["copied"], 2)
        self.assertEqual(censored.add(self.german_disc, self.tmp / "out", self.log)["copied"], 0)
        self.assertTrue(censored.status(self.tmp / "out").ready(censored.GERMAN))

        r = censored.set_censored(self.tmp / "out", True, self.log)
        self.assertEqual((r["moved"], r["lacking"], r["changed"]), (2, [], []))
        self.assertEqual(self.live("sp/lvl/chars.cod"), GERMAN["sp/lvl/chars.cod"])
        self.assertEqual(self.live("sp/lvl/lvl.cod"), UNCUT["sp/lvl/lvl.cod"])     # the text stays
        self.assertEqual(self.live("mp/x.cod"), UNCUT["mp/x.cod"])                 # multiplayer too
        for deep in (False, True):
            self.assertTrue(censored.status(self.tmp / "out", deep=deep).censored)
        # the disc check calls them the German disc's, not damaged, and skips the store
        rep = disc_files.check(self.tmp / "out")
        st = {r.path: r.status for r in rep.rows}
        self.assertEqual(st["sp/lvl/chars.cod"], "other_disc")
        self.assertEqual(st["sp/lvl/zn01.cod"], "other_disc")
        self.assertFalse(rep.problems)
        self.assertFalse(any(p.startswith("censored/") for p in st))

        r = censored.set_censored(self.tmp / "out", False, self.log)
        self.assertEqual(r["moved"], 2)
        self.assertEqual(self.live("sp/lvl/zn01.cod"), UNCUT["sp/lvl/zn01.cod"])
        self.assertFalse(censored.status(self.tmp / "out", deep=True).censored)
        self.assertTrue((self.game / "censored/german/sp/lvl/zn01.cod").is_file())
        self.assertFalse(disc_files.check(self.tmp / "out").problems)

    def test_half_way_and_rerip(self):
        censored.add(self.german_disc, self.tmp / "out", self.log)
        censored.set_censored(self.tmp / "out", True, self.log)
        # a switch back that stopped after taking a file out
        shutil.move(str(self.game / "sp/lvl/chars.cod"), str(self.game / "censored/german/sp/lvl/chars.cod"))
        r = censored.set_censored(self.tmp / "out", False, self.log)
        self.assertEqual(r["moved"], 2)
        self.assertEqual(self.live("sp/lvl/chars.cod"), UNCUT["sp/lvl/chars.cod"])
        # taken off the disc again while on: the German copy is gone, nothing is lost
        censored.set_censored(self.tmp / "out", True, self.log)
        (self.game / "sp/lvl/chars.cod").write_bytes(UNCUT["sp/lvl/chars.cod"])
        r = censored.set_censored(self.tmp / "out", False, self.log)
        self.assertEqual((r["moved"], r["kept"]), (1, 1))
        r = censored.set_censored(self.tmp / "out", True, self.log)
        self.assertEqual(r["lacking"], ["sp/lvl/chars.cod"])
        self.assertEqual(censored.add(self.german_disc, self.tmp / "out", self.log)["copied"], 1)
        self.assertEqual(censored.set_censored(self.tmp / "out", True, self.log)["moved"], 1)

    def test_changed_by_hand_is_left(self):
        censored.add(self.german_disc, self.tmp / "out", self.log)
        (self.game / "sp/lvl/chars.cod").write_bytes(b"X" * 64)
        r = censored.set_censored(self.tmp / "out", True, self.log)
        self.assertEqual(r["changed"], ["sp/lvl/chars.cod"])
        self.assertEqual(self.live("sp/lvl/chars.cod"), b"X" * 64)

    def test_links_are_never_followed(self):
        """[security] A link where a game file or a spare copy should be is left alone,
        and nothing is written through one."""
        outside = self.tmp / "outside.bin"
        outside.write_bytes(b"keep me")
        censored.add(self.german_disc, self.tmp / "out", self.log)
        chars = self.game / "sp/lvl/chars.cod"
        chars.unlink()
        chars.symlink_to(outside)
        r = censored.set_censored(self.tmp / "out", True, self.log)
        self.assertIn("sp/lvl/chars.cod", r["changed"])
        self.assertTrue(chars.is_symlink())
        self.assertEqual(outside.read_bytes(), b"keep me")
        # a half-done copy that is a link: removed, not written through
        game = self.make("de2/assets", "germany")
        dpart = game / "censored/uncut/sp/lvl/zn01.cod.part"
        dpart.parent.mkdir(parents=True, exist_ok=True)
        dpart.symlink_to(outside)
        censored.add(self.usa_disc, self.tmp / "de2", self.log)
        self.assertEqual(outside.read_bytes(), b"keep me")
        self.assertEqual((game / "censored/uncut/sp/lvl/zn01.cod").read_bytes(), UNCUT["sp/lvl/zn01.cod"])

    def test_linked_store_folder_is_refused(self):
        """[security] assets/censored/ linked elsewhere: nothing is copied out there."""
        away = self.tmp / "away"
        away.mkdir()
        (self.game / "censored").symlink_to(away, target_is_directory=True)
        with self.assertRaises(SetupError):
            censored.add(self.german_disc, self.tmp / "out", self.log)
        self.assertEqual(list(away.rglob("*")), [])

    def test_safe_rel(self):
        for ok in ("sp/a/b.cod", "SP/x.cod"):
            self.assertTrue(censored._safe_rel(ok), ok)
        for bad in ("../sp/a.cod", "sp/../../x.cod", "/sp/a.cod", "mp/a.cod", "sp\\a.cod", "sp/c:/a.cod", "sp//a.cod"):
            self.assertFalse(censored._safe_rel(bad), bad)

    def test_damaged_disc(self):
        (self.german_disc / "sp/lvl/chars.cod").write_bytes(b"H" * 64)
        with self.assertRaises(SetupError):
            censored.add(self.german_disc, self.tmp / "out", self.log)
        self.assertFalse(list((self.game / "censored").rglob("*.cod")))
        self.assertFalse(list((self.game / "censored").rglob("*.part")))

    def test_german_game_gets_uncut(self):
        game = self.make("de_out/assets", "germany")
        with self.assertRaises(SetupError):             # its own disc has nothing new
            censored.add(self.german_disc, self.tmp / "de_out", self.log)
        self.assertEqual(censored.add(self.usa_disc, self.tmp / "de_out", self.log)["copied"], 2)
        self.assertTrue(censored.status(self.tmp / "de_out").censored)
        censored.set_censored(self.tmp / "de_out", False, self.log)
        self.assertEqual((game / "sp/lvl/chars.cod").read_bytes(), UNCUT["sp/lvl/chars.cod"])
        self.assertEqual((game / "sp/lvl/lvl.cod").read_bytes(), GERMAN["sp/lvl/lvl.cod"])

    def test_japan_refused(self):
        self.make("jp_out/assets", "japan")
        with self.assertRaises(SetupError):
            censored.status(self.tmp / "jp_out")
        with self.assertRaises(SetupError):
            censored.add(self.german_disc, self.tmp / "jp_out", self.log)

    def test_cli(self):
        import contextlib, io
        censored.add(self.german_disc, self.tmp / "out", self.log)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(censored.main([str(self.tmp / "out"), "--on"]), 0)
        self.assertTrue(censored.status(self.tmp / "out").censored)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(censored.main([str(self.tmp / "out"), "--off", "--status"]), 0)
        self.assertFalse(censored.status(self.tmp / "out").censored)


if __name__ == "__main__":
    unittest.main()

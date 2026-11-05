"""[acct2-decomp] Tests for c3w/disc_files.py: the discs' MD5 lists and the
check of a game folder against them. Made-up files only; no game data.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import hashlib
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import disc_files, repair  # noqa: E402
from c3w.pipeline import KNOWN_XEX  # noqa: E402


def _md5(b: bytes) -> str:
    return hashlib.md5(b).hexdigest()


class ShippedLists(unittest.TestCase):
    def test_every_disc_has_a_list(self):
        for d in disc_files.DISCS:
            files = disc_files.known_files(d)
            self.assertGreater(len(files), 500, d.key)
            self.assertIn("default.xex", files)
            if d.key != "japan":            # the Japanese build isn't one the build accepts
                self.assertIn(d.xex_sha1, KNOWN_XEX)
                self.assertEqual(KNOWN_XEX[d.xex_sha1], d.name)
            for k in files.values():
                self.assertRegex(k.md5, r"^[0-9a-f]{32}$")
                self.assertGreaterEqual(k.size, 0)
                self.assertFalse(k.path.startswith("/") or ".." in k.path.split("/"), k.path)

    def test_counts(self):
        self.assertEqual(len(disc_files.known_files(disc_files.disc_by_key("usa_europe"))), 553)
        self.assertEqual(len(disc_files.known_files(disc_files.disc_by_key("europe_fr_es_it"))), 638)
        self.assertEqual(len(disc_files.known_files(disc_files.disc_by_key("germany"))), 551)
        self.assertEqual(len(disc_files.known_files(disc_files.disc_by_key("japan"))), 567)


class Check(unittest.TestCase):
    """A made-up disc of three files, and a second disc with one more."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.files = {"default.xex": b"XEX2 made up", "sp/a.cod": b"a" * 100, "mp/b.cod": b"b" * 50}
        self.extra_other = ("sp/voice_fr.wfr", b"bonjour")
        lists = self.tmp / "lists"
        lists.mkdir()
        (lists / "one.tsv").write_text("md5\tsize\tpath\n" + "".join(
            f"{_md5(b)}\t{len(b)}\t{p}\n" for p, b in self.files.items()), encoding="utf-8")
        p, b = self.extra_other
        (lists / "two.tsv").write_text(f"md5\tsize\tpath\n{_md5(b)}\t{len(b)}\t{p}\n", encoding="utf-8")
        self.saved = (disc_files.DATA_DIR, disc_files.DISCS, dict(disc_files._lists))
        disc_files.DATA_DIR = lists
        xex_sha1 = hashlib.sha1(self.files["default.xex"]).hexdigest()
        disc_files.DISCS = (disc_files.Disc("one", "Disc one", xex_sha1),
                            disc_files.Disc("two", "Disc two", "0" * 40))
        disc_files._lists.clear()
        self.game = self.tmp / "out" / "assets"
        for rel, b in self.files.items():
            (self.game / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.game / rel).write_bytes(b)

    def tearDown(self):
        disc_files.DATA_DIR, disc_files.DISCS, lists = self.saved
        disc_files._lists.clear()
        disc_files._lists.update(lists)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def status(self, report):
        return {r.path: r.status for r in report.rows}

    def test_all_same(self):
        r = disc_files.check(self.tmp / "out")          # the output folder: its assets/ is used
        self.assertEqual(r.disc.key, "one")
        self.assertEqual(set(self.status(r).values()), {"ok"})
        self.assertFalse(r.problems)
        self.assertIn("3 of 3", r.summary())

    def test_changed_cut_missing_extra_other(self):
        (self.game / "sp/a.cod").write_bytes(b"A" * 100)            # same size, other bytes
        (self.game / "mp/b.cod").write_bytes(b"b" * 10)             # cut off
        (self.game / "sp/new.txt").write_bytes(b"mine")             # not on a disc
        p, b = self.extra_other
        (self.game / p).write_bytes(b)                              # from disc two
        (self.game / "default.xex").rename(self.game / "default.xex.keep")
        r = disc_files.check(self.game, disc_files.disc_by_key("one"))
        st = self.status(r)
        self.assertEqual(st["sp/a.cod"], "changed")
        self.assertEqual(st["mp/b.cod"], "size")
        self.assertEqual(st["default.xex"], "missing")
        self.assertEqual(st["sp/new.txt"], "extra")
        self.assertEqual(st[p], "other_disc")
        self.assertEqual(len(r.problems), 3)

    def test_quick_sees_sizes_only(self):
        (self.game / "sp/a.cod").write_bytes(b"A" * 100)
        (self.game / "mp/b.cod").write_bytes(b"b" * 10)
        st = self.status(disc_files.check(self.game, deep=False))
        self.assertEqual(st["sp/a.cod"], "ok")      # same size: a quick check can't tell
        self.assertEqual(st["mp/b.cod"], "size")

    def test_unknown_disc(self):
        (self.game / "default.xex").write_bytes(b"another game")
        r = disc_files.check(self.game)
        self.assertIsNone(r.disc)
        self.assertEqual(r.rows, [])
        self.assertIn("not a known", r.summary())

    def test_tsv(self):
        (self.game / "mp/b.cod").unlink()
        r = disc_files.check(self.game)
        out = self.tmp / "r.tsv"
        disc_files.write_tsv(r, out)
        lines = out.read_text(encoding="utf-8").splitlines()
        self.assertTrue(lines[0].startswith("# Disc one"))
        self.assertIn("missing\t\t" + _md5(self.files["mp/b.cod"]) + "\t\t50\tmp/b.cod", lines)

    def test_repair_reports_sizes(self):
        (self.game / "mp/b.cod").write_bytes(b"b" * 10)
        report = repair.Report(self.tmp / "out")
        repair._check_assets(report)
        hits = [i for i in report.issues if "differ from the disc" in i.title]
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0].fix, "rerip")
        self.assertIn("mp/b.cod", hits[0].detail)

    def test_bad_list_line(self):
        bad = self.tmp / "bad.tsv"
        bad.write_text("md5\tsize\tpath\nnot-a-line\n", encoding="utf-8")
        with self.assertRaises(disc_files.SetupError):
            disc_files.load_list(bad)


if __name__ == "__main__":
    unittest.main()

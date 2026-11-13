"""Tests for the Big Red One discs the setup tool builds from (c3w/bro.py, [acct2-ocr] 2026-11-12, the owner 18:07:
"we need to make sure the setup tool has the MD5s of the discs that can be used"): the four default.xbe MD5s, each
disc's list of files (c3w/disc_md5/bro_*.tsv), the check of the copied game files against it, and the copy made
beside the recompile (GameCopy). No Tkinter, no network, nothing built, no game files: the lists hold names, sizes
and checksums, and the folders here are made up.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import hashlib
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from c3w import bro, disc_files  # noqa: E402
from c3w.common import Cancelled, Log, SetupError  # noqa: E402
from test_bro import fake_xbe  # noqa: E402


def _md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def _capture():
    seen = []
    return Log(sinks=[lambda kind, text, frac: seen.append((kind, text))]), seen


class KnownDiscsTests(unittest.TestCase):
    def test_four_discs_each_with_a_list(self):
        self.assertEqual(len(bro.KNOWN_XBE), 4)
        self.assertEqual(set(bro.KNOWN_XBE), set(bro.DISC_LISTS))
        for md5, name in bro.KNOWN_XBE.items():
            self.assertRegex(md5, r"^[0-9a-f]{32}$")
            self.assertIn("Big Red One", name)
        self.assertIn("the disc the port was made from", bro.KNOWN_XBE["3b50372b121dc4e51b2b3dbda7cba19d"])

    def test_each_list_names_its_own_default_xbe(self):
        """A list belongs to its disc: the default.xbe in it has the MD5 the disc is known by."""
        for md5, key in bro.DISC_LISTS.items():
            path = bro.DISC_MD5_DIR / f"{key}.tsv"
            self.assertTrue(path.is_file(), path)
            files = bro.disc_list(md5)
            self.assertGreater(len(files), 600, key)
            self.assertEqual(files["default.xbe"].md5, md5, key)
            self.assertGreater(files["default.xbe"].size, 0)

    def test_lists_hold_only_names_sizes_and_checksums(self):
        for key in bro.DISC_LISTS.values():
            lines = (bro.DISC_MD5_DIR / f"{key}.tsv").read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[0], "md5\tsize\tpath")
            for line in lines[1:]:
                md5, size, rel = line.split("\t")
                self.assertRegex(md5, r"^[0-9a-f]{32}$")
                self.assertTrue(size.isdigit())
                self.assertFalse(rel.startswith("/") or "\\" in rel or ".." in rel.split("/"), rel)

    def test_lists_are_not_call_of_duty_3s(self):
        """The Call of Duty 3 lists in the same folder are told by default.xex; these don't touch them."""
        keys = {d.key for d in disc_files.DISCS}
        self.assertFalse(keys & set(bro.DISC_LISTS.values()))

    def test_a_known_disc_is_found(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp, "disc")
            folder.mkdir()
            data = fake_xbe()
            (folder / "default.xbe").write_bytes(data)
            with mock.patch.dict(bro.KNOWN_XBE, {_md5(data): bro.KNOWN_XBE["43293b5fb91f7f3595a87af2a652474f"]}):
                disc, line = bro.check_disc(str(folder))
            self.assertIsNotNone(disc)
            self.assertIn("Germany", line)

    def test_another_version_is_named_by_its_md5(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp, "disc")
            folder.mkdir()
            data = fake_xbe(version=99)
            (folder / "default.xbe").write_bytes(data)
            disc, line = bro.check_disc(str(folder))
            self.assertIsNone(disc)
            self.assertIn(_md5(data), line)
            self.assertIn("report", line)


class CheckGameFilesTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.lists = root / "lists"
        self.lists.mkdir()
        self.game = root / "game"
        (self.game / "sp").mkdir(parents=True)
        self.xbe = fake_xbe()
        self.files = {"default.xbe": self.xbe, "sp/level.ff": b"level" * 100, "Media/Movie.bik": b"\x01\x02\x03"}
        rows = ["md5\tsize\tpath"] + [f"{_md5(b)}\t{len(b)}\t{p}" for p, b in self.files.items()]
        (self.lists / "bro_test.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
        for p, b in self.files.items():
            out = self.game / p
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(b)
        self.disc = bro.Disc("folder", self.game, _md5(self.xbe), bro.xbe_info(self.xbe), 3, 0)
        patches = [mock.patch.object(bro, "DISC_MD5_DIR", self.lists),
                   mock.patch.dict(bro.DISC_LISTS, {self.disc.xbe_md5: "bro_test"})]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self._tmp.cleanup()

    def test_all_the_same(self):
        check = bro.check_game_files(self.disc, self.game)
        self.assertEqual((check.listed, check.same, check.problems), (3, 3, []))

    def test_paths_match_whatever_their_case(self):
        (self.game / "Media" / "Movie.bik").rename(self.game / "Media" / "MOVIE.BIK")
        self.assertEqual(bro.check_game_files(self.disc, self.game).problems, [])

    def test_missing_cut_off_and_changed(self):
        (self.game / "Media" / "Movie.bik").unlink()
        (self.game / "sp" / "level.ff").write_bytes(b"level")
        (self.game / "default.xbe").write_bytes(bytes(len(self.xbe)))
        check = bro.check_game_files(self.disc, self.game)
        self.assertEqual(check.same, 0)
        text = "\n".join(check.problems)
        self.assertIn("missing: Media/Movie.bik", text)
        self.assertIn("another size (5 bytes, the disc's has 500): sp/level.ff", text)
        self.assertIn("changed or damaged (another MD5): default.xbe", text)

    def test_extra_files_are_not_problems(self):
        (self.game / "saves.txt").write_text("mine", encoding="utf-8")
        self.assertEqual(bro.check_game_files(self.disc, self.game).problems, [])

    def test_a_disc_without_a_list(self):
        with mock.patch.dict(bro.DISC_LISTS, {}, clear=True):
            self.assertIsNone(bro.check_game_files(self.disc, self.game))

    def test_no_game_folder_yet(self):
        check = bro.check_game_files(self.disc, self.game.parent / "nothing")
        self.assertEqual((check.same, len(check.problems)), (0, 3))

    def test_cancel(self):
        stop = threading.Event()
        stop.set()
        with self.assertRaises(Cancelled):
            bro.check_game_files(self.disc, self.game, stop)

    def test_report(self):
        log, seen = _capture()
        bro.report_game_files(bro.check_game_files(self.disc, self.game), log, self.game)
        self.assertEqual([k for k, _ in seen], ["info"])
        self.assertIn("all 3 are the same", seen[0][1])
        (self.game / "sp" / "level.ff").unlink()
        log, seen = _capture()
        bro.report_game_files(bro.check_game_files(self.disc, self.game), log, self.game)
        self.assertTrue(seen and all(k == "warn" for k, _ in seen))
        self.assertIn("1 of the 3", seen[0][1])
        self.assertIn(str(self.game), seen[0][1])
        self.assertIn("missing: sp/level.ff", seen[1][1])

    def test_report_shows_ten_and_counts_the_rest(self):
        check = bro.FilesCheck("A disc", 30, 5, [f"missing: f{i}" for i in range(25)])
        log, seen = _capture()
        bro.report_game_files(check, log, self.game)
        self.assertEqual(len(seen), 12)
        self.assertIn("and 15 more", seen[-1][1])


class GameCopyTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.src = root / "disc"
        (self.src / "sp").mkdir(parents=True)
        self.xbe = fake_xbe()
        (self.src / "default.xbe").write_bytes(self.xbe)
        (self.src / "sp" / "level.ff").write_bytes(b"level" * 1000)
        self.lay = bro.layout(root / "out", root / "work")
        self.disc = bro.open_disc(str(self.src))
        lists = root / "lists"
        lists.mkdir()
        rows = ["md5\tsize\tpath", f"{_md5(self.xbe)}\t{len(self.xbe)}\tdefault.xbe",
                f"{_md5(b'level' * 1000)}\t5000\tsp/level.ff"]
        (lists / "bro_test.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
        patches = [mock.patch.object(bro, "DISC_MD5_DIR", lists),
                   mock.patch.dict(bro.DISC_LISTS, {self.disc.xbe_md5: "bro_test"})]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self._tmp.cleanup()

    def test_copies_then_checks(self):
        log, seen = _capture()
        copier = bro.GameCopy(self.disc, self.lay, log, threading.Event()).start()
        copier.finish()
        self.assertEqual((self.lay.game / "sp" / "level.ff").read_bytes(), b"level" * 1000)
        text = "\n".join(t for _, t in seen)
        self.assertIn("Copied and checked the game's files beside the recompile", text)
        self.assertIn("all 2 are the same as on the disc", text)

    def test_a_damaged_copy_is_reported_not_fatal(self):
        (self.src / "sp" / "level.ff").write_bytes(b"LEVEL" * 1000)    # the same size, other bytes
        log, seen = _capture()
        bro.GameCopy(self.disc, self.lay, log, threading.Event()).start().finish()
        warns = [t for k, t in seen if k == "warn"]
        self.assertTrue(warns)
        self.assertIn("changed or damaged (another MD5): sp/level.ff", "\n".join(warns))

    def test_an_error_comes_out_of_finish(self):
        def broken(*args):
            raise SetupError("the disc can't be read")
        log, _ = _capture()
        with mock.patch.object(bro, "copy_files", broken):
            copier = bro.GameCopy(self.disc, self.lay, log, threading.Event()).start()
            with self.assertRaises(SetupError):
                copier.finish()

    def test_stop_ends_it(self):
        started = threading.Event()

        def slow(disc, lay, log, cancel):
            started.set()
            while not cancel.is_set():
                started.wait(0.01)
            raise Cancelled()
        log, _ = _capture()
        with mock.patch.object(bro, "copy_files", slow):
            copier = bro.GameCopy(self.disc, self.lay, log, threading.Event()).start()
            self.assertTrue(started.wait(5))
            copier.stop()
            self.assertFalse(copier._thread.is_alive())

    def test_the_users_cancel_stops_it_too(self):
        cancel = threading.Event()
        cancel.set()
        log, _ = _capture()
        copier = bro.GameCopy(self.disc, self.lay, log, cancel).start()
        with self.assertRaises(Cancelled):
            copier.finish()

    def test_place_xbe_first(self):
        bro.place_xbe(self.disc, self.lay)
        self.assertEqual(self.lay.xbe.read_bytes(), self.xbe)
        self.assertFalse(self.lay.game.exists())     # only default.xbe, the rest comes with GameCopy


if __name__ == "__main__":
    unittest.main()

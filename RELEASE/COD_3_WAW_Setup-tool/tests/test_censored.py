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
sys.path.insert(0, str(Path(__file__).resolve().parent))

import fake_cod  # noqa: E402
from c3w import censored, cod_archive, disc_files, repair  # noqa: E402
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

    def test_the_level_archives(self):
        censored._levels = None
        try:
            lv = censored.levels()
            self.assertEqual(len(lv), 6)
            self.assertEqual(sum(len(n) for n in lv.values()), 23)
            de = disc_files.known_files(disc_files.disc_by_key("germany"))
            for rel, names in lv.items():
                self.assertTrue(censored.has_language(rel) and censored._safe_rel(rel), rel)
                self.assertIn(rel.lower(), de)
                for u, g in names:
                    self.assertIn("of_", u.lower())
                    self.assertEqual(g.lower(), u.lower().replace("of_", "no_", 1))
                    self.assertTrue(u.endswith(".xetex") and g.endswith(".xetex"))
        finally:
            censored._levels = None

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


class _Game(unittest.TestCase):
    """Made-up discs and a game built from the USA/Europe one (no tests of its own)."""
    uncut, german = UNCUT, GERMAN
    level_list = ""                         # lists/censored_level_textures.tsv: none here

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        lists = self.tmp / "lists"
        lists.mkdir()
        self.discs = {"usa_europe": self.uncut, "germany": self.german, "europe_fr_es_it": self.uncut, "japan": {}}
        for key, files in self.discs.items():
            rows = dict(files, **{"default.xex": XEX[key]})
            (lists / f"{key}.tsv").write_text("md5\tsize\tpath\n" + "".join(
                f"{_md5(b)}\t{len(b)}\t{p}\n" for p, b in rows.items()), encoding="utf-8")
        self.saved = (disc_files.DATA_DIR, disc_files.DISCS, dict(disc_files._lists), censored._pairs,
                      censored._levels, censored.level_list_path)
        disc_files.DATA_DIR = lists
        disc_files.DISCS = tuple(disc_files.Disc(k, f"Disc {k}", _sha1(XEX[k])) for k in self.discs)
        disc_files._lists.clear()
        censored._pairs = censored._levels = None
        level_list = self.tmp / "levels.tsv"
        level_list.write_text(self.level_list, encoding="utf-8")
        censored.level_list_path = lambda: level_list
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
        (disc_files.DATA_DIR, disc_files.DISCS, lists, censored._pairs, censored._levels,
         censored.level_list_path) = self.saved
        disc_files._lists.clear()
        disc_files._lists.update(lists)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def live(self, rel: str) -> bytes:
        return (self.game / rel).read_bytes()


class Swap(_Game):
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


LEVEL = "sp/lvl/lvl.cod"
U_FLAG = fake_cod.texture(0x11, 0x400, addr=0x200)
G_FLAG = fake_cod.texture(0x99, 0x400, addr=0x380)
U_LEVEL = fake_cod.level(b"english text", U_FLAG, "of_flag_c.xetex")
G_LEVEL = fake_cod.level(b"deutscher text", G_FLAG, "no_flag_c.xetex", extra_texture=True)   # laid out otherwise


def flag_of(archive: bytes):
    """(object, GPU data, the bank) of the made-up level's flag texture."""
    ch = cod_archive.unpack(archive, cod_archive.read_index(archive).streamed().chunks[0])
    it = next(i for i in cod_archive.apkf_items(ch) if i.name == "flag_c")
    (o, n), (g, m) = it.parts
    return ch[o:o + n], ch[g:g + m], ch


class Levels(_Game):
    """A level archive (it carries the language too): a copy of the game's own file with the other
    release's pixels at the same items, switched like the others."""
    uncut = dict(UNCUT, **{LEVEL: U_LEVEL})
    german = dict(GERMAN, **{LEVEL: G_LEVEL})
    level_list = ("# made up\n" + f"{LEVEL}\tof_flag_c.xetex\tno_flag_c.xetex\n"
                  "sp/lvl/chars.cod\tof_flag_c.xetex\tno_flag_c.xetex\n"      # not a level archive: ignored
                  "sp/none/none.cod\tof_a.xetex\tno_a.xetex\n"                # not on the discs: ignored
                  "../sp/lvl/lvl.cod\tof_a.xetex\tno_a.xetex\n")              # not a safe path: ignored

    def test_list(self):
        self.assertEqual(censored.levels(), {LEVEL: [("of_flag_c.xetex", "no_flag_c.xetex")]})

    def test_add_on_off(self):
        out = self.tmp / "out"
        st = censored.status(out)
        self.assertEqual((st.total, st.censored, st.ready(censored.GERMAN)), (3, False, False))
        r = censored.add(self.german_disc, out, self.log)
        self.assertEqual((r["copied"], r["levels"], r["levels_skipped"]), (2, 1, []))
        made = (self.game / "censored/german" / LEVEL).read_bytes()
        obj, gpu, bank = flag_of(made)
        self.assertEqual((obj, gpu), (U_FLAG[0], G_FLAG[1]))        # our texture object, their pixels
        self.assertIn(b"english text", bank)                         # our language
        self.assertEqual(self.live(LEVEL), U_LEVEL)                  # nothing in use changed yet
        row = censored._read_levels(self.game / "censored")[LEVEL]
        self.assertEqual((row.own, row.orig, row.made), (censored.UNCUT, censored.Made(len(U_LEVEL), _md5(U_LEVEL)),
                                                         censored.Made(len(made), _md5(made))))
        r = censored.add(self.german_disc, out, self.log)           # again: nothing new
        self.assertEqual((r["copied"], r["levels"], r["levels_ready"]), (0, 0, 1))
        self.assertTrue(censored.status(out).ready(censored.GERMAN))

        r = censored.set_censored(out, True, self.log)
        self.assertEqual((r["moved"], r["lacking"], r["changed"]), (3, [], []))
        self.assertEqual(self.live(LEVEL), made)
        self.assertEqual((self.game / "censored/uncut" / LEVEL).read_bytes(), U_LEVEL)
        for deep in (False, True):
            self.assertTrue(censored.status(out, deep=deep).censored)
            rep = disc_files.check(out, deep=deep)
            self.assertFalse(rep.problems, deep)
        self.assertEqual({r.path: r.status for r in rep.rows}[LEVEL], "censored")
        self.assertIn("1 made by the censored campaign", rep.summary())
        report = repair.Report(out)
        repair._check_assets(report)
        self.assertFalse([i for i in report.issues if "differ from the disc" in i.title])

        r = censored.set_censored(out, False, self.log)
        self.assertEqual(r["moved"], 3)
        self.assertEqual(self.live(LEVEL), U_LEVEL)                  # the game's own file, byte for byte
        self.assertFalse(censored.status(out, deep=True).censored)
        self.assertEqual({r.path: r.status for r in disc_files.check(out).rows}[LEVEL], "ok")

    def test_taken_off_the_disc_again(self):
        out = self.tmp / "out"
        censored.add(self.german_disc, out, self.log)
        censored.set_censored(out, True, self.log)
        (self.game / LEVEL).write_bytes(U_LEVEL)                    # the copy in use is gone
        st = censored.status(out, deep=True)
        self.assertEqual((st.in_use.get(censored.UNCUT), st.ready(censored.GERMAN)), (1, False))
        r = censored.set_censored(out, True, self.log)
        self.assertEqual((r["moved"], r["kept"], r["lacking"]), (0, 2, [LEVEL]))
        self.assertEqual(censored.add(self.german_disc, out, self.log)["levels"], 1)    # made again
        self.assertEqual(censored.set_censored(out, True, self.log)["moved"], 1)
        self.assertTrue(censored.status(out, deep=True).censored)

    def test_german_game_gets_uncut(self):
        game = self.make("de_out/assets", "germany")
        out = self.tmp / "de_out"
        r = censored.add(self.usa_disc, out, self.log)
        self.assertEqual((r["copied"], r["levels"]), (2, 1))
        self.assertTrue(censored.status(out, deep=True).censored)
        censored.set_censored(out, False, self.log)
        obj, gpu, bank = flag_of((game / LEVEL).read_bytes())
        self.assertEqual((obj, gpu), (G_FLAG[0], U_FLAG[1]))
        self.assertIn(b"deutscher text", bank)
        self.assertFalse(disc_files.check(out).problems)
        censored.set_censored(out, True, self.log)
        self.assertEqual((game / LEVEL).read_bytes(), G_LEVEL)

    def test_changed_by_hand_is_left(self):
        (self.game / LEVEL).write_bytes(b"my own level")
        r = censored.add(self.german_disc, self.tmp / "out", self.log)
        self.assertEqual(r["levels"], 0)
        self.assertIn("changed by hand", r["levels_skipped"][0])
        self.assertFalse((self.game / "censored/german" / LEVEL).exists())
        r = censored.set_censored(self.tmp / "out", True, self.log)
        self.assertEqual((r["moved"], r["lacking"]), (2, [LEVEL]))
        self.assertEqual(self.live(LEVEL), b"my own level")

    def test_texture_that_doesnt_fit_is_not_copied(self):
        other = fake_cod.level(b"deutscher text", fake_cod.texture(0x99, 0x800), "no_flag_c.xetex")
        (self.german_disc / LEVEL).write_bytes(other)
        disc_files._lists.clear()
        (self.tmp / "lists/germany.tsv").write_text(
            (self.tmp / "lists/germany.tsv").read_text().replace(f"{_md5(G_LEVEL)}\t{len(G_LEVEL)}",
                                                                 f"{_md5(other)}\t{len(other)}"))
        r = censored.add(self.german_disc, self.tmp / "out", self.log)
        self.assertEqual(r["levels"], 0)
        self.assertIn("none of its textures", r["levels_skipped"][0])

    def test_damaged_disc(self):
        (self.german_disc / LEVEL).write_bytes(G_LEVEL[:-1] + b"\1")
        with self.assertRaises(SetupError):
            censored.add(self.german_disc, self.tmp / "out", self.log)
        self.assertFalse((self.game / "censored/german" / LEVEL).exists())
        self.assertFalse(list((self.game / "censored").rglob("*.part")))
        self.assertEqual(self.live(LEVEL), U_LEVEL)

    def test_links_are_never_followed(self):
        """[security] A linked level file is skipped; a half-done copy that is a link is removed,
        never written through."""
        outside = self.tmp / "outside.bin"
        outside.write_bytes(b"keep me")
        part = self.game / "censored/german" / (LEVEL + ".part")
        part.parent.mkdir(parents=True)
        part.symlink_to(outside)
        self.assertEqual(censored.add(self.german_disc, self.tmp / "out", self.log)["levels"], 1)
        self.assertEqual(outside.read_bytes(), b"keep me")
        self.assertFalse(part.exists() or part.is_symlink())
        game = self.make("out2/assets", "usa_europe")
        (game / LEVEL).unlink()
        (game / LEVEL).symlink_to(outside)
        r = censored.add(self.german_disc, self.tmp / "out2", self.log)
        self.assertEqual(r["levels"], 0)
        self.assertIn("a link", r["levels_skipped"][0])
        manifest = self.game / "censored" / censored.LEVELS_MANIFEST
        manifest.unlink()
        manifest.symlink_to(outside)
        (self.game / "censored/german" / LEVEL).unlink()
        with self.assertRaises(SetupError):
            censored.add(self.german_disc, self.tmp / "out", self.log)
        self.assertEqual(outside.read_bytes(), b"keep me")


if __name__ == "__main__":
    unittest.main()

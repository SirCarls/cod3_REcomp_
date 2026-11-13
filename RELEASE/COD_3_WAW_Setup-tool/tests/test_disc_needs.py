"""[acct2-decomp] Tests for c3w/disc_needs.py: which options need which Call of Duty 3 disc, and
where a game stands with each. Made-up files only; no game data.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import contextlib
import hashlib
import io
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import censored, disc_files, disc_needs as dn, inputs  # noqa: E402
from c3w.common import SetupError  # noqa: E402

XEX = {"usa_europe": b"XEX2 usa", "germany": b"XEX2 de", "europe_fr_es_it": b"XEX2 fr", "japan": b"XEX2 jp"}
SHA = {k: hashlib.sha1(v).hexdigest() for k, v in XEX.items()}
FILES = {"usa_europe": {"sp/lvl/chars.cod": b"U" * 32, "sp/lvl/lvl.wbk": b"english"},
         "germany": {"sp/lvl/chars.cod": b"G" * 32, "sp/lvl/lvl.wde": b"deutsch"},
         "europe_fr_es_it": {"sp/lvl/chars.cod": b"U" * 32, "sp/lvl/sound/lvl.wfr": b"fr",
                             "sp/lvl/sound/lvl.wes": b"es", "sp/lvl/sound/lvl.wit": b"it"},
         "japan": {"sp/lvl/chars.cod": b"J" * 32, "sp/lvl/lvl.wbk": b"english jp"}}


def _md5(b: bytes) -> str:
    return hashlib.md5(b).hexdigest()


class DiscNeeds(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        lists = self.tmp / "lists"
        lists.mkdir()
        for key, files in FILES.items():
            rows = dict(files, **{"default.xex": XEX[key]})
            (lists / f"{key}.tsv").write_text("md5\tsize\tpath\n" + "".join(
                f"{_md5(b)}\t{len(b)}\t{p}\n" for p, b in rows.items()), encoding="utf-8")
        self.saved = (disc_files.DATA_DIR, disc_files.DISCS, dict(disc_files._lists), censored._pairs,
                      censored._levels, censored.level_list_path)
        disc_files.DATA_DIR = lists
        disc_files.DISCS = tuple(disc_files.Disc(k, f"Disc {k}", SHA[k]) for k in FILES)
        disc_files._lists.clear()
        censored._pairs = censored._levels = None
        empty = self.tmp / "levels.tsv"
        empty.write_text("", encoding="utf-8")
        censored.level_list_path = lambda: empty
        dn._seen.clear()

    def tearDown(self):
        (disc_files.DATA_DIR, disc_files.DISCS, lists, censored._pairs, censored._levels,
         censored.level_list_path) = self.saved
        disc_files._lists.clear()
        disc_files._lists.update(lists)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def game(self, key: str, name: str = "out") -> Path:
        root = self.tmp / name
        for p, b in dict(FILES[key], **{"default.xex": XEX[key]}).items():
            (root / "assets" / p).parent.mkdir(parents=True, exist_ok=True)
            (root / "assets" / p).write_bytes(b)
        return root

    def lines(self, out, found=None):
        return {ln.option: ln for ln in dn.lines(out, found=found or {})}

    # -- telling a disc ------------------------------------------------------------------------
    def test_identify(self):
        self.assertEqual(dn.identify(self.game("germany")).key, "germany")
        self.assertEqual(dn.identify(self.game("usa_europe", "us") / "assets").key, "usa_europe")
        (self.tmp / "empty").mkdir()
        (self.tmp / "other").mkdir()
        (self.tmp / "other" / "default.xex").write_bytes(b"another game")
        (self.tmp / "not.iso").write_bytes(b"not an image" * 100)
        for bad, why in ((self.tmp / "empty", "no default.xex"), (self.tmp / "other", "this tool knows"),
                         (self.tmp / "not.iso", "isn't an Xbox 360 disc image"), (self.tmp / "gone.iso", "isn't there")):
            with self.assertRaises(SetupError) as e:
                dn.identify(bad)
            self.assertIn(why, str(e.exception))

    def test_check_choice(self):
        us = self.game("usa_europe")
        self.assertEqual(dn.check_choice(us, (dn.USA, dn.FR_ES_IT), "Switching it off").key, "usa_europe")
        with self.assertRaises(SetupError) as e:
            dn.check_choice(us, (dn.GERMANY,), "The censored campaign")
        self.assertIn("is the USA/Europe disc", str(e.exception))
        self.assertIn("The censored campaign needs the German disc", str(e.exception))

    def test_game_disc(self):
        self.assertIsNone(dn.game_disc(None))
        self.assertIsNone(dn.game_disc(self.tmp / "nothing"))
        self.assertEqual(dn.game_disc(self.game("europe_fr_es_it")).key, "europe_fr_es_it")

    def test_found_discs(self):
        inp = self.tmp / "input"
        (inp / "sub").mkdir(parents=True)
        for name in ("a_de.iso", "sub/b_us.iso", "c_de.iso", "notes.txt", "d_bad.iso"):
            (inp / name).write_bytes(b"x")
        by_name = {"a_de.iso": "germany", "b_us.iso": "usa_europe", "c_de.iso": "germany"}

        def fake(p):
            if p.name not in by_name:
                raise SetupError("not a disc")
            return disc_files.disc_by_key(by_name[p.name])
        with mock.patch.object(dn, "identify", side_effect=fake):
            found = dn.found_discs(inp)
        self.assertEqual({k: p.name for k, p in found.items()}, {"germany": "a_de.iso", "usa_europe": "b_us.iso"})
        self.assertEqual(dn.found_discs(None), {})
        self.assertEqual(dn.found_discs(self.tmp / "no such folder"), {})

    # -- where a game stands -------------------------------------------------------------------
    def test_not_built(self):
        ln = self.lines(self.tmp / "nothing")
        self.assertEqual(list(ln), ["Your game"])
        self.assertIn("not built yet", ln["Your game"].text)
        self.assertFalse(ln["Your game"].ready)

    def test_usa_game(self):
        out = self.game("usa_europe")
        ln = self.lines(out)
        self.assertIn("built from the USA/Europe disc", ln["Your game"].text)
        v = ln["Voices"]
        self.assertFalse(v.ready)
        self.assertIn("in the game: English;", v.text)
        self.assertIn("German needs the German disc", v.text)
        self.assertIn("French, Spanish and Italian need the Europe (Fr, Es, It) disc", v.text)
        self.assertIn("plays the game's own", v.text)
        c = ln["Censored campaign"]
        self.assertFalse(c.ready)
        self.assertIn("needs the German disc once", c.text)
        self.assertTrue(ln["Multiplayer and map packs"].ready)
        self.assertIn("films' sound", ln["Console language"].text)
        self.assertIn("built-in censor", ln["No other disc needed"].text)
        # the German disc in the input folder is named where it helps
        ln = self.lines(out, {dn.GERMANY: self.tmp / "input" / "de.iso"})
        self.assertIn("de.iso is in the input folder", ln["Censored campaign"].text)
        self.assertIn("In the input folder: the German disc", ln["Your game"].text)
        # German voices added; the censored campaign's files added
        (out / "assets/sp/lvl/lvl.wde").write_bytes(b"deutsch")
        (out / "assets/censored/german/sp/lvl").mkdir(parents=True)
        (out / "assets/censored/german/sp/lvl/chars.cod").write_bytes(FILES["germany"]["sp/lvl/chars.cod"])
        ln = self.lines(out)
        self.assertIn("in the game: English and German (added);", ln["Voices"].text)
        self.assertNotIn("German needs", ln["Voices"].text)
        self.assertTrue(ln["Censored campaign"].ready)
        self.assertIn("it switches at once", ln["Censored campaign"].text)

    def test_german_game(self):
        out = self.game("germany")
        ln = self.lines(out, {dn.FR_ES_IT: Path("fr.iso")})
        self.assertIn("English needs the USA/Europe disc", ln["Voices"].text)
        c = ln["Censored campaign"]
        self.assertFalse(c.ready)
        self.assertIn("your game is the German release", c.text)
        self.assertIn("needs the USA/Europe or Europe (Fr, Es, It) disc once (fr.iso is in the input folder)", c.text)
        mp = ln["Multiplayer and map packs"]
        self.assertIsNone(mp.ready)
        self.assertIn("not with the German disc", mp.text)

    def test_fr_es_it_game(self):
        ln = self.lines(self.game("europe_fr_es_it"))
        self.assertIn("in the game: French, Spanish and Italian;", ln["Voices"].text)
        self.assertIn("picks the soldiers' language and the text", ln["Console language"].text)
        self.assertIsNone(ln["Multiplayer and map packs"].ready)

    def test_japanese_files(self):
        ln = self.lines(self.game("japan"))
        self.assertEqual(list(ln), ["Your game"])
        self.assertIn("can't build", ln["Your game"].text)

    def test_one_option_failing_doesnt_hide_the_others(self):
        out = self.game("usa_europe")
        with mock.patch.object(dn, "voice_lines", side_effect=RuntimeError("boom")):
            ln = self.lines(out)
        self.assertIn("can't tell", ln["Voices"].text)
        self.assertIn("Censored campaign", ln)
        self.assertIn("Multiplayer and map packs", ln)

    def test_text_and_cli(self):
        out = self.game("usa_europe")
        self.assertTrue(dn.text(out, found={}).startswith("Your game: built from the USA/Europe disc"))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(dn.main([str(out)]), 0)
        self.assertIn("What needs which disc:", buf.getvalue())
        self.assertIn("[need] Censored campaign", buf.getvalue())
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(dn.main(["--table"]), 0)
        self.assertNotIn("Your game", buf.getvalue())

    # -- Set up's notes ------------------------------------------------------------------------
    def test_setup_note(self):
        note = dn.setup_note(SHA["usa_europe"], {SHA["germany"]: Path("in/de.iso"), SHA["usa_europe"]: Path("us.iso")})
        self.assertTrue(note.startswith("\n"))
        self.assertIn("Builds the campaign and multiplayer", note)
        self.assertIn("Also in the input folder: the German disc (de.iso: the censored campaign and German voices",
                      note)
        self.assertNotIn("us.iso", note)
        self.assertIn("Builds the campaign only (multiplayer needs the USA/Europe disc)",
                      dn.setup_note(SHA["germany"], {}))
        self.assertIn("can't be built", dn.setup_note(SHA["japan"], {}))
        self.assertEqual(dn.setup_note(None, {}), "")
        self.assertEqual(dn.unknown_name(SHA["japan"], "?"), "Disc japan")
        self.assertEqual(dn.unknown_name("0" * 40, "?"), "?")
        self.assertEqual(dn.unknown_name(None, "?"), "?")

    def test_the_table(self):
        self.assertEqual(set(dn.RELEASES), {d.key for d in self.saved[1]})   # the real discs' keys
        self.assertEqual(set(dn.VOICE_DISC), set(dn.LANGUAGE_NAMES))
        for option, where, needs in dn.TABLE:
            self.assertTrue(option and where and needs)
        self.assertEqual([k for k, r in dn.RELEASES.items() if r.multiplayer], [dn.USA])
        self.assertEqual([k for k, r in dn.RELEASES.items() if not r.builds], [dn.JAPAN])


class InputFolder(unittest.TestCase):
    def test_candidates(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            (t / "sub").mkdir()
            (t / "a.iso").write_bytes(b"")
            (t / "sub" / "b.iso").write_bytes(b"")
            (t / "sub" / "deeper").mkdir()
            (t / "sub" / "deeper" / "c.iso").write_bytes(b"")
            self.assertEqual([p.name for p in inputs.candidates(t)], ["a.iso", "b.iso"])
            self.assertEqual(inputs.candidates(t / "none"), [])


if __name__ == "__main__":
    unittest.main()

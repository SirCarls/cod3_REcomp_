"""[acct2-decomp] 2026-11-21 Tests for the censored campaign through a new build into the same output folder
(owner: "Please check the logic on the censoring. Also alternate voices."):

- the files the switch had put in are put aside in assets/censored/ before the game's files are written again
  (c3w/censored.py before_rebuild), and the campaign is switched back as it was when the game is still the same
  version (after_rebuild; Pipeline.cod3_assets calls both);
- Status.ready counts each file once (a file in use that also waits in the store counted twice);
- the built-in censor makes a picture again when its source or its region changed (c3w/censor_builtin.py).

Made-up files only; no game data (the scaffolding is test_censored.py's).

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import shutil
import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_censor_builtin as tcb  # noqa: E402
from test_censored import GERMAN, UNCUT, _Game  # noqa: E402
from c3w import censor_builtin as cb  # noqa: E402
from c3w import censored  # noqa: E402
from c3w.common import Log  # noqa: E402
from c3w.pipeline import BuildOptions, Pipeline  # noqa: E402

CHARS, ZONE = "sp/lvl/chars.cod", "sp/lvl/zn01.cod"   # the same size in both versions, and another size


class Rebuild(_Game):
    """The game in out/ was built from the USA/Europe disc; then it is built again from a disc."""

    def build_from(self, disc: Path) -> list:
        lines = []
        log = Log(sinks=[lambda kind, text, frac: lines.append((kind, text))])
        opts = BuildOptions(cod3_source=str(disc), output_dir=str(self.tmp / "out"), work_dir=str(self.tmp / "work"),
                            build_recomp=False)
        Pipeline(opts, log, threading.Event()).cod3_assets()
        return lines

    def on(self) -> None:
        censored.add(self.german_disc, self.tmp / "out", self.log)
        self.assertEqual(censored.set_censored(self.tmp / "out", True, self.log)["moved"], 2)

    def test_same_disc_keeps_the_campaign_on(self):
        self.on()
        self.build_from(self.usa_disc)
        self.assertEqual(self.live(CHARS), GERMAN[CHARS])
        self.assertEqual(self.live(ZONE), GERMAN[ZONE])      # another size than the disc's: written, then back
        st = censored.status(self.tmp / "out", deep=True)
        self.assertTrue(st.censored)
        self.assertTrue(st.ready(censored.UNCUT))
        self.assertEqual((self.game / "censored/uncut" / ZONE).read_bytes(), UNCUT[ZONE])

    def test_same_disc_keeps_the_campaign_off(self):
        censored.add(self.german_disc, self.tmp / "out", self.log)
        self.build_from(self.usa_disc)
        st = censored.status(self.tmp / "out", deep=True)
        self.assertFalse(st.censored)
        self.assertTrue(st.ready(censored.GERMAN))
        self.assertEqual(self.live(ZONE), UNCUT[ZONE])

    def test_another_uncut_disc_keeps_the_campaign_on(self):
        self.on()
        fr = self.make("fr_disc", "europe_fr_es_it")
        self.build_from(fr)
        self.assertEqual((self.game / "default.xex").read_bytes(), b"XEX2 fr")
        self.assertEqual(self.live(CHARS), GERMAN[CHARS])
        self.assertEqual(self.live(ZONE), GERMAN[ZONE])
        self.assertTrue(censored.status(self.tmp / "out", deep=True).censored)

    def test_german_disc_after_on_has_both_versions_ready(self):
        self.on()
        self.build_from(self.german_disc)
        st = censored.status(self.tmp / "out", deep=True)
        self.assertEqual(st.own, censored.GERMAN)
        self.assertTrue(st.censored)
        self.assertTrue(st.ready(censored.UNCUT))
        self.assertEqual(censored.set_censored(self.tmp / "out", False, self.log)["moved"], 2)
        self.assertEqual(self.live(ZONE), UNCUT[ZONE])

    def test_german_disc_after_off_is_the_discs_own_and_says_so(self):
        censored.add(self.german_disc, self.tmp / "out", self.log)
        lines = self.build_from(self.german_disc)
        st = censored.status(self.tmp / "out", deep=True)
        self.assertTrue(st.censored)                          # the German disc's own textures
        self.assertTrue(st.ready(censored.UNCUT))             # the uncut ones were put aside, not lost
        self.assertEqual((self.game / "censored/uncut" / CHARS).read_bytes(), UNCUT[CHARS])
        self.assertTrue(any(kind == "info" and "its own version now" in text for kind, text in lines))

    def test_nothing_happens_without_the_censored_campaign(self):
        self.build_from(self.usa_disc)
        self.assertFalse((self.game / censored.STORE).exists())
        self.assertIsNone(censored.before_rebuild(self.game, "", False, self.log))

    def test_ready_counts_each_file_once(self):
        self.on()
        # zone: in use as German, and the store has a second German copy; chars: uncut, no German anywhere
        shutil.copyfile(self.game / ZONE, self.game / "censored/german" / ZONE)
        (self.game / CHARS).unlink()
        (self.game / CHARS).write_bytes(UNCUT[CHARS])
        (self.game / "censored/uncut" / CHARS).unlink()
        st = censored.status(self.tmp / "out")
        self.assertFalse(st.ready(censored.GERMAN))
        self.assertTrue(st.ready(censored.UNCUT))


class BuiltinStamp(unittest.TestCase):
    """A picture the built-in censor made is made again when what it is made from changes."""
    setUp = tcb.Apply.setUp          # test_censor_builtin's made-up rip and regions (its tests aren't run again)
    tearDown = tcb.Apply.tearDown
    folder = tcb.Apply.folder

    def test_a_new_source_of_the_same_size_makes_it_again(self):
        cb.apply(self.data, self.tool, cb.Settings(insignia="black"), self.log)
        cb.write_png(self.data / "textures" / "helmet.png", 8, 8, bytearray([200, 10, 10, 255] * 64))
        r = cb.apply(self.data, self.tool, cb.Settings(insignia="black"), self.log)
        self.assertEqual(r.made, ["helmet"])
        _, _, px = cb.read_png(self.folder() / "helmet.png")
        self.assertEqual(bytes(px[0:3]), bytes([200, 10, 10]))   # outside the rectangle: the new source

    def test_a_region_measured_again_makes_it_again(self):
        cb.apply(self.data, self.tool, cb.Settings(insignia="black"), self.log)
        regions = self.tool / "lists" / "censor_regions.tsv"
        old, new = "helmet\t8\t8\tinsignia\t1,1,7,7", "helmet\t8\t8\tinsignia\t2,2,6,6"
        regions.write_text(regions.read_text().replace(old, new))
        r = cb.apply(self.data, self.tool, cb.Settings(insignia="black"), self.log)
        self.assertEqual(r.made, ["helmet"])

    def test_nothing_changed_makes_nothing(self):
        cb.apply(self.data, self.tool, cb.Settings(flags="trans", insignia="grey"), self.log)
        self.assertEqual(cb.apply(self.data, self.tool, cb.Settings(flags="trans", insignia="grey"), self.log).made,
                         [])


if __name__ == "__main__":
    unittest.main()

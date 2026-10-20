"""c3w/fastfiles.py: which game a fastfile comes from. Only made-up headers
here (no game data): the numbers are the ones read from the owner's files
(docs/formats/fastfiles.md)."""
import sys
import tempfile
import threading
import unittest
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from c3w import common, rippers, waw  # noqa: E402
from c3w import fastfiles as ff  # noqa: E402
from test_waw import make_xiso, quiet_log  # noqa: E402

ZLIB = zlib.compress(b"\0" * 64)   # a stream as the games pack zones: 78 xx ...


def header(magic: bytes, version: int, big_endian: bool, rest: bytes = ZLIB) -> bytes:
    return magic + version.to_bytes(4, "big" if big_endian else "little") + rest


class Identify(unittest.TestCase):
    def check(self, data: bytes, game: str, platform: str, signed=False, encrypted=False):
        ident = ff.identify(data)
        self.assertIsNotNone(ident, data[:16])
        self.assertEqual((ident.game, ident.platform, ident.signed, ident.encrypted),
                         (game, platform, signed, encrypted))
        return ident

    def test_pc_games(self):
        self.check(header(b"IWffu100", 5, False), ff.COD4, ff.PC)
        self.check(header(b"IWffu100", 387, False), ff.WAW, ff.PC)
        mw2_rest = b"\x01" + b"\x00" * 8 + ZLIB    # online-update flag, file time, then zlib at 21
        self.check(header(b"IWffu100", 276, False, mw2_rest), ff.MW2, ff.PC)
        self.check(header(b"IWff0100", 276, False, b"\x01" + b"\x00" * 8 + b"IWffs100"), ff.MW2, ff.PC,
                   signed=True)
        self.check(header(b"IWffu100", 473, False), ff.BO1, ff.PC)
        self.check(header(b"IWffu100", 1, False, mw2_rest), ff.MW3, ff.PC)
        self.check(header(b"TAff0100", 147, False, b"PHEEBs71" + b"\0" * 4 + b"weapons"), ff.BO2, ff.PC,
                   encrypted=True)
        self.check(header(b"IWffu100", 565, False, b"\x01\x00\x08\x04" + b"\0" * 28 + ZLIB), ff.GHOSTS, ff.PC)

    def test_byte_order_tells_cod4_console_from_mw3_pc(self):
        # Both are version 1: big-endian on the console, little-endian on PC.
        self.check(header(b"IWffu100", 1, True), ff.COD4, ff.XBOX360)
        self.check(header(b"IWffu100", 1, False), ff.MW3, ff.PC)

    def test_xbox360(self):
        ident = self.check(header(b"IWffu100", 387, True), ff.WAW, ff.XBOX360)
        self.assertEqual(ident.describe(), "Call of Duty: World at War (Xbox 360, version 387)")
        self.check(header(b"IWff0100", 387, True, b"IWffs100" + b"\0" * 4), ff.WAW, ff.XBOX360, signed=True)
        self.check(header(b"IWff0100", 1, True, b"IWffs100"), ff.COD4, ff.XBOX360, signed=True)
        # Modern Warfare 2: zlib after 25 more bytes when it lists no extra entries.
        self.check(header(b"IWffu100", 269, True, b"\x01" + b"\0" * 24 + ZLIB), ff.MW2, ff.XBOX360)
        # Black Ops is encrypted on the Xbox 360.
        self.check(header(b"IWff0100", 473, True, b"\0\0\0\x22PHEEBs71" + b"\0" * 4 + b"common"), ff.BO1,
                   ff.CONSOLE, signed=True, encrypted=True)

    def test_console_without_a_zlib_stream_stays_unsure(self):
        # PS3 files are packed in blocks (CoD-FF-Tools); nothing here says which console.
        self.check(header(b"IWffu100", 387, True, b"\x40\x00" + b"\0" * 30), ff.WAW, ff.CONSOLE)

    def test_cod2_xbox360_has_no_magic(self):
        data = (0x68).to_bytes(4, "big") + (1000).to_bytes(4, "big") + b"\0" * 12 + ZLIB
        ident = self.check(data, ff.COD2, ff.XBOX360)
        self.assertEqual(ident.game_name, "Call of Duty 2")
        # Without its zlib stream at byte 20 it is not taken for one.
        self.assertIsNone(ff.identify((0x68).to_bytes(4, "big") + b"\0" * 40))

    def test_wii(self):
        self.check(header(b"IWffu100", 0x1A2, True), ff.COD4, ff.WII)
        self.check(header(b"IWffu100", 0x19B, True), ff.WAW, ff.WII)

    def test_magic_belongs_to_the_game(self):
        # Black Ops II's version under the IW magic, or World at War's under Treyarch's, is nothing known.
        self.assertIsNone(ff.identify(header(b"IWffu100", 147, False)))
        self.assertIsNone(ff.identify(header(b"TAff0100", 387, False)))
        self.assertIn("version 0x93", ff.unknown_version(header(b"IWffu100", 147, False)))

    def test_not_fastfiles(self):
        for data in (b"", b"IWffu10", b"PK\x03\x04" + b"\0" * 40, b"KAPF" + b"\0" * 60,
                     header(b"IWffu100", 12345, False)):
            self.assertIsNone(ff.identify(data))
        self.assertIsNone(ff.unknown_version(b"PK\x03\x04" + b"\0" * 40))


class Files(unittest.TestCase):
    def test_split_by_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "waw.ff").write_bytes(header(b"IWffu100", 387, False))
            (d / "cod4.ff").write_bytes(header(b"IWffu100", 5, False))
            (d / "bo1.ff").write_bytes(header(b"IWffu100", 473, False))
            (d / "odd.ff").write_bytes(b"IWffu100")          # unknown: left for the ripper to judge
            files = sorted(d.iterdir())
            waw, others = ff.split_by_game(files)
            self.assertEqual([p.name for p in waw], ["odd.ff", "waw.ff"])
            self.assertEqual(others, {"Call of Duty 4: Modern Warfare (PC)": [d / "cod4.ff"],
                                      "Call of Duty: Black Ops (PC)": [d / "bo1.ff"]})
            self.assertTrue(ff.is_world_at_war(d / "waw.ff"))
            self.assertFalse(ff.is_world_at_war(d / "cod4.ff"))
            self.assertFalse(ff.is_world_at_war(d / "missing.ff"))


class TheSetupToolTellsGamesApart(unittest.TestCase):
    """Where the setup tool uses it: Xbox 360 discs, custom maps, the rip."""

    def test_another_games_disc_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            iso = Path(tmp) / "cod4.iso"
            make_xiso(iso, {"common.ff": header(b"IWffu100", 1, True)})
            ok, msg = waw.check_xbox360_image(iso)
            self.assertFalse(ok)
            self.assertIn("Call of Duty 4: Modern Warfare (Xbox 360) disc image, not World at War", msg)
            make_xiso(iso, {"common.ff": header(b"IWffu100", 387, True)})
            self.assertTrue(waw.check_xbox360_image(iso)[0])

    def test_another_games_custom_map_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            archive = tmp / "zombie_bo1map.zip"
            with zipfile.ZipFile(archive, "w") as z:
                z.writestr("mods/zombie_bo1map/mod.ff", header(b"IWffu100", 473, False))
            log = quiet_log()
            with self.assertRaises(common.SetupError) as raised:
                waw.install_custom_map(archive, tmp / "out", tmp / "scratch", waw.Unpackers(),
                                       common.Runner(log, threading.Event()), log, threading.Event())
            self.assertIn("Call of Duty: Black Ops (PC)", str(raised.exception))
            self.assertEqual(waw.list_custom_maps(tmp / "out"), [])

    def test_the_rip_leaves_other_games_out(self):
        class Runner:
            ran = []

            def check_cancel(self):
                pass

            def capture(self, cmd, timeout=None):
                self.ran.append(Path(cmd[2]).name)
                return 0, ""

        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "patch.ff").write_bytes(header(b"IWffu100", 387, False))
            (d / "ui.ff").write_bytes(header(b"IWffu100", 5, False))
            lines = []
            log = common.Log(sinks=[lambda kind, text, frac: lines.append(text)])
            runner = Runner()
            ok, errors = rippers.rip_fastfiles(Path("rip"), sorted(d.iterdir()), d / "out", runner, log)
            self.assertEqual((ok, errors, runner.ran), (1, [], ["patch.ff"]))
            self.assertTrue(any("Left out 1 Call of Duty 4: Modern Warfare (PC) fastfile" in line
                                for line in lines), lines)


if __name__ == "__main__":
    unittest.main()

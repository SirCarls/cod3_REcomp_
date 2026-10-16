"""Tests for taking map packs out of patch downloads. They build small fake
downloads (no game data): a zip holding a zip self-extracting installer, and
an installer with a fastfile stored inside it.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import io
import struct
import sys
import tempfile
import threading
import unittest
import zipfile
import zlib
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import common, map_packs  # noqa: E402


def quiet_log() -> common.Log:
    return common.Log(sinks=[])


def fake_fastfile(map_name: str) -> bytes:
    """A PC fastfile header and a zlib zone that names its map."""
    zone = b"\0" * 64 + f"maps/mp/{map_name}.d3dbsp".encode() + bytes(range(256)) * 40
    return b"IWffu100" + struct.pack("<I", 387) + zlib.compress(zone)


def fake_iwd() -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("images/x.iwi", b"image")
    return buf.getvalue()


def fake_program(payload: bytes = b"") -> bytes:
    """A tiny Windows program (one section) with `payload` appended."""
    pe = 0x80
    head = bytearray(0x200)
    head[0:2] = b"MZ"
    head[0x3C:0x40] = struct.pack("<I", pe)
    head[pe:pe + 4] = b"PE\0\0"
    head[pe + 6:pe + 8] = struct.pack("<H", 1)        # one section
    head[pe + 20:pe + 22] = struct.pack("<H", 0xE0)   # optional header size
    section = pe + 24 + 0xE0
    head[section:section + 8] = b".text\0\0\0"
    head[section + 16:section + 24] = struct.pack("<II", 0x100, 0x200)  # raw size, raw pointer
    return bytes(head) + b"\xcc" * 0x100 + payload


class MapPackTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.cancel = threading.Event()
        self.log = quiet_log()
        self.runner = common.Runner(self.log, self.cancel)

    def tearDown(self):
        self.tmp.cleanup()

    def test_zip_holding_a_zip_installer(self):
        # The installer: a program with a zip appended (a self-extractor).
        inner = io.BytesIO()
        with zipfile.ZipFile(inner, "w") as z:
            z.writestr("Program/zone/english/mp_test.ff", fake_fastfile("mp_test"))
            z.writestr("Program/zone/english/patch.ff", b"RTPatch difference data")
            z.writestr("Program/main/iw_99.iwd", fake_iwd())
            z.writestr("Program/readme.txt", b"hello")
        installer = fake_program(inner.getvalue())
        download = self.root / "codwaw-v1.5_1.6.zip"
        with zipfile.ZipFile(download, "w") as z:
            z.writestr("CoDWaW-1.5-1.6-PatchSetup.exe", installer)

        self.assertEqual(map_packs.identify(download), map_packs.MAP_PACK_3)
        dest = self.root / "out"
        files = map_packs.extract(download, dest, self.root / "scratch", map_packs.Unpackers(), self.runner,
                                  self.log, self.cancel)
        self.assertEqual(files, [dest / "zone" / "english" / "mp_test.ff"])
        self.assertTrue((dest / "main" / "iw_99.iwd").is_file())
        self.assertFalse((dest / "zone" / "english" / "patch.ff").exists())   # not a whole fastfile
        self.assertFalse((self.root / "scratch" / "map-pack-unpack").exists())  # cleaned up
        self.assertTrue(download.is_file())                                    # the download is kept

    def test_carving_a_stored_fastfile(self):
        ff = fake_fastfile("mp_carved")
        installer = self.root / "CoDWaW-PatchSetup.exe"
        installer.write_bytes(fake_program(b"\x11" * 1000 + ff + b"\x22" * 500))
        dest = self.root / "carved"
        found = map_packs.carve_fastfiles(installer, dest, self.log, self.cancel)
        self.assertEqual([p.name for p in found], ["mp_carved.ff"])
        self.assertEqual(found[0].read_bytes(), ff)

    def test_signatures_across_chunks(self):
        data = b"x" * 10 + b"IWffu100" + b"y" * 20 + b"IWffu100"
        path = self.root / "blob.bin"
        path.write_bytes(data)
        for chunk in (4, 7, 13, 64):
            with mock.patch.object(map_packs, "CHUNK", chunk):
                found = map_packs.scan_signatures(path, {b"IWffu100": "ff"})
            self.assertEqual(found[b"IWffu100"], [10, 38], chunk)

    def test_pe_overlay(self):
        path = self.root / "setup.exe"
        path.write_bytes(fake_program(b"PAYLOAD"))
        self.assertEqual(map_packs.pe_overlay(path), 0x300)
        lines = map_packs.describe(path)
        self.assertTrue(any("appended data starts at 0x300" in line for line in lines))

    def test_only_missing_fastfiles_are_added(self):
        source = [Path("a/mp_airfield.ff"), Path("a/common_mp.ff")]
        extra = [Path("b/mp_drum.ff"), Path("b/Common_mp.ff")]
        new, already = map_packs.missing_from(source, extra)
        self.assertEqual(new, [Path("b/mp_drum.ff")])
        self.assertEqual(already, ["Common_mp"])

    def test_download_names(self):
        self.assertTrue(map_packs.looks_like_patch(Path("CoDWaW-1.4-1.5_PatchSetup.exe")))
        self.assertTrue(map_packs.looks_like_patch(Path("codwaw-v1.5_1.6.zip")))
        self.assertFalse(map_packs.looks_like_patch(Path("holiday.zip")))


if __name__ == "__main__":
    unittest.main()

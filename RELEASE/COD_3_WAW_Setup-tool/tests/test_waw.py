"""Tests for the World at War source rule, Steam library parsing, the Xbox
disc image reader and custom map unpacking. They build small fake inputs;
no game data is used.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import struct
import sys
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import common, waw  # noqa: E402
from c3w.xiso import XisoImage  # noqa: E402

SECTOR = 2048
XGD2_BASE = 0x0FD90000


def quiet_log() -> common.Log:
    return common.Log(sinks=[])


def make_xiso(path: Path, files: dict) -> None:
    """A minimal XGD2-layout Xbox disc image with a one-entry-per-folder tree.
    `files` maps one 'dir/dir/name' path to its bytes."""
    (rel, data), = files.items()
    parts = rel.split("/")

    def entry(start: int, size: int, attr: int, name: bytes) -> bytes:
        e = struct.pack("<HHIIBB", 0, 0, start, size, attr, len(name)) + name
        return e + b"\xff" * ((-len(e)) % 4)

    with open(path, "wb") as f:
        header = b"MICROSOFT*XBOX*MEDIA" + struct.pack("<IIQ", 40, SECTOR, 0)
        header += b"\0" * (0x7EC - len(header)) + b"MICROSOFT*XBOX*MEDIA"
        f.seek(XGD2_BASE + 32 * SECTOR)
        f.write(header + b"\0" * (SECTOR - len(header)))
        sector = 40
        for depth, name in enumerate(parts):
            last = depth == len(parts) - 1
            target = 60 if last else sector + 1
            node = entry(target, len(data) if last else SECTOR, 0x80 if last else 0x10, name.encode())
            f.seek(XGD2_BASE + sector * SECTOR)
            f.write(node + b"\xff" * (SECTOR - len(node)))
            sector += 1
        f.seek(XGD2_BASE + 60 * SECTOR)
        f.write(data)


class SourceRule(unittest.TestCase):
    def test_steam_is_the_only_source_when_given(self):
        source, notes = waw.choose_source("/steam/waw", "/x/waw.iso", "/pc/disc", waw.PC_DISC)
        self.assertEqual(source.kind, waw.STEAM)
        self.assertEqual(len(notes), 1)
        self.assertIn("only World at War source", notes[0])

    def test_alternative_picks_between_xbox_and_pc(self):
        self.assertEqual(waw.choose_source("", "/x.iso", "/pc", waw.PC_DISC)[0].kind, waw.PC_DISC)
        self.assertEqual(waw.choose_source("", "/x.iso", "/pc", waw.XBOX360)[0].kind, waw.XBOX360)

    def test_only_one_alternative_filled_in_is_used(self):
        self.assertEqual(waw.choose_source("", "", "/pc", waw.XBOX360)[0].kind, waw.PC_DISC)

    def test_nothing_given(self):
        self.assertEqual(waw.choose_source(" ", "", "", waw.XBOX360)[0].kind, waw.NONE)


class SteamLibraries(unittest.TestCase):
    def test_libraryfolders_vdf(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "steamapps").mkdir()
            (root / "steamapps" / "libraryfolders.vdf").write_text(
                '"libraryfolders"\n{\n "0"\n {\n  "path"\t\t"D:\\\\SteamLibrary"\n }\n'
                ' "1"\n {\n  "path"\t\t"/data/SteamLibrary"\n }\n}\n', encoding="utf-8")
            libraries = waw._steam_libraries(root)
            self.assertIn(Path("D:\\SteamLibrary"), libraries)
            self.assertIn(Path("/data/SteamLibrary"), libraries)

    def test_install_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / "steamapps" / "common" / waw.STEAM_INSTALL_DIR
            for sub in ("main", "zone/english"):
                (folder / sub).mkdir(parents=True)
            self.assertFalse(waw.validate_install(folder)[0])
            (folder / "CoDWaW.exe").write_bytes(b"")
            ok, msg = waw.validate_install(folder)
            self.assertTrue(ok)
            self.assertIn("Steam", msg)

    def test_fastfiles_skip_mods(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for rel in ("zone/english/patch.ff", "zone/english/mods/x/mod.ff", "zone/common.ff"):
                (folder / rel).parent.mkdir(parents=True, exist_ok=True)
                (folder / rel).write_bytes(b"IWffu100")
            names = [p.name for p in waw.fastfiles(folder)]
            self.assertEqual(sorted(names), ["common.ff", "patch.ff"])


class XboxImage(unittest.TestCase):
    def test_extract_only_zone(self):
        with tempfile.TemporaryDirectory() as tmp:
            iso = Path(tmp) / "waw.iso"
            make_xiso(iso, {"zone/english/patch.ff": b"IWffu100" + b"x" * 100})
            ok, msg = waw.check_xbox360_image(iso)
            self.assertTrue(ok, msg)
            self.assertEqual(waw.guess_kind(iso), waw.XBOX360)
            source = waw.Source(waw.XBOX360, iso)
            log = quiet_log()
            folder = waw.prepare(source, Path(tmp) / "work", None, None, common.Runner(log, threading.Event()),
                                 log, threading.Event())
            self.assertEqual([p.name for p in waw.fastfiles(folder)], ["patch.ff"])
            with XisoImage(iso) as img:
                self.assertEqual(img.kind, "XGD2")


    def test_fastfiles_in_the_disc_root(self):
        # The real Xbox 360 disc keeps its fastfiles in the root folder.
        with tempfile.TemporaryDirectory() as tmp:
            iso = Path(tmp) / "waw.iso"
            make_xiso(iso, {"nazi_zombie_prototype.ff": b"IWffu100" + b"y" * 64})
            self.assertTrue(waw.check_xbox360_image(iso)[0])
            log = quiet_log()
            folder = waw.prepare(waw.Source(waw.XBOX360, iso), Path(tmp) / "work", None, None,
                                 common.Runner(log, threading.Event()), log, threading.Event())
            self.assertEqual([p.name for p in waw.fastfiles(folder)], ["nazi_zombie_prototype.ff"])


class CustomMaps(unittest.TestCase):
    def test_zip_map_goes_into_the_output_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            archive = tmp / "nazi_zombie_test.zip"
            with zipfile.ZipFile(archive, "w") as z:
                z.writestr("mods/nazi_zombie_test/mod.ff", b"IWffu100")
                z.writestr("usermaps/nazi_zombie_test/zone/english/nazi_zombie_test.ff", b"IWffu100")
            out = tmp / "out"
            log = quiet_log()
            maps = waw.install_custom_map(archive, out, tmp / "scratch", waw.Unpackers(),
                                          common.Runner(log, threading.Event()), log, threading.Event())
            self.assertEqual([m.name for m in maps], ["nazi_zombie_test"])
            listed = waw.list_custom_maps(out)
            self.assertEqual(len(listed[0].fastfiles), 2)
            self.assertTrue(waw.is_within(listed[0].folder, out / waw.CUSTOM_MAPS))
            waw.remove_custom_map(listed[0], out, log)
            self.assertEqual(waw.list_custom_maps(out), [])


if __name__ == "__main__":
    unittest.main()

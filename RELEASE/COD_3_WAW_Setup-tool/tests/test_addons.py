"""[acct2-decomp] Tests for adding the Xbox 360 map packs (c3w/addons.py,
c3w/stfs.py). They build small fake STFS packages (no game data) and check
that good ones install where the game looks, and that every kind of bad one
is refused without writing anything.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import hashlib
import io
import struct
import sys
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import addons, common, stfs  # noqa: E402

BLOCK = 0x1000
HEADER = 0xA000          # header_size 0x971A rounded up to a block
# Laid out like the real files: "//" comments, columns lined up with runs of tabs.
DLMAPS = ("//  A comment line, with 'quotes' and 100% of anything in it.\r\n"
          "//   \tMapPack\t\tID\tFrontEnd DisplayName\tPakName\t\tLoadingScreen Title\tLoadingScreen Location\r\n"
          "\r\n"
          "\t9\t\t100\tTest_Field_(Test)\tmp_test\t\tTEST_FIELD\t\tSOMEWHERE,_FRANCE\t\t\tUS\r\n"
          "\r\n"
          "\t9\t\t100\tTest_Field_(Test)\tmp_test\t\tCHAMP\t\tP\xc9RIERS,_FRANCE\t\tFR\r\n").encode("latin-1")


def make_package(files, title=addons.TITLE_ID, content_type=addons.CONTENT_MARKETPLACE, magic=b"LIVE",
                 dirs=None, raw_entries=None) -> bytes:
    """A read-only ("female") STFS package with one file-table block at data
    block 0 and the files in consecutive blocks after it. `files` maps a
    path to its bytes; folders are made from the paths."""
    dirs = list(dirs or [])
    for p in files:
        parts = p.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            d = "/".join(parts[:i])
            if d not in dirs:
                dirs.append(d)
    index = {}
    entries = []                              # (name, parent index, is_dir, data)
    for d in dirs:
        parent = index.get("/".join(d.split("/")[:-1]), -1)
        index[d] = len(entries)
        entries.append((d.split("/")[-1], parent, True, b""))
    for p, data in files.items():
        parent = index.get("/".join(p.split("/")[:-1]), -1)
        entries.append((p.split("/")[-1], parent, False, data))
    blocks = [b""]                             # block 0: the file table, filled below
    table = bytearray(BLOCK)
    for i, (name, parent, is_dir, data) in enumerate(entries):
        e = bytearray(0x40)
        nb = name.encode("latin-1")
        e[:len(nb)] = nb
        e[0x28] = len(nb) | (0x80 if is_dir else 0x40)
        start = len(blocks)
        n = 0 if is_dir else (len(data) + BLOCK - 1) // BLOCK
        e[0x29:0x2C] = n.to_bytes(3, "little")
        e[0x2C:0x2F] = n.to_bytes(3, "little")
        e[0x2F:0x32] = (start if n else 0).to_bytes(3, "little")
        e[0x32:0x34] = struct.pack(">h", parent)
        e[0x34:0x38] = struct.pack(">I", len(data))
        table[i * 0x40:(i + 1) * 0x40] = e
        for k in range(n):
            blocks.append(data[k * BLOCK:(k + 1) * BLOCK].ljust(BLOCK, b"\0"))
    if raw_entries:
        for i, e in raw_entries.items():
            table[i * 0x40:(i + 1) * 0x40] = e
    blocks[0] = bytes(table)
    assert len(blocks) < 0xAA
    head = bytearray(HEADER)
    head[:4] = magic
    head[0x340:0x344] = struct.pack(">I", 0x971A)
    head[0x344:0x348] = struct.pack(">I", content_type)
    head[0x360:0x364] = struct.pack(">I", title)
    vd = bytearray(0x24)
    vd[0] = 0x24
    vd[2] = 1                                  # read-only: one copy of each hash table
    vd[3:5] = struct.pack("<H", 1)
    vd[5:8] = (0).to_bytes(3, "little")
    vd[0x1C:0x20] = struct.pack(">I", len(blocks))
    head[0x379:0x379 + 0x24] = vd
    head[0x3A9:0x3AD] = b"\0\0\0\0"
    head[0x411:0x411 + 8] = "Test".encode("utf-16-be")
    hashes = bytearray(BLOCK)
    hashes[21:24] = b"\xff\xff\xff"            # block 0 (the file table) ends its chain
    return bytes(head) + bytes(hashes) + b"".join(blocks)


GOOD = {"mp/dlmaps.txt": DLMAPS, "mp/mp_test/mp_test.cod": b"KAPF" + bytes(range(256)) * 40,
        "mp/mp_test/mp_test.wbk": b"wave" * 3000}


class AddonTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.out = self.root / "output"
        self.out.mkdir()
        self.scratch = self.root / "work" / "scratch"
        self.log = common.Log(sinks=[])
        self.cancel = threading.Event()

    def tearDown(self):
        self.tmp.cleanup()

    def write_pkg(self, files=GOOD, name="pkg", **kw) -> Path:
        p = self.root / "in" / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(make_package(files, **kw))
        return p

    def known(self, path: Path, name="Test map pack"):
        """Patch the known-pack list so this fake package counts as known."""
        sha = hashlib.sha1(path.read_bytes()).hexdigest()
        pack = addons.KnownPack(name, "AB" * 21, path.stat().st_size)
        return mock.patch.dict(addons.KNOWN_PACKS, {sha: pack}, clear=True)

    def installed(self) -> Path:
        return self.out / "userdata" / addons.CONTENT_DIR / ("AB" * 21)

    # ------------------------------------------------------------ STFS reading
    def test_reads_files(self):
        p = self.write_pkg()
        with stfs.StfsPackage(p) as pkg:
            self.assertEqual(pkg.title_id, addons.TITLE_ID)
            self.assertEqual(pkg.display_name, "Test")
            ents = {e.path: e for e in pkg.entries()}
            self.assertTrue(ents["mp"].is_dir and ents["mp/mp_test"].is_dir)
            self.assertEqual(pkg.read_file(ents["mp/dlmaps.txt"]), DLMAPS)
            buf = io.BytesIO()
            pkg.copy_file(ents["mp/mp_test/mp_test.cod"], buf.write)
            self.assertEqual(buf.getvalue(), GOOD["mp/mp_test/mp_test.cod"])

    def test_not_a_package(self):
        p = self.root / "x.bin"
        p.write_bytes(b"MZ" + b"\0" * 0xA000)
        with self.assertRaises(stfs.StfsError):
            stfs.StfsPackage(p)

    def test_bad_names_refused(self):
        for bad in ("..", "a:b", "con.txt", "x\x01y", "dir.", "a b"):
            p = self.write_pkg({"mp/" + bad: b"x"}, name="bad")
            with stfs.StfsPackage(p) as pkg, self.assertRaises(stfs.StfsError, msg=bad):
                pkg.entries()

    def test_case_twins_refused(self):
        p = self.write_pkg({"mp/A.cod": b"1", "mp/a.cod": b"2"}, name="twins")
        with stfs.StfsPackage(p) as pkg, self.assertRaises(stfs.StfsError):
            pkg.entries()

    def test_block_outside_package_refused(self):
        e = bytearray(0x40)
        e[:5] = b"x.cod"
        e[0x28] = 5 | 0x40
        e[0x29:0x2C] = (3).to_bytes(3, "little")
        e[0x2F:0x32] = (0x50).to_bytes(3, "little")        # past the end
        e[0x32:0x34] = struct.pack(">h", -1)
        e[0x34:0x38] = struct.pack(">I", 3 * BLOCK)
        p = self.write_pkg({"mp/dlmaps.txt": DLMAPS}, name="far", raw_entries={5: bytes(e)})
        with stfs.StfsPackage(p) as pkg, self.assertRaises(stfs.StfsError):
            # refused when listing (more blocks than the package has) or, at the latest, when reading
            for ent in pkg.entries():
                if ent.path == "x.cod":
                    pkg.copy_file(ent, lambda b: None)

    def test_size_bigger_than_blocks_refused(self):
        e = bytearray(0x40)
        e[:5] = b"y.cod"
        e[0x28] = 5 | 0x40
        e[0x29:0x2C] = (1).to_bytes(3, "little")
        e[0x2F:0x32] = (1).to_bytes(3, "little")
        e[0x32:0x34] = struct.pack(">h", -1)
        e[0x34:0x38] = struct.pack(">I", 50 * BLOCK)
        p = self.write_pkg({"mp/dlmaps.txt": DLMAPS}, name="big", raw_entries={5: bytes(e)})
        with stfs.StfsPackage(p) as pkg, self.assertRaises(stfs.StfsError):
            pkg.entries()

    # ------------------------------------------------------------ dlmaps.txt
    def test_dlmaps(self):
        rows = addons.parse_dlmaps(DLMAPS)
        self.assertEqual([r.pak for r in rows], ["mp_test", "mp_test"])
        self.assertEqual(rows[1].location, "P\xc9RIERS,_FRANCE")
        self.assertEqual(rows[0].language, "US")
        for bad in (b"a\tb\tc\n", b"T\t1\t%s%n\tmp_x\tt\tl\tUS\n", b"T\t1\tN\t../x\tt\tl\tUS\n",
                    b"T\tone\tN\tmp_x\tt\tl\tUS\n", b"T\t1\tN\tmp_x\tt\tl\tUSA\n", b"T\t1\tN\x07\tmp_x\tt\tl\tUS\n",
                    b"\r\n", b"T\t1\t" + b"N" * 65 + b"\tmp_x\tt\tl\tUS\n"):
            with self.assertRaises(common.SetupError, msg=bad):
                addons.parse_dlmaps(bad)

    # ------------------------------------------------------------ adding
    def test_add_package(self):
        p = self.write_pkg()
        with self.known(p):
            added = addons.add([p], self.out, self.scratch, self.cancel, self.log)
        self.assertEqual([a.maps for a in added], [["mp_test"]])
        dest = self.installed()
        self.assertEqual((dest / "mp" / "dlmaps.txt").read_bytes(), DLMAPS)
        self.assertEqual((dest / "mp" / "mp_test" / "mp_test.cod").read_bytes(), GOOD["mp/mp_test/mp_test.cod"])
        reg = addons.read_registry(self.out)
        self.assertEqual([(a.id, a.kind, a.present) for a in reg], [("AB" * 21, "map_pack", True)])
        self.assertIn("Test map pack", addons.summary(self.out))
        self.assertEqual(p.read_bytes(), make_package(GOOD))           # the download is never changed

    def test_add_from_zip(self):
        p = self.write_pkg()
        z = self.root / "in" / "pack.zip"
        with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("readme.txt", "hello")
            zf.writestr("../../evil/415607E1/00000002/pkg", p.read_bytes())   # the member's name is never used
        with self.known(p):
            addons.add([z], self.out, self.scratch, self.cancel, self.log)
        self.assertTrue((self.installed() / "mp" / "dlmaps.txt").is_file())
        self.assertFalse((self.root / "evil").exists())
        self.assertEqual(list(self.scratch.glob("*")) if self.scratch.exists() else [], [])

    def test_add_from_folder(self):
        p = self.write_pkg()
        with self.known(p):
            addons.add([p.parent], self.out, self.scratch, self.cancel, self.log)
        self.assertTrue(self.installed().is_dir())

    def assertRefused(self, p: Path, patch_known: bool = True):
        ctx = self.known(p) if patch_known else mock.patch.dict(addons.KNOWN_PACKS, {}, clear=True)
        with ctx, self.assertRaises(common.SetupError):
            addons.add([p], self.out, self.scratch, self.cancel, self.log)
        content = self.out / "userdata" / addons.CONTENT_DIR
        self.assertEqual(list(content.iterdir()) if content.exists() else [], [])

    def test_unknown_pack_refused(self):
        self.assertRefused(self.write_pkg(), patch_known=False)

    def test_wrong_title_refused(self):
        self.assertRefused(self.write_pkg(title=0x4D5307E6))

    def test_wrong_content_type_refused(self):
        self.assertRefused(self.write_pkg(content_type=1))

    def test_program_in_pack_refused(self):
        self.assertRefused(self.write_pkg(dict(GOOD, **{"mp/mp_test/run.xex": b"XEX2"})))

    def test_file_outside_mp_refused(self):
        self.assertRefused(self.write_pkg(dict(GOOD, **{"other/x.cod": b"x"})))

    def test_missing_map_folder_refused(self):
        files = {"mp/dlmaps.txt": DLMAPS, "mp/other/other.cod": b"x"}
        self.assertRefused(self.write_pkg(files))

    def test_bad_dlmaps_refused(self):
        files = dict(GOOD, **{"mp/dlmaps.txt": b"T\t1\t%n%n%n\tmp_test\tt\tl\tUS\r\n"})
        self.assertRefused(self.write_pkg(files))

    def test_zip_bomb_refused(self):
        z = self.root / "in" / "bomb.zip"
        z.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("pkg", b"LIVE" + b"\0" * (4 << 20))
        with mock.patch.dict(addons.KNOWN_PACKS, {}, clear=True), self.assertRaises(common.SetupError):
            addons.add([z], self.out, self.scratch, self.cancel, self.log)

    def test_reinstall_replaces(self):
        p = self.write_pkg()
        with self.known(p):
            addons.add([p], self.out, self.scratch, self.cancel, self.log)
            stray = self.installed() / "mp" / "stray.txt"
            stray.write_text("old")
            addons.add([p], self.out, self.scratch, self.cancel, self.log)
        self.assertFalse(stray.exists())
        self.assertEqual(len(addons.read_registry(self.out)), 1)
        self.assertEqual([x.name for x in self.installed().parent.iterdir()], ["AB" * 21])

    def test_cancel_keeps_old(self):
        p = self.write_pkg()
        with self.known(p):
            addons.add([p], self.out, self.scratch, self.cancel, self.log)
            marker = self.installed() / "mp" / "keep.txt"
            marker.write_text("old")
            cancel = threading.Event()
            real = stfs.StfsPackage.copy_file

            def copy_then_cancel(pkg, e, write, check=None):
                real(pkg, e, write, check)
                cancel.set()
            with mock.patch.object(stfs.StfsPackage, "copy_file", copy_then_cancel), \
                    self.assertRaises(common.Cancelled):
                addons.add([p], self.out, self.scratch, cancel, self.log)
        self.assertTrue(marker.exists())
        self.assertEqual([x.name for x in self.installed().parent.iterdir()], ["AB" * 21])

    def test_remove(self):
        p = self.write_pkg()
        with self.known(p):
            addons.add([p], self.out, self.scratch, self.cancel, self.log)
        addons.remove(self.out, "AB" * 21, self.log)
        self.assertFalse(self.installed().exists())
        self.assertEqual(addons.read_registry(self.out), [])
        self.assertEqual(addons.summary(self.out), "none")

    def test_tampered_registry_never_deletes_elsewhere(self):
        victim = self.root / "victim"
        victim.mkdir()
        (victim / "file").write_text("keep")
        common.write_json(self.out / "userdata" / addons.REGISTRY, {"version": 1, "addons": [
            {"id": "AB" * 21, "kind": "map_pack", "name": "x", "sha1": "0", "folder": "../../victim"},
            {"id": "../victim", "kind": "map_pack", "name": "x", "sha1": "0",
             "folder": (addons.CONTENT_DIR / "AB").as_posix()}]})
        self.assertEqual(addons.read_registry(self.out), [])
        with self.assertRaises(common.SetupError):
            addons.remove(self.out, "AB" * 21, self.log)
        self.assertTrue((victim / "file").exists())

    def test_needs_output_folder(self):
        with self.assertRaises(common.SetupError):
            addons.add([self.write_pkg()], self.root / "nope", self.scratch, self.cancel, self.log)


if __name__ == "__main__":
    unittest.main()

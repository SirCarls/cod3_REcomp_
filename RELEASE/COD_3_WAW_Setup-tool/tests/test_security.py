"""Tests for the security rules: the work folder the cleanup empties is
always one the tool made, and a crafted disc image can't make the reader loop
forever or write outside the folder it extracts to. Small made-up files only;
no game data.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import struct
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import common, pipeline, xiso  # noqa: E402


def quiet_log() -> common.Log:
    return common.Log(sinks=[])


class WorkFolderTests(unittest.TestCase):
    def test_new_or_empty_folder_is_claimed(self):
        with tempfile.TemporaryDirectory() as tmp:
            for work in (Path(tmp) / "new", Path(tmp) / "empty"):
                if work.name == "empty":
                    work.mkdir()
                common.claim_work_dir(work)
                self.assertTrue(common.owns_work_dir(work))

    def test_folder_with_other_files_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / "Documents"
            work.mkdir()
            (work / "thesis.odt").write_text("mine")
            with self.assertRaises(common.SetupError):
                common.claim_work_dir(work)
            self.assertFalse(common.owns_work_dir(work))
            self.assertEqual((work / "thesis.odt").read_text(), "mine")

    def test_folder_from_an_earlier_build_is_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / "work"
            (work / "tools").mkdir(parents=True)
            (work / "setup.log").write_text("old log")
            common.claim_work_dir(work)
            self.assertTrue(common.owns_work_dir(work))

    def test_a_file_is_not_a_work_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / "work"
            work.write_text("a file")
            with self.assertRaises(common.SetupError):
                common.claim_work_dir(work)

    def test_build_refuses_a_foreign_work_folder_before_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / "Downloads"
            work.mkdir()
            (work / "photo.jpg").write_bytes(b"jpg")
            opts = pipeline.BuildOptions(output_dir=str(Path(tmp) / "out"), work_dir=str(work))
            with self.assertRaises(common.SetupError):
                pipeline.Pipeline(opts, quiet_log(), threading.Event()).run()
            self.assertEqual(sorted(p.name for p in work.iterdir()), ["photo.jpg"])

    def test_cleanup_empties_only_a_marked_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            for marked in (True, False):
                work = Path(tmp) / f"work-{marked}"
                work.mkdir()
                if marked:
                    common.claim_work_dir(work)   # then the build fills it
                (work / "tools").mkdir()
                (work / "cod3").mkdir()
                (work / "stray.txt").write_text("x")
                opts = pipeline.BuildOptions(output_dir=str(out), work_dir=str(work))
                pipeline.Pipeline(opts, quiet_log(), threading.Event()).cleanup()
                left = sorted(p.name for p in work.iterdir())
                if marked:
                    self.assertEqual(left, sorted(["tools", common.WORK_MARKER]))
                else:
                    self.assertEqual(left, ["cod3", "stray.txt", "tools"])


def make_xiso(path: Path, entries) -> None:
    """A tiny XDVDFS image: the volume header at sector 32 and the root
    folder's table at sector 33. `entries` are (name, attributes, sector,
    size), chained through each record's right link."""
    sector = xiso.SECTOR
    table = bytearray()
    offsets = []
    for name, _, _, _ in entries:
        offsets.append(len(table))
        size = 14 + len(name)
        table += bytes(size + (-size % 4))
    for i, (name, attrs, start, size) in enumerate(entries):
        right = offsets[i + 1] // 4 if i + 1 < len(entries) else 0
        struct.pack_into("<HHIIBB", table, offsets[i], 0, right, start, size, attrs, len(name))
        table[offsets[i] + 14:offsets[i] + 14 + len(name)] = name
    image = bytearray(sector * 40)
    header = 32 * sector
    image[header:header + 20] = xiso.MAGIC
    struct.pack_into("<II", image, header + 20, 33, len(table))
    image[header + 0x7EC:header + 0x7EC + 20] = xiso.MAGIC
    image[33 * sector:33 * sector + len(table)] = table
    image[34 * sector:34 * sector + 5] = b"hello"
    path.write_bytes(bytes(image))


class XisoTests(unittest.TestCase):
    def test_folder_inside_itself_is_read_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            iso = Path(tmp) / "loop.iso"
            # "loop" points back at the root folder's own table.
            make_xiso(iso, [(b"a.txt", 0, 34, 5), (b"loop", xiso.ATTR_DIRECTORY, 33, 64)])
            with xiso.XisoImage(iso) as img:
                self.assertEqual(sorted(e.path for e in img.entries), ["a.txt", "loop"])

    def test_names_stay_inside_the_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            iso = Path(tmp) / "names.iso"
            make_xiso(iso, [(b"..", 0, 34, 5), (b"C:evil.txt", 0, 34, 5), (b"x\\..\\y.txt", 0, 34, 5)])
            dest = Path(tmp) / "out"
            with xiso.XisoImage(iso) as img:
                img.extract(dest, quiet_log(), threading.Event())
            self.assertEqual(sorted(p.name for p in dest.iterdir()), ["C_evil.txt", "x_.._y.txt"])
            self.assertEqual((dest / "C_evil.txt").read_bytes(), b"hello")


class RipperOutputTests(unittest.TestCase):
    def test_fastfile_folder_is_never_the_parent(self):
        from c3w import rippers
        self.assertEqual(rippers.fastfile_folder_name(Path("zone/english/mp_drum.ff")), "mp_drum")
        for odd in ("...ff", "..ff", ". .ff"):
            name = rippers.fastfile_folder_name(Path("zone") / odd)
            self.assertTrue(name.startswith("fastfile_"), odd)
            self.assertNotIn(name, (".", ".."))


if __name__ == "__main__":
    unittest.main()

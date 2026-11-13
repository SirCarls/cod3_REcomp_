"""[acct2-main] Tests for making the HD (waifu2x) textures again without a
full build, and for deleting them (c3w/upscale.py: the .style record,
rebuild_overrides, generated, delete_generated; c3w/pipeline.py:
BuildOptions.upscale_from_scratch). waifu2x and the ripper aren't run: the
stand-ins from test_upscale.py play their part.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import dataclasses
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from c3w import deps, pipeline, upscale  # noqa: E402
from c3w.common import Log  # noqa: E402
from c3w.pipeline import BuildOptions, Pipeline  # noqa: E402
from test_upscale import FakeWaifu2x, make_tool, png_bytes, write_keys  # noqa: E402


def lines_log(lines: list) -> Log:
    return Log(sinks=[lambda kind, text, frac: lines.append((kind, text))])


def age(path: Path, seconds: float) -> None:
    """Moves a file's time `seconds` into the past."""
    t = time.time() - seconds
    os.utime(path, (t, t))


def a_build(root: Path, names=("a", "b")) -> Path:
    """An output folder with ripped textures and the ripper's list."""
    out = root / "out"
    textures = upscale.data_dir(out) / "textures"
    textures.mkdir(parents=True)
    for name in names:
        (textures / f"{name}.png").write_bytes(png_bytes(16, 8))
        age(textures / f"{name}.png", 3600)
    write_keys(textures, list(names))
    return out


class StyleTests(unittest.TestCase):
    def upscale(self, root, src, dst, settings, fake, lines=None):
        tool = root / "tools"
        tool = make_tool(tool) if not (tool / "waifu2x").is_dir() else tool / "waifu2x"
        return upscale.upscale_folder(tool, src, dst, root / "work" / "up",
                                      settings, fake, lines_log(lines if lines is not None else []),
                                      threading.Event())

    def test_a_new_style_or_noise_redoes_the_copies(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = a_build(root)
            src = upscale.data_dir(out) / "textures"
            dst = upscale.data_dir(out) / upscale.OUTPUT_NAME
            fake = FakeWaifu2x()
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("photo", 1), fake), (2, 0))
            self.assertEqual(upscale.recorded_style(dst), "photo 1")
            # The same again: nothing to do.
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("photo", 1), fake), (0, 0))
            self.assertEqual(len(fake.calls), 1)
            for p in list(dst.glob("*.png")) + [dst / upscale.STYLE_FILE]:
                age(p, 600)
            # Another style: every copy is made again, and said so.
            lines = []
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("art", 1), fake, lines), (2, 0))
            self.assertEqual(fake.calls[-1]["model"], "models-upconv_7_anime_style_art_rgb")
            self.assertEqual(upscale.recorded_style(dst), "art 1")
            self.assertTrue(any("made again with the Artwork style" in t for _, t in lines), lines)
            # Another noise reduction: the same.
            for p in list(dst.glob("*.png")) + [dst / upscale.STYLE_FILE]:
                age(p, 600)
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("art", 3), fake), (2, 0))
            self.assertEqual(fake.calls[-1]["noise"], 3)
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("art", 3), fake), (0, 0))

    def test_copies_made_after_the_change_are_kept(self):
        """An interrupted run with the new style: what it made isn't made a
        third time."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = a_build(root)
            src = upscale.data_dir(out) / "textures"
            dst = upscale.data_dir(out) / upscale.OUTPUT_NAME
            dst.mkdir()
            for name in ("a", "b"):
                (dst / f"{name}.png").write_bytes(png_bytes(32, 16))
                age(dst / f"{name}.png", 900)
            upscale.record_style(dst, "photo 1")
            upscale.record_scale(dst, 2)
            age(dst / upscale.STYLE_FILE, 1200)
            since = upscale.choose_style(dst, upscale.Settings("art", 1), Log(sinks=[]))
            (dst / "b.png").write_bytes(png_bytes(32, 16))   # made with the new style, then stopped
            os.utime(dst / "b.png", (since + 1, since + 1))
            fake = FakeWaifu2x()
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("art", 1), fake), (1, 0))
            self.assertEqual(fake.calls[0]["files"], ["a.png"])

    def test_a_folder_from_an_older_setup_tool_is_not_all_made_again(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = a_build(root)
            src = upscale.data_dir(out) / "textures"
            dst = upscale.data_dir(out) / upscale.OUTPUT_NAME
            dst.mkdir()
            for name in ("a", "b"):
                (dst / f"{name}.png").write_bytes(png_bytes(32, 16))
                age(dst / f"{name}.png", 900)
            fake = FakeWaifu2x()
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("cunet", 2), fake), (0, 0))
            self.assertEqual(fake.calls, [])
            # Now known: made with what was chosen; the next change redoes them.
            self.assertEqual(upscale.recorded_style(dst), "cunet 2")
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("cunet", 2), fake), (0, 0))
            age(dst / upscale.STYLE_FILE, 1200)
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("photo", 2), fake), (2, 0))

    def test_1x_copies_without_noise_reduction_stay_up_to_date(self):
        """The originals copied as they are (1x, no noise reduction) get the
        time they're copied, so a style change doesn't leave them "missing"."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = a_build(root)
            src = upscale.data_dir(out) / "textures"
            dst = upscale.data_dir(out) / upscale.OUTPUT_NAME
            fake = FakeWaifu2x()
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("photo", 0, 1), fake), (2, 0))
            for p in list(dst.glob("*.png")) + [dst / upscale.STYLE_FILE]:
                age(p, 600)
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("photo", -1, 1), fake), (2, 0))
            self.assertEqual(len(fake.calls), 1)   # copied, not given to waifu2x
            self.assertEqual(self.upscale(root, src, dst, upscale.Settings("photo", -1, 1), fake), (0, 0))

    def test_1x_always_records_the_model_it_uses(self):
        self.assertEqual(upscale.style_key(upscale.Settings("photo", 2, 1)), "cunet 2")
        self.assertEqual(upscale.style_key(upscale.Settings("bogus", 9, 2)), "photo 1")   # the defaults


class RebuildTests(unittest.TestCase):
    def options(self, out: Path, work: Path, **extra) -> BuildOptions:
        base = BuildOptions(work_dir=str(work), upscale_model="art", upscale_noise=2, upscale_scale=4)
        return dataclasses.replace(base, **upscale.rebuild_overrides(out, **extra))

    def test_only_the_upscaling_and_the_pack(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            o = self.options(a_build(root), root / "work")
            with mock.patch.object(pipeline, "IS_LINUX", True), mock.patch.object(pipeline, "IS_WINDOWS", False):
                titles = [s.title for s in Pipeline(o, Log(sinks=[]), threading.Event()).steps()]
            self.assertEqual(titles, ["Checking inputs", "Tools", "Ripper", "Upscaling textures (waifu2x)",
                                      "HD texture pack", "Finishing", "Cleaning up"])
            # The chosen style, noise reduction and scale are kept.
            self.assertEqual((o.upscale_model, o.upscale_noise, o.upscale_scale), ("art", 2, 4))
            self.assertFalse(o.build_recomp or o.build_linux or o.build_windows)

    def test_no_disc_needed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            o = self.options(a_build(root), root / "work")
            p = Pipeline(o, Log(sinks=[]), threading.Event())
            with mock.patch.object(pipeline, "free_space", return_value=1 << 50), \
                    mock.patch.object(pipeline, "host_triple", return_value="linux-amd64"):
                p.preflight()   # no "Choose the Call of Duty 3 .iso"
            self.assertFalse(p._runs("assets"))
            self.assertFalse(p._runs("rip_cod3"))

    def test_blocker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertIn("no build", upscale.rebuild_blocker(root / "nothing"))
            (root / "empty").mkdir()
            self.assertIn("no ripped textures", upscale.rebuild_blocker(root / "empty"))
            out = a_build(root)
            self.assertEqual(upscale.rebuild_blocker(out), "")
            textures = upscale.data_dir(out) / "textures"
            (textures / upscale.KEYS_FILE).write_text("0001\t64\t64\t18\ta\ta\n", encoding="utf-8")   # old ripper
            self.assertIn("older ripper", upscale.rebuild_blocker(out))

    def test_from_scratch_deletes_the_old_copies_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = a_build(root)
            data = upscale.data_dir(out)
            old = data / upscale.OUTPUT_NAME
            old.mkdir()
            for name in ("a", "b", "gone"):
                (old / f"{name}.png").write_bytes(png_bytes(32, 16))   # up to date, but asked to redo
            (data / upscale.PACK_NAME).mkdir()
            (data / upscale.PACK_NAME / upscale.PACK_INDEX).write_text("", encoding="utf-8")
            lines = []
            p = Pipeline(self.options(out, root / "work", from_scratch=True), lines_log(lines), threading.Event())
            fake = FakeWaifu2x()
            p.runner = fake
            with mock.patch.object(deps, "fetch_portable", return_value=make_tool(root)):
                p.upscale_textures()
            self.assertEqual(sorted(x.name for x in old.glob("*.png")), ["a.png", "b.png"])
            self.assertEqual(len(fake.calls), 1)
            self.assertEqual(fake.calls[0]["scale"], 4)
            self.assertTrue((data / upscale.PACK_NAME / upscale.PACK_INDEX).is_file())   # the pack is kept
            self.assertTrue((data / "textures" / "a.png").is_file())                     # the originals too
            self.assertFalse([t for k, t in lines if k == "warn"], lines)

    def test_without_from_scratch_up_to_date_copies_stay(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = a_build(root)
            p = Pipeline(self.options(out, root / "work"), Log(sinks=[]), threading.Event())
            fake = FakeWaifu2x()
            p.runner = fake
            with mock.patch.object(deps, "fetch_portable", return_value=make_tool(root)):
                p.upscale_textures()
                p.upscale_textures()
            self.assertEqual(len(fake.calls), 1)


class DeleteTests(unittest.TestCase):
    def build_with_everything(self, root: Path) -> Path:
        out = a_build(root)
        data = upscale.data_dir(out)
        for name, size in ((upscale.OUTPUT_NAME, 100), (upscale.LEGACY_OUTPUT_NAME, 30)):
            (data / name / "sub").mkdir(parents=True)
            (data / name / "x.png").write_bytes(b"p" * size)
        (data / upscale.PACK_NAME).mkdir()
        (data / upscale.PACK_NAME / upscale.PACK_INDEX).write_text("k\t1\t1\t1\tx.dds\n", encoding="utf-8")
        (data / upscale.PACK_NAME / "x.dds").write_bytes(b"d" * 50)
        (out / upscale.RECORD).write_text(json.dumps({
            "cod3_textures_upscaled": "data/cod3/textures_upscaled", "cod3_hd_textures": "data/cod3/hd_textures",
            "tool_version": "x"}), encoding="utf-8")
        return out

    def record(self, out: Path) -> dict:
        return json.loads((out / upscale.RECORD).read_text(encoding="utf-8"))

    def test_lists_what_is_there(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self.build_with_everything(Path(tmp))
            found = upscale.generated(out)
            self.assertEqual([(g.what, g.path.name) for g in found],
                             [(upscale.PICTURES, upscale.OUTPUT_NAME), (upscale.PICTURES, upscale.LEGACY_OUTPUT_NAME),
                              (upscale.PACK, upscale.PACK_NAME)])
            self.assertEqual(found[0].size, 100)
            self.assertEqual(upscale.generated(Path(tmp) / "none"), [])

    def test_pictures_only_keeps_the_pack_and_the_originals(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self.build_with_everything(Path(tmp))
            data = upscale.data_dir(out)
            freed, left = upscale.delete_generated(out, frozenset({upscale.PICTURES}), Log(sinks=[]))
            self.assertEqual((freed, left), (130, []))
            self.assertFalse((data / upscale.OUTPUT_NAME).exists())
            self.assertFalse((data / upscale.LEGACY_OUTPUT_NAME).exists())
            self.assertTrue(upscale.pack_ready(data / upscale.PACK_NAME))
            self.assertTrue((data / "textures" / "a.png").is_file())
            rec = self.record(out)
            self.assertIsNone(rec["cod3_textures_upscaled"])
            self.assertEqual(rec["cod3_hd_textures"], "data/cod3/hd_textures")
            self.assertEqual(rec["tool_version"], "x")

    def test_the_pack_too(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self.build_with_everything(Path(tmp))
            freed, left = upscale.delete_generated(out, frozenset({upscale.PICTURES, upscale.PACK}), Log(sinks=[]))
            self.assertEqual((freed, left), (130 + 50 + len("k\t1\t1\t1\tx.dds\n"), []))
            self.assertEqual(upscale.generated(out), [])
            rec = self.record(out)
            self.assertIsNone(rec["cod3_textures_upscaled"])
            self.assertIsNone(rec["cod3_hd_textures"])
            self.assertTrue((upscale.data_dir(out) / "textures" / upscale.KEYS_FILE).is_file())

    @unittest.skipIf(os.name == "nt", "symbolic links need extra rights on Windows")
    def test_a_link_is_removed_not_followed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = a_build(root)
            elsewhere = root / "elsewhere"
            elsewhere.mkdir()
            (elsewhere / "keep.png").write_bytes(b"k")
            os.symlink(elsewhere, upscale.data_dir(out) / upscale.OUTPUT_NAME)
            freed, left = upscale.delete_generated(out, frozenset({upscale.PICTURES}), Log(sinks=[]))
            self.assertEqual(left, [])
            self.assertFalse((upscale.data_dir(out) / upscale.OUTPUT_NAME).is_symlink())
            self.assertTrue((elsewhere / "keep.png").is_file())

    def test_without_a_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self.build_with_everything(Path(tmp))
            (out / upscale.RECORD).unlink()
            upscale.delete_generated(out, frozenset({upscale.PACK}), Log(sinks=[]))
            self.assertFalse((out / upscale.RECORD).exists())


if __name__ == "__main__":
    unittest.main()

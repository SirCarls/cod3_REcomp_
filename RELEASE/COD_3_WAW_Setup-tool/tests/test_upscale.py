"""Tests for the optional waifu2x texture upscaling (c3w/upscale.py) and the
HD texture pack made from it for the playable game. waifu2x and the ripper
aren't run: stand-in "runners" play their part and write the files they
would write.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import json
import os
import struct
import sys
import zlib
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import deps, pipeline, rippers, upscale  # noqa: E402
from c3w.common import Log, SetupError  # noqa: E402
from c3w.pipeline import BuildOptions, Pipeline  # noqa: E402


def png_bytes(width: int, height: int) -> bytes:
    """The start of a PNG of that size (signature and header chunk): all the
    tool reads of one."""
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    chunk = b"IHDR" + ihdr
    return b"\x89PNG\r\n\x1a\n" + struct.pack(">I", len(ihdr)) + chunk + struct.pack(">I", zlib.crc32(chunk))


class FakeWaifu2x:
    """Stands in for Runner.run(waifu2x ...): "upscales" each PNG in -i by
    writing a PNG header -s times its size to -o (or copying a file that
    isn't a real PNG), printing waifu2x's per-file line."""

    def __init__(self):
        self.calls = []

    def run(self, cmd, quiet=False, line_filter=None, **kw):
        args = dict(zip(cmd[1::2], cmd[2::2]))
        src, dst = Path(args["-i"]), Path(args["-o"])
        scale = int(args["-s"])
        self.calls.append({"src": src, "noise": int(args["-n"]), "files": sorted(p.name for p in src.glob("*.png")),
                           "model": Path(args["-m"]).name, "scale": scale})
        for png in src.glob("*.png"):
            size = upscale.png_size(png)
            data = png_bytes(size[0] * scale, size[1] * scale) if size else png.read_bytes()
            (dst / png.name).write_bytes(data)
            if line_filter:
                line_filter(f"{png} -> {dst / png.name} done")
        return 0


def make_tool(root: Path) -> Path:
    tool = root / "waifu2x"
    for model, _ in upscale.MODELS.values():
        (tool / model).mkdir(parents=True)
    (tool / upscale.exe_name()).write_bytes(b"")
    return tool


def write_keys(textures: Path, names, kinds: dict = None) -> None:
    """An hd_keys.tsv as the ripper writes it (output layout 6) for PNGs
    `names`, each its own picture: a color map unless `kinds` says."""
    kinds = kinds or {}
    lines = ["# key\twidth\theight\txenos_format\tname\tfile\tkind"]
    for i, name in enumerate(names, 1):
        lines.append(f"{i:016x}\t64\t64\t18\t{name}\t{name}\t{kinds.get(name, 'diffuse')}")
    (textures / upscale.KEYS_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8")


BOTH = frozenset({"diffuse", "normal"})


class UpscaleTests(unittest.TestCase):
    def test_pinned_for_both_systems(self):
        for key, folder in (("waifu2x-linux", "waifu2x-ncnn-vulkan-20220728-ubuntu"),
                            ("waifu2x-win", "waifu2x-ncnn-vulkan-20220728-windows")):
            url, sub, sha = deps.PORTABLE[key]
            self.assertTrue(url.startswith("https://github.com/nihui/waifu2x-ncnn-vulkan/releases/download/"))
            self.assertEqual(sub, folder)
            self.assertRegex(sha, r"^[0-9a-f]{64}$")
            self.assertIn(key, deps.STEP_ONLY)   # never put on PATH for the build

    def test_normal_maps_by_name(self):
        for name in ("brick_n", "brick_nml", "wall_nrm2", "soldier_normal", "tank_ddn", "rock_n_01", "road-bump",
                     "gmctruck_n", "cloth$burlap_n", "v_metal_detail01_n"):   # the last three: CoD3's style
            self.assertTrue(upscale.is_data_map(Path(name + ".png")), name)
        for name in ("brick", "snow", "nazi_flag", "town_sign", "concrete_d", "lamp_on", "banner", "gmctruck_c",
                     "gmctruck_s", "metal$metal_col"):
            self.assertFalse(upscale.is_data_map(Path(name + ".png")), name)

    def test_settings_fall_back_to_defaults(self):
        s = upscale.Settings("nonsense", 9, 3).validated()
        self.assertEqual((s.model, s.noise, s.scale), (upscale.DEFAULT_MODEL, upscale.DEFAULT_NOISE, 2))
        s = upscale.Settings("cunet", -1, 4).validated()
        self.assertEqual((s.model, s.noise, s.scale), ("cunet", -1, 4))
        self.assertEqual(sorted(upscale.SCALES), [1, 2, 4])   # what waifu2x's -s offers here
        self.assertEqual(upscale.DEFAULT_SCALE, 2)

    def test_command(self):
        cmd = upscale.command(Path("w2x"), Path("in"), Path("out"), Path("models-cunet"), 2)
        self.assertEqual(cmd[cmd.index("-s") + 1], "2")
        self.assertEqual(cmd[cmd.index("-n") + 1], "2")
        self.assertEqual(cmd[cmd.index("-f") + 1], "png")
        self.assertIn("-v", cmd)   # per-file lines, for the progress bar
        cmd = upscale.command(Path("w2x"), Path("in"), Path("out"), Path("models-cunet"), 1, 4)
        self.assertEqual(cmd[cmd.index("-s") + 1], "4")

    def test_png_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.png"
            path.write_bytes(png_bytes(640, 360))
            self.assertEqual(upscale.png_size(path), (640, 360))
            path.write_bytes(b"png")
            self.assertIsNone(upscale.png_size(path))
            self.assertIsNone(upscale.png_size(Path(tmp) / "missing.png"))

    def test_whole_folder_then_nothing_left(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, dst = root / "textures", root / "textures_upscaled"
            src.mkdir()
            for name in ("brick", "snow", "flag"):
                (src / f"{name}.png").write_bytes(b"png")
            (src / "brick.dds").write_bytes(b"dds")   # not an image waifu2x is given
            write_keys(src, ["brick", "snow", "flag"])
            fake = FakeWaifu2x()
            done, missing = upscale.upscale_folder(make_tool(root), src, dst, root / "work" / "upscale",
                                                   upscale.Settings("photo", 1), fake, Log(sinks=[]),
                                                   threading.Event())
            self.assertEqual((done, missing), (3, 0))
            self.assertEqual(len(fake.calls), 1)
            self.assertEqual(fake.calls[0]["src"], src)          # no staging needed
            self.assertEqual(fake.calls[0]["model"], "models-upconv_7_photo")
            self.assertFalse((dst / "brick.dds").exists())
            # Second build: everything is up to date, waifu2x isn't started.
            done, missing = upscale.upscale_folder(make_tool(root / "again"), src, dst, root / "work" / "upscale",
                                                   upscale.Settings(), fake, Log(sinks=[]), threading.Event())
            self.assertEqual((done, missing, len(fake.calls)), (0, 0, 1))

    def test_only_the_pictures_the_pack_uses(self):
        """hd_keys.tsv decides: lightmaps and lookup tables ("-") are left
        out, other pictures under a name (name__key) are included, and only
        color maps (kind diffuse) unless other kinds are asked for."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, dst = root / "textures", root / "textures_upscaled"
            src.mkdir()
            for name in ("brick", "brick__00000000000000ab", "brick_n", "brick_s", "garamond", "lm0000",
                         "fogramp", "unlisted"):
                (src / f"{name}.png").write_bytes(b"png")
            (src / upscale.KEYS_FILE).write_text(
                "# key\twidth\theight\txenos_format\tname\tfile\tkind\n"
                "00000000000000aa\t64\t64\t18\tbrick\tbrick\tdiffuse\n"
                "00000000000000ab\t64\t64\t18\tbrick\tbrick__00000000000000ab\tdiffuse\n"
                "00000000000000ac\t64\t64\t18\tbrick_copy\tbrick\tdiffuse\n"
                "00000000000000ad\t64\t64\t20\tbrick_n\tbrick_n\tnormal\n"
                "00000000000000ae\t64\t64\t18\tbrick_s\tbrick_s\tspecular\n"
                "00000000000000af\t512\t256\t20\tgaramond\tgaramond\tunknown\n"
                "0000000000000001\t512\t512\t20\tlm0000\t-\tother\n"
                "0000000000000002\t256\t8\t6\tfogramp\t-\tunknown\n")
            self.assertEqual([p.stem for p in upscale.sources(src)], ["brick", "brick__00000000000000ab"])
            self.assertEqual([p.stem for p in upscale.sources(src, BOTH)],
                             ["brick", "brick__00000000000000ab", "brick_n"])
            self.assertEqual([p.stem for p in upscale.sources(src, frozenset(upscale.KINDS))],
                             ["brick", "brick__00000000000000ab", "brick_n", "brick_s", "garamond"])
            fake = FakeWaifu2x()
            log_lines = []
            done, missing = upscale.upscale_folder(make_tool(root), src, dst, root / "work" / "upscale",
                                                   upscale.Settings("photo", 1), fake,
                                                   Log(sinks=[lambda kind, text, frac: log_lines.append(text)]),
                                                   threading.Event())
            self.assertEqual((done, missing), (2, 0))
            self.assertNotEqual(fake.calls[0]["src"], src)       # staged: the folder has others
            self.assertFalse((dst / "lm0000.png").exists())
            self.assertFalse((dst / "brick_n.png").exists())
            self.assertTrue(any("normal, specular and detail maps stay as the game has them" in t
                                for t in log_lines))
            # An older rip (6 columns, no kind): nothing, and the folder is
            # refused until the game is ripped again.
            (src / upscale.KEYS_FILE).write_text("00000000000000aa\t64\t64\t18\tbrick\tbrick\n")
            self.assertEqual(upscale.sources(src), [])
            with self.assertRaises(SetupError):
                upscale.upscale_folder(make_tool(root / "again"), src, dst, root / "work" / "upscale",
                                       upscale.Settings("photo", 1), fake, Log(sinks=[]), threading.Event())
            # The kinds survive validation; unknown names are dropped.
            self.assertEqual(upscale.Settings(kinds=frozenset({"normal", "colour"})).validated().kinds,
                             frozenset({"normal"}))
            self.assertEqual(upscale.Settings(kinds=frozenset()).validated().kinds, upscale.DEFAULT_KINDS)

    def test_an_old_pack_that_shares_pictures_is_repaired(self):
        with tempfile.TemporaryDirectory() as tmp:
            pack = Path(tmp)
            index = pack / upscale.PACK_INDEX
            index.write_text("# key\twidth\theight\txenos_format\tfile\n"
                             "00000000000000aa\t512\t512\t20\tlm0000.dds\n"
                             "00000000000000ab\t512\t512\t20\tlm0000.dds\n"
                             "00000000000000b0\t64\t64\t18\twall.dds\n"
                             "00000000000000b1\t64\t64\t18\twall.dds\n"
                             "00000000000000c0\t256\t8\t6\tfogramp.dds\n"
                             "00000000000000d0\t64\t64\t18\tdoor.dds\n")
            self.assertTrue(upscale.repair_old_pack(pack, Log(sinks=[])))
            rows = [line for line in index.read_text().splitlines() if not line.startswith("#")]
            self.assertEqual(rows, ["00000000000000d0\t64\t64\t18\tdoor.dds"])
            self.assertIn("lm0000.dds", (pack / "index-before-fix.tsv").read_text())
            # A good index (each file one key) is left alone.
            self.assertFalse(upscale.repair_old_pack(pack, Log(sinks=[])))
            self.assertFalse(upscale.repair_old_pack(pack / "missing", Log(sinks=[])))

    def test_only_missing_and_normal_maps_without_denoise(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, dst = root / "textures", root / "textures_upscaled"
            src.mkdir()
            dst.mkdir()
            for name in ("brick", "brick_n", "snow", "flag"):
                (src / f"{name}.png").write_bytes(b"png")
            write_keys(src, ["brick", "brick_n", "snow", "flag"], {"brick_n": "normal"})
            (dst / "snow.png").write_bytes(b"done before")
            future = time.time() + 60
            os.utime(dst / "snow.png", (future, future))
            # A texture ripped again since its copy was made is done again.
            (dst / "flag.png").write_bytes(b"old")
            past = time.time() - 3600
            os.utime(dst / "flag.png", (past, past))
            fake = FakeWaifu2x()
            staging = root / "work" / "upscale"
            done, missing = upscale.upscale_folder(make_tool(root), src, dst, staging,
                                                   upscale.Settings("cunet", 2, kinds=BOTH), fake,
                                                   Log(sinks=[]), threading.Event())
            self.assertEqual((done, missing), (3, 0))
            by_noise = {c["noise"]: c["files"] for c in fake.calls}
            self.assertEqual(by_noise, {2: ["brick.png", "flag.png"], -1: ["brick_n.png"]})
            self.assertTrue(all(c["src"] != src for c in fake.calls))   # staged
            self.assertEqual((dst / "snow.png").read_bytes(), b"done before")
            self.assertFalse(staging.exists())                        # cleaned up
            self.assertEqual(sorted(p.name for p in src.iterdir()),   # originals untouched
                             ["brick.png", "brick_n.png", "flag.png", "hd_keys.tsv", "snow.png"])

    def test_the_chosen_scale_goes_to_waifu2x_and_is_recorded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, dst = root / "textures", root / "textures_upscaled"
            src.mkdir()
            (src / "brick.png").write_bytes(png_bytes(64, 32))
            (src / "brick_n.png").write_bytes(png_bytes(64, 32))
            write_keys(src, ["brick", "brick_n"], {"brick_n": "normal"})
            fake = FakeWaifu2x()
            done, missing = upscale.upscale_folder(make_tool(root), src, dst, root / "work" / "upscale",
                                                   upscale.Settings("photo", 1, 4, BOTH), fake, Log(sinks=[]),
                                                   threading.Event())
            self.assertEqual((done, missing), (2, 0))
            self.assertEqual({c["scale"] for c in fake.calls}, {4})
            self.assertEqual(upscale.png_size(dst / "brick.png"), (256, 128))
            self.assertEqual(upscale.recorded_scale(dst), 4)

    def test_a_new_scale_redoes_the_copies_but_keeps_those_already_made(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, dst = root / "textures", root / "textures_upscaled"
            src.mkdir()
            dst.mkdir()
            for name in ("a", "b", "c"):
                (src / f"{name}.png").write_bytes(png_bytes(16, 8))
            write_keys(src, ["a", "b", "c"])
            # A 2x build, then an interrupted 4x one that made b.
            (dst / "a.png").write_bytes(png_bytes(32, 16))
            (dst / "b.png").write_bytes(png_bytes(64, 32))
            (dst / "c.png").write_bytes(png_bytes(32, 16))
            future = time.time() + 60
            for name in ("a", "b", "c"):
                os.utime(dst / f"{name}.png", (future, future))
            upscale.record_scale(dst, 2)
            fake = FakeWaifu2x()
            log_lines = []
            done, missing = upscale.upscale_folder(
                make_tool(root), src, dst, root / "work" / "upscale", upscale.Settings("photo", 1, 4), fake,
                Log(sinks=[lambda kind, text, frac: log_lines.append(text)]), threading.Event())
            self.assertEqual((done, missing), (2, 0))
            self.assertEqual(fake.calls[0]["files"], ["a.png", "c.png"])
            self.assertEqual(upscale.png_size(dst / "a.png"), (64, 32))
            self.assertEqual(upscale.recorded_scale(dst), 4)
            self.assertTrue(any("made at 2x, so they are made again at 4x" in t for t in log_lines))
            # Back at 4x next time: nothing to do.
            done, missing = upscale.upscale_folder(make_tool(root / "again"), src, dst, root / "work" / "upscale",
                                                   upscale.Settings("photo", 1, 4), fake, Log(sinks=[]),
                                                   threading.Event())
            self.assertEqual((done, missing, len(fake.calls)), (0, 0, 1))

    def test_1x_reduces_noise_with_cunet_and_copies_what_it_would_not_change(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, dst = root / "textures", root / "textures_upscaled"
            src.mkdir()
            for name in ("brick", "brick_n"):
                (src / f"{name}.png").write_bytes(png_bytes(32, 32))
            write_keys(src, ["brick", "brick_n"], {"brick_n": "normal"})
            fake = FakeWaifu2x()
            done, missing = upscale.upscale_folder(make_tool(root), src, dst, root / "work" / "upscale",
                                                   upscale.Settings("photo", 2, 1, BOTH), fake, Log(sinks=[]),
                                                   threading.Event())
            self.assertEqual((done, missing), (2, 0))
            # Only the color texture goes to waifu2x (cunet: the only model
            # with denoise-only networks); the normal map is copied as is.
            self.assertEqual([(c["files"], c["model"], c["noise"], c["scale"]) for c in fake.calls],
                             [(["brick.png"], "models-cunet", 2, 1)])
            self.assertEqual((dst / "brick_n.png").read_bytes(), (src / "brick_n.png").read_bytes())
        with tempfile.TemporaryDirectory() as tmp:
            # No noise reduction at 1x: waifu2x would change nothing.
            root = Path(tmp)
            src, dst = root / "textures", root / "out"
            src.mkdir()
            (src / "brick.png").write_bytes(png_bytes(32, 32))
            write_keys(src, ["brick"])
            fake = FakeWaifu2x()
            done, missing = upscale.upscale_folder(make_tool(root), src, dst, root / "work" / "upscale",
                                                   upscale.Settings("cunet", -1, 1), fake, Log(sinks=[]),
                                                   threading.Event())
            self.assertEqual((done, missing, fake.calls), (1, 0, []))
            self.assertEqual(upscale.recorded_scale(dst), 1)

    def test_a_textures_2x_folder_is_renamed_never_deleted(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            old = data / upscale.LEGACY_OUTPUT_NAME
            old.mkdir()
            (old / "brick.png").write_bytes(b"made at 2x")
            out = upscale.output_dir(data, Log(sinks=[]))
            self.assertEqual(out, data / "textures_upscaled")
            self.assertFalse(old.exists())
            self.assertEqual((out / "brick.png").read_bytes(), b"made at 2x")
            self.assertEqual(upscale.recorded_scale(out), 2)
            # Both there (the old one came back): both are left alone.
            old.mkdir()
            (old / "x.png").write_bytes(b"x")
            self.assertEqual(upscale.output_dir(data, Log(sinks=[])), out)
            self.assertTrue((old / "x.png").is_file())
            self.assertTrue((out / "brick.png").is_file())
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(upscale.output_dir(Path(tmp), Log(sinks=[])), Path(tmp) / "textures_upscaled")
            self.assertEqual(list(Path(tmp).iterdir()), [])   # nothing made yet

    def test_cancel_stops_before_waifu2x(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "textures"
            src.mkdir()
            (src / "a.png").write_bytes(b"png")
            write_keys(src, ["a"])
            cancel = threading.Event()
            cancel.set()
            fake = FakeWaifu2x()
            with self.assertRaises(pipeline.Cancelled):
                upscale.upscale_folder(make_tool(root), src, root / "out", root / "work" / "upscale",
                                       upscale.Settings(), fake, Log(sinks=[]), cancel)
            self.assertEqual(fake.calls, [])


class UpscaleStepTests(unittest.TestCase):
    def titles(self, **opts) -> list:
        with tempfile.TemporaryDirectory() as tmp:
            o = BuildOptions(output_dir=str(Path(tmp) / "out"), work_dir=str(Path(tmp) / "work"),
                             build_recomp=False, **opts)
            with mock.patch.object(pipeline, "IS_LINUX", True), mock.patch.object(pipeline, "IS_WINDOWS", False):
                return [s.title for s in Pipeline(o, Log(sinks=[]), threading.Event()).steps()]

    def test_step_after_the_rip_then_the_pack(self):
        t = self.titles(upscale_textures=True)
        i = t.index("Upscaling textures (waifu2x)")
        self.assertEqual(t[i - 1], "Ripping Call of Duty 3")
        self.assertEqual(t[i + 1], "HD texture pack")
        self.assertEqual(t[i + 2], "User texture pack")   # c3w/user_textures.py
        self.assertEqual(t[i + 3], "World at War")

    def test_off_by_default_and_never_in_game_only_builds(self):
        for title in ("Upscaling textures (waifu2x)", "HD texture pack"):
            self.assertNotIn(title, self.titles())
            self.assertNotIn(title, self.titles(upscale_textures=True, binaries_only=True))

    def test_disk_space_counts_the_pack_and_grows_with_the_scale(self):
        for scale in upscale.SCALES:
            self.assertGreater(pipeline.hd_pack_size(scale), 0)
            self.assertEqual(pipeline.upscaled_size(scale),
                             pipeline.upscaled_png_size(scale) + pipeline.hd_pack_size(scale))
        self.assertEqual(pipeline.upscaled_size(4), 4 * pipeline.upscaled_size(2))
        self.assertEqual(pipeline.upscaled_size(2), 4 * pipeline.upscaled_size(1))

    def test_the_scale_reaches_the_build_from_the_command_line(self):
        import cod3_waw_setup
        seen = []

        class FakePipeline:
            def __init__(self, opts, log, cancel):
                seen.append(opts)

            def run(self):
                pass
        ask = pipeline.APPROVAL.ask if hasattr(pipeline, "APPROVAL") else None
        try:
            for extra, scale in (([], 2), (["--upscale-scale", "4"], 4), (["--upscale-scale", "1"], 1)):
                argv = ["setup", "build", "--iso", "game.iso", "--no-waw", "--no-recomp", "--upscale-textures",
                        "--yes"] + extra
                with mock.patch.object(sys, "argv", argv), mock.patch.object(cod3_waw_setup, "Pipeline", FakePipeline), \
                        mock.patch("builtins.print"):
                    self.assertEqual(cod3_waw_setup.main(), 0)
                self.assertEqual(seen[-1].upscale_scale, scale)
            with mock.patch.object(sys, "argv", ["setup", "build", "--upscale-scale", "3"]), \
                    mock.patch("sys.stderr"), self.assertRaises(SystemExit):
                cod3_waw_setup.main()   # waifu2x has no 3x
        finally:
            if ask is not None:
                pipeline.APPROVAL.ask = ask


class FakeRip:
    """Stands in for Runner.run(rip cod3-hd-pack ...): writes the pack the
    ripper would write (one DDS per upscaled PNG and index.tsv) and prints
    progress, or fails the way it's told to."""

    def __init__(self, rc=0, raises=None, write_index=True):
        self.calls = []
        self.rc, self.raises, self.write_index = rc, raises, write_index

    def run(self, cmd, quiet=False, check=True, line_filter=None, **kw):
        self.calls.append([str(c) for c in cmd])
        if self.raises:
            raise self.raises
        textures, upscaled, out = (Path(c) for c in cmd[2:5])
        pngs = sorted(upscaled.glob("*.png"))
        for i, png in enumerate(pngs, 1):
            (out / (png.stem + ".dds")).write_bytes(b"DDS ")
            if line_filter:
                line_filter(f"[{i}/{len(pngs)}] {png.stem}.dds")
        if self.write_index:
            lines = ["#key\twidth\theight\tformat\tfile"]
            lines += [f"{i:016x}\t64\t64\t18\t{p.stem}.dds" for i, p in enumerate(pngs)]
            lines.append(f"{999:016x}\t64\t64\t18\t{pngs[0].stem}.dds")   # a second key for one file
            (out / upscale.PACK_INDEX).write_text("\n".join(lines) + "\n", encoding="utf-8")
        if line_filter:
            line_filter(f"done: {len(pngs)} packed, 0 skipped")
        return self.rc


class HdPackStepTests(unittest.TestCase):
    """The "HD texture pack" step: `rip cod3-hd-pack` after waifu2x, tolerant
    of failures like the upscaling itself."""

    def pipeline(self, tmp: Path, runner, keys=True, upscaled=("brick", "snow")):
        out = tmp / "out"
        textures = out / "data" / "cod3" / "textures"
        textures.mkdir(parents=True)
        (textures / "brick.png").write_bytes(b"png")
        if keys:
            write_keys(textures, ["brick"])
        if upscaled:
            (textures.parent / upscale.OUTPUT_NAME).mkdir()
            for name in upscaled:
                (textures.parent / upscale.OUTPUT_NAME / f"{name}.png").write_bytes(b"png")
        self.events = []
        log = Log(sinks=[lambda kind, text, frac: self.events.append((kind, text, frac))])
        p = Pipeline(BuildOptions(output_dir=str(out), work_dir=str(tmp / "work"), upscale_textures=True, jobs=3),
                     log, threading.Event())
        p.rip, p.runner = tmp / "rip", runner
        return p, textures

    def warnings(self) -> list:
        return [text for kind, text, _ in self.events if kind == "warn"]

    def test_runs_the_pack_command_with_the_right_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake = FakeRip()
            p, textures = self.pipeline(Path(tmp), fake)
            p.hd_texture_pack()
            data = textures.parent
            self.assertEqual(fake.calls, [[str(Path(tmp) / "rip"), "cod3-hd-pack", str(data / "textures"),
                                           str(data / "textures_upscaled"), str(data / "hd_textures"), "--jobs",
                                           "3"]])
            self.assertTrue(upscale.pack_ready(data / "hd_textures"))
            self.assertEqual(upscale.pack_size(data / "hd_textures"), 2)   # files, not keys
            self.assertEqual(self.warnings(), [])
            progress = [(text, frac) for kind, text, frac in self.events if kind == "progress"]
            self.assertIn(("packed 2/2 HD textures", 1.0), progress)
            self.assertTrue(any(kind == "out" and text.startswith("done:") for kind, text, _ in self.events))

    def test_manifest_records_the_pack_only_when_it_exists(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, textures = self.pipeline(Path(tmp), FakeRip())
            p.finish()
            manifest = json.loads((p.out / "cod3_waw.json").read_text(encoding="utf-8"))
            self.assertIsNone(manifest["cod3_hd_textures"])
            p.hd_texture_pack()
            p.finish()
            manifest = json.loads((p.out / "cod3_waw.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["cod3_hd_textures"], "data/cod3/hd_textures")
            self.assertEqual(manifest["cod3_textures_upscaled"], "data/cod3/textures_upscaled")
            self.assertNotIn("cod3_textures_2x", manifest)

    def test_copies_from_an_earlier_2x_build_are_moved_and_packed(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake = FakeRip()
            p, textures = self.pipeline(Path(tmp), fake, upscaled=())
            old = textures.parent / upscale.LEGACY_OUTPUT_NAME
            old.mkdir()
            (old / "brick.png").write_bytes(b"png")
            p.hd_texture_pack()
            new = textures.parent / upscale.OUTPUT_NAME
            self.assertEqual(fake.calls[0][3], str(new))
            self.assertTrue((new / "brick.png").is_file())
            self.assertFalse(old.exists())
            self.assertTrue(upscale.pack_ready(textures.parent / upscale.PACK_NAME))

    def test_a_failure_warns_and_the_build_goes_on(self):
        for fake in (FakeRip(rc=1, write_index=False),                       # the ripper stopped
                     FakeRip(raises=SetupError("Program not found: rip")),   # no ripper at all
                     FakeRip(rc=0, write_index=False)):                     # finished without an index
            with self.subTest(calls=fake.rc), tempfile.TemporaryDirectory() as tmp:
                p, _ = self.pipeline(Path(tmp), fake)
                p.hd_texture_pack()   # no exception
                self.assertEqual(len(fake.calls), 1)
                self.assertEqual(len(self.warnings()), 1)
                self.assertIn("HD texture pack wasn't made", self.warnings()[0])
                self.assertIn("uses its own textures", self.warnings()[0])

    def test_an_earlier_pack_stays_after_a_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, textures = self.pipeline(Path(tmp), FakeRip(rc=1, write_index=False))
            pack = textures.parent / upscale.PACK_NAME
            pack.mkdir()
            (pack / upscale.PACK_INDEX).write_text("#\n", encoding="utf-8")
            p.hd_texture_pack()
            self.assertIn("earlier build", self.warnings()[0])
            self.assertTrue(upscale.pack_ready(pack))

    def test_cancel_is_not_swallowed(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, _ = self.pipeline(Path(tmp), FakeRip(raises=pipeline.Cancelled()))
            with self.assertRaises(pipeline.Cancelled):
                p.hd_texture_pack()

    def test_nothing_to_pack(self):
        # No upscaled copies (waifu2x failed and never ran before), or a rip
        # from a ripper without hd_keys.tsv: the ripper isn't started.
        for kwargs in ({"upscaled": ()}, {"keys": False}):
            with self.subTest(**kwargs), tempfile.TemporaryDirectory() as tmp:
                fake = FakeRip()
                p, _ = self.pipeline(Path(tmp), fake, **kwargs)
                p.hd_texture_pack()
                self.assertEqual(fake.calls, [])
                self.assertEqual(len(self.warnings()), 1)

    def test_progress_shapes(self):
        got = []
        handle = rippers._pack_progress(Log(sinks=[lambda kind, text, frac: got.append((kind, text, frac))]))
        # A count first, then one line per written file ...
        for line in ("4 textures to pack, 12 worker threads", "a.dds", "b.dds",
                     "error: c.png is 100x100, expected 128x128", "skipped 1 wrong size"):
            handle(line)
        self.assertIn(("progress", "packed 2/4 HD textures", 0.5), got)
        self.assertIn(("out", "error: c.png is 100x100, expected 128x128", None), got)
        self.assertIn(("out", "skipped 1 wrong size", None), got)
        self.assertNotIn(("out", "a.dds", None), got)   # thousands of these: not logged
        # ... or a done/total pair.
        got.clear()
        handle("packed 30/120 cloth$burlap_n.dds")
        self.assertEqual(got, [("progress", "packed 30/120 HD textures", 0.25)])

    def test_prebuilt_ripper_must_have_the_new_layout(self):
        class Capture:
            def __init__(self, text):
                self.text = text

            def capture(self, cmd, **kw):
                return 0, self.text
        # 6: hd_keys.tsv's kind column (only color pictures are upscaled);
        # an older ripper (which gave several textures one picture, or
        # upscaled normal maps and read small textures from the wrong
        # place) is rebuilt and the game ripped again.
        self.assertEqual(rippers.RIP_OUTPUT_VERSION, 6)
        self.assertFalse(rippers._works(Path("rip"), Capture("rip 0.1.0, output layout 5")))
        self.assertTrue(rippers._works(Path("rip"), Capture("rip 0.1.0, output layout 6")))


if __name__ == "__main__":
    unittest.main()

"""[acct2-decomp] Tests for c3w/censor_builtin.py: the built-in censor mode.
Made-up pictures and regions only; no game data (the shipped list is only parsed and counted).

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import random
import shutil
import struct
import sys
import tempfile
import threading
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import censor_builtin as cb  # noqa: E402
from c3w import user_textures  # noqa: E402
from c3w.common import Log, SetupError  # noqa: E402

TOOL = Path(__file__).resolve().parent.parent


def _png_with_filters(w, h, rgba, filters, bpp=4):
    """A PNG written with the given filter per row (to test reading every filter)."""
    stride = w * bpp
    raw, prev = bytearray(), bytes(stride)
    for y in range(h):
        line = bytes(rgba[y * stride:(y + 1) * stride])
        f = filters[y % len(filters)]
        out = bytearray(stride)
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 0:
                p = 0
            elif f == 1:
                p = a
            elif f == 2:
                p = b
            elif f == 3:
                p = (a + b) >> 1
            else:
                q = a + b - c
                pa, pb, pc = abs(q - a), abs(q - b), abs(q - c)
                p = a if pa <= pb and pa <= pc else b if pb <= pc else c
            out[i] = (line[i] - p) & 0xFF
        raw += bytes([f]) + out
        prev = line

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    ctype = {4: 6, 3: 2, 1: 0, 2: 4}[bpp]
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


class Png(unittest.TestCase):
    def setUp(self):
        self.pil = cb._PIL
        cb._PIL = None              # the plain-Python path

    def tearDown(self):
        cb._PIL = self.pil

    def test_every_filter_reads_back(self):
        rnd = random.Random(7)
        for bpp in (4, 3, 1, 2):
            w, h = 37, 11
            px = bytes(rnd.randrange(256) for _ in range(w * h * bpp))
            d = Path(tempfile.mkdtemp())
            try:
                p = d / "t.png"
                p.write_bytes(_png_with_filters(w, h, px, [0, 1, 2, 3, 4], bpp))
                rw, rh, rgba = cb.read_png(p)
                self.assertEqual((rw, rh), (w, h))
                for i in range(w * h):
                    src = px[i * bpp:(i + 1) * bpp]
                    want = {4: src, 3: src + b"\xff", 1: src * 3 + b"\xff", 2: src[:1] * 3 + src[1:]}[bpp]
                    self.assertEqual(bytes(rgba[i * 4:i * 4 + 4]), want, (bpp, i))
            finally:
                shutil.rmtree(d)

    def test_write_then_read(self):
        d = Path(tempfile.mkdtemp())
        try:
            px = bytearray(range(256)) * 4
            cb.write_png(d / "a.png", 16, 16, px)
            self.assertEqual(cb.read_png(d / "a.png"), (16, 16, px))
            self.assertFalse((d / "a.png.part").exists())
        finally:
            shutil.rmtree(d)


class Designs(unittest.TestCase):
    def test_stripes(self):
        self.assertEqual(cb.design_color("rainbow", 0.5, 0.01), (0xE4, 0x03, 0x03))
        self.assertEqual(cb.design_color("rainbow", 0.5, 0.99), (0x75, 0x07, 0x87))
        self.assertEqual(cb.design_color("trans", 0.5, 0.5), (0xFF, 0xFF, 0xFF))
        self.assertEqual(cb.design_color("bi", 0.5, 0.5), (0x9B, 0x4F, 0x96))      # 2:1:2
        self.assertEqual(cb.design_color("bi", 0.5, 0.39), (0xD6, 0x02, 0x70))
        self.assertEqual(cb.design_color("ace", 0.1, 0.0), (0, 0, 0))

    def test_progress_chevron_and_field(self):
        self.assertEqual(cb.design_color("progress", 0.0, 0.5), (0xFF, 0xFF, 0xFF))   # the hoist's point
        self.assertEqual(cb.design_color("progress", 0.95, 0.05), (0xE4, 0x03, 0x03))  # the rainbow field

    def test_plain_and_custom(self):
        self.assertEqual(cb.design_color("black", 0.3, 0.3), cb.PLAIN["black"])
        self.assertEqual(cb.design_color("custom", 0.3, 0.3, (1, 2, 3)), (1, 2, 3))
        self.assertEqual(cb.parse_color("#3366cc"), (0x33, 0x66, 0xCC))
        with self.assertRaises(SetupError):
            cb.parse_color("blue")

    def test_every_design_has_a_label(self):
        for k in cb.DESIGN_KEYS:
            self.assertTrue(cb.LABELS[k])
            cb.Settings(flags=k, insignia=k).check()


class Regions(unittest.TestCase):
    def test_shipped_list_parses(self):
        text = cb.regions_path(TOOL).read_text(encoding="utf-8")
        rows = [l for l in text.splitlines() if l and not l.startswith("#")]
        regs = cb.read_regions(TOOL)
        self.assertEqual(len(regs), len(rows))                      # every shipped row is valid
        self.assertGreaterEqual(len(regs), 50)
        self.assertEqual({r.role for r in regs}, {cb.FLAG, cb.INSIGNIA, cb.DATA})
        self.assertEqual(len({r.key for r in regs}), len(regs))
        for r in regs:
            self.assertTrue(r.rects or r.role == cb.FLAG, r.name)
            self.assertTrue(r.where == "campaign" or r.where.startswith("mp/"), r.where)

    def test_bad_rows_are_skipped(self):
        good = "0123456789abcdef\tx\t8\t8\tinsignia\t1,1,7,7\tcampaign"
        text = "\n".join([good, good.replace("1,1,7,7", "1,1,9,4"), good.replace("insignia", "other"),
                          "short\trow", good.replace("\t8\t8\t", "\t0\t8\t"),
                          good.replace("campaign", "mp/../x.cod"), good.replace("1,1,7,7", ""),
                          good.replace("0123456789abcdef", "0123456789abcdeg")])
        regs = cb.parse_regions(text)
        self.assertEqual(len(regs), 1)
        r = regs[0]
        self.assertEqual(r.rects_at(16, 16), [(2, 2, 14, 14)])     # scaled 2x
        m = r.mask_at(8, 8)
        self.assertTrue(m[4 * 8 + 4] and m[1 * 8 + 4] and m[4 * 8 + 1])   # the middle and the edges' middles
        self.assertFalse(m[0] or m[7 * 8 + 7])                          # outside
        self.assertEqual(sum(r.mask_at(16, 16)), 4 * sum(m))
        big = cb.Region("0" * 16, "x", 40, 40, cb.INSIGNIA, [(0, 0, 40, 40)])
        mb = big.mask_at(40, 40)
        self.assertFalse(mb[0] or mb[39] or mb[39 * 40])                  # rounded corners
        self.assertTrue(mb[20 * 40] and mb[20 * 40 + 39] and mb[20])      # the edges' middles


class Painting(unittest.TestCase):
    def test_inpaint_takes_the_symbol_off(self):
        w = h = 16
        px = bytearray([100, 50, 25, 255] * (w * h))
        mask = bytearray(w * h)
        for y in range(5, 11):
            for x in range(5, 11):
                o = (y * w + x) * 4
                px[o:o + 4] = bytes([0, 0, 0, 200])
                mask[y * w + x] = 1
        self.assertEqual(cb.inpaint(px, w, h, mask), 36)
        for y in range(5, 11):
            for x in range(5, 11):
                o = (y * w + x) * 4
                self.assertEqual(bytes(px[o:o + 4]), bytes([100, 50, 25, 200]))   # colour from around, alpha kept

    def test_rect_design(self):
        w = h = 8
        px = bytearray([200] * (w * h * 4))
        cb.paint_rects(px, w, h, "white", (0, 0, 0), [(2, 2, 6, 6)])
        self.assertEqual(bytes(px[(3 * w + 3) * 4:(3 * w + 3) * 4 + 3]), bytes(cb.PLAIN["white"]))
        self.assertEqual(bytes(px[0:4]), bytes([200] * 4))

    def test_flag_keeps_alpha_and_covers_everything(self):
        w, h = 24, 12
        px = bytearray()
        for i in range(w * h):
            px += bytes([180, 30, 30, 128 if i < w else 255])
        mask = bytearray(w * h)
        cb.paint_flag(px, w, h, "trans", (0, 0, 0), mask, [])
        self.assertTrue(all(px[i * 4 + 3] == (128 if i < w else 255) for i in range(w * h)))
        mid = (6 * w + 12) * 4
        self.assertGreater(min(px[mid:mid + 3]), 200)          # the trans flag's white middle stripe


def _tile_dxt1(lin: bytes, wb: int, hb: int) -> bytes:
    """The GPU layout of a linear DXT1 base level: tiled, 8-in-16 swapped (as the game keeps it)."""
    size = max(cb._tiled(x, y, wb, 8) for y in range(hb) for x in range(wb)) + 1
    gpu = bytearray(size * 8)
    for y in range(hb):
        for x in range(wb):
            i = cb._tiled(x, y, wb, 8) * 8
            gpu[i:i + 8] = lin[(y * wb + x) * 8:(y * wb + x + 1) * 8]
    gpu[0::2], gpu[1::2] = gpu[1::2], gpu[0::2]
    return bytes(gpu)


def _fetch_obj(width: int, height: int, fmt: int = 18) -> bytes:
    w0 = (1 << 31) | ((width // 32) << 22) | 2
    w1 = fmt | (1 << 6)
    w2 = (width - 1) | ((height - 1) << 13)
    w5 = 1 << 9
    return bytes(0x48) + struct.pack(">6I", w0, w1, w2, 0, 0, w5)


class Multiplayer(unittest.TestCase):
    def test_xxh64(self):
        self.assertEqual(cb.xxh64(b""), 0xEF46DB3751D8E999)
        self.assertEqual(cb.xxh64(b"a"), 0xD24EC4F1A98C6E5B)
        self.assertEqual(cb.xxh64(b"abc"), 0x44BC2CF5AD770999)
        self.assertEqual(cb.xxh64(b"Nobody inspects the spammish repetition"), 0xFBCEA83C8A378BF1)

    def test_untile_and_decode(self):
        wb = hb = 32                                   # a 128 x 128 DXT1 texture
        rnd = random.Random(3)
        lin = bytearray()
        for _ in range(wb * hb):
            c0, c1 = sorted((rnd.randrange(65536), rnd.randrange(65536)), reverse=True)
            lin += struct.pack("<HHI", c0, c1, rnd.randrange(1 << 32))
        b = cb.base_level(_fetch_obj(128, 128), _tile_dxt1(bytes(lin), wb, hb))
        self.assertEqual(b[:3], (128, 128, 18))
        self.assertEqual(b[3], bytes(lin))
        # A block of one colour (white, index 0 everywhere) decodes to that colour.
        one = struct.pack("<HHI", 0xFFFF, 0, 0) * 4
        px = cb.decode_dxt(one, 8, 8, 18)
        self.assertEqual(bytes(px[:4]), b"\xff\xff\xff\xff")
        # DXT1 with c0 <= c1: index 3 is see-through black.
        cut = struct.pack("<HHI", 0, 0xFFFF, 0xFFFFFFFF)
        self.assertEqual(bytes(cb.decode_dxt(cut, 4, 4, 18)[:4]), b"\x00\x00\x00\x00")

    def test_originals_go_into_the_rip_and_out_again(self):
        d = Path(tempfile.mkdtemp())
        try:
            tex = d / "data" / "cod3" / "textures"
            tex.mkdir(parents=True)
            keys = "# key\twidth\theight\txenos_format\tname\tfile\tkind\nabc\n"
            (tex / "hd_keys.tsv").write_text(keys)
            reg = cb.Region("0123456789abcdef", "flag", 8, 4, cb.FLAG, [(1, 1, 7, 3)], "mp/x.cod")
            pic = (8, 4, 19, bytearray([10, 20, 30, 255] * 32))
            cb.sync_mp_originals(d / "data" / "cod3", {"flag__0123456789abcdef": (reg, pic)})
            self.assertEqual(cb.key_files(tex), {"0123456789abcdef": "flag__0123456789abcdef"})
            self.assertTrue((tex / "flag__0123456789abcdef.png").is_file())
            self.assertIn("0123456789abcdef\t8\t4\t19\tflag\tflag__0123456789abcdef\tdiffuse",
                          (tex / "hd_keys.tsv").read_text())
            # Again, without the picture (already there): nothing doubles.
            cb.sync_mp_originals(d / "data" / "cod3", {"flag__0123456789abcdef": (reg, None)})
            self.assertEqual((tex / "hd_keys.tsv").read_text().count("flag__"), 1)
            # Not wanted any more: the line, the picture and the list go.
            cb.sync_mp_originals(d / "data" / "cod3", {})
            self.assertEqual((tex / "hd_keys.tsv").read_text(), keys)
            self.assertFalse((tex / "flag__0123456789abcdef.png").exists())
            self.assertFalse((tex / cb.MP_MANIFEST_NAME).exists())
        finally:
            shutil.rmtree(d)


class Apply(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp())
        self.tool = self.d / "tool"
        (self.tool / "lists").mkdir(parents=True)
        (self.tool / "lists" / "censor_regions.tsv").write_text(
            "# test\n"
            "aaaaaaaaaaaaaaaa\tbanner\t8\t8\tflag\t2,2,6,6\tcampaign\n"
            "bbbbbbbbbbbbbbbb\thelmet\t8\t8\tinsignia\t1,1,7,7\tcampaign\n"
            "cccccccccccccccc\thelmet_n\t8\t8\tdata\t1,1,7,7\tcampaign\n"
            "dddddddddddddddd\tmp_only\t8\t8\tinsignia\t1,1,7,7\tmp/x.cod\n")
        self.data = self.d / "out" / "data" / "cod3"
        tex = self.data / "textures"
        tex.mkdir(parents=True)
        (tex / "hd_keys.tsv").write_text(
            "# key\twidth\theight\txenos_format\tname\tfile\tkind\n"
            "aaaaaaaaaaaaaaaa\t8\t8\t18\tbanner\tbanner\tdiffuse\n"
            "bbbbbbbbbbbbbbbb\t8\t8\t18\thelmet\thelmet\tdiffuse\n"
            "cccccccccccccccc\t8\t8\t20\thelmet_n\thelmet_n\tnormal\n")
        for n in ("banner", "helmet", "helmet_n"):
            cb.write_png(tex / f"{n}.png", 8, 8, bytearray([90, 90, 90, 255] * 64))
        self.log = Log(sinks=[])

    def tearDown(self):
        shutil.rmtree(self.d)

    def folder(self):
        return self.data / "user_textures_png"

    def test_on_change_off(self):
        r = cb.apply(self.data, self.tool, cb.Settings(flags="rainbow", insignia="blend"), self.log)
        self.assertEqual(sorted(r.made), ["banner", "helmet", "helmet_n"])
        self.assertEqual(r.missing, ["mp_only"])
        self.assertEqual(sorted(cb.read_manifest(self.folder())), ["banner", "helmet", "helmet_n"])
        self.assertEqual(cb.load_settings(self.data).flags, "rainbow")
        # Same settings again: nothing remade.
        r = cb.apply(self.data, self.tool, cb.Settings(flags="rainbow", insignia="blend"), self.log)
        self.assertEqual(r.made, [])
        # Flags off: the banner's picture goes, the others stay.
        r = cb.apply(self.data, self.tool, cb.Settings(flags="off", insignia="black"), self.log)
        self.assertEqual(r.removed, ["banner"])
        self.assertEqual(sorted(r.made), ["helmet"])          # the normal map is "blend" either way
        self.assertFalse((self.folder() / "banner.png").exists())
        # All off: every picture it made goes, and the list with them.
        r = cb.apply(self.data, self.tool, cb.Settings(), self.log)
        self.assertEqual(sorted(r.removed), ["helmet", "helmet_n"])
        self.assertFalse((self.folder() / cb.MANIFEST_NAME).exists())

    def test_the_players_own_picture_wins(self):
        self.folder().mkdir(parents=True)
        own = self.folder() / "helmet.png"
        own.write_bytes(b"my picture")
        r = cb.apply(self.data, self.tool, cb.Settings(insignia="rainbow"), self.log)
        self.assertEqual(r.kept, ["helmet"])
        self.assertEqual(own.read_bytes(), b"my picture")
        cb.apply(self.data, self.tool, cb.Settings(), self.log)
        self.assertEqual(own.read_bytes(), b"my picture")      # never removed either

    def test_a_picture_changed_by_hand_becomes_the_players(self):
        cb.apply(self.data, self.tool, cb.Settings(insignia="grey"), self.log)
        p = self.folder() / "helmet.png"
        p.write_bytes(p.read_bytes() + b"x")
        r = cb.apply(self.data, self.tool, cb.Settings(), self.log)
        self.assertNotIn("helmet", r.removed)
        self.assertTrue(p.exists())

    def test_hd_copy_is_painted_at_its_size(self):
        up = self.data / "textures_upscaled"
        up.mkdir()
        cb.write_png(up / "helmet.png", 16, 16, bytearray([90, 90, 90, 255] * 256))
        cb.apply(self.data, self.tool, cb.Settings(insignia="white"), self.log)
        w, h, px = cb.read_png(self.folder() / "helmet.png")
        self.assertEqual((w, h), (16, 16))
        self.assertEqual(bytes(px[(4 * 16 + 4) * 4:(4 * 16 + 4) * 4 + 3]), bytes(cb.PLAIN["white"]))

    def test_needs_a_rip(self):
        shutil.rmtree(self.data / "textures")
        with self.assertRaises(SetupError):
            cb.apply(self.data, self.tool, cb.Settings(flags="rainbow"), self.log)
        cb.apply(self.data, self.tool, cb.Settings(), self.log)   # turning off works without one

    def test_a_new_hd_scale_makes_the_picture_again(self):
        up = self.data / "textures_upscaled"
        up.mkdir()
        cb.write_png(up / "helmet.png", 16, 16, bytearray([90, 90, 90, 255] * 256))
        cb.apply(self.data, self.tool, cb.Settings(insignia="black"), self.log)
        cb.write_png(up / "helmet.png", 32, 32, bytearray([90, 90, 90, 255] * 1024))
        r = cb.apply(self.data, self.tool, cb.Settings(insignia="black"), self.log)
        self.assertIn("helmet", r.made)
        self.assertEqual(cb.read_png(self.folder() / "helmet.png")[:2], (32, 32))

    def test_made_names_are_only_the_untouched_ones(self):
        cb.apply(self.data, self.tool, cb.Settings(flags="trans", insignia="grey"), self.log)
        self.assertEqual(cb.made_names(self.data), {"banner", "helmet", "helmet_n"})
        p = self.folder() / "helmet.png"
        p.write_bytes(p.read_bytes() + b"x")
        self.assertEqual(cb.made_names(self.data), {"banner", "helmet_n"})


class JoinedWithTheUserPack(unittest.TestCase):
    """The built-in pictures go with the player's own: every build of the user texture pack
    applies the saved choice first (owner 2026-11-05 07:24)."""

    class FakeRunner:
        def __init__(self):
            self.cancel = threading.Event()
            self.packed = []

        def run(self, cmd, quiet=False, check=True, line_filter=None, **kw):
            pictures, pack = Path(str(cmd[3])), Path(str(cmd[4]))
            pack.mkdir(parents=True, exist_ok=True)
            self.packed = sorted(p.stem for p in pictures.glob("*.png"))
            (pack / "index.tsv").write_text("".join(f"{i:016x}\t8\t8\t18\t{n}.dds\n"
                                                    for i, n in enumerate(self.packed, 1)))
            return 0

    setUp = Apply.setUp
    tearDown = Apply.tearDown
    folder = Apply.folder

    def build(self):
        runner = self.FakeRunner()
        keys = user_textures.build(self.d / "rip", self.data, self.tool, 1, runner, self.log)
        return keys, runner.packed

    def test_refresh_does_nothing_without_a_choice(self):
        self.assertIsNone(cb.refresh(self.data, self.tool, self.log))
        self.assertFalse(self.folder().exists())

    def test_the_pack_build_applies_the_saved_choice(self):
        cb.save_settings(self.data, cb.Settings(flags="rainbow", insignia="blend"))
        keys, packed = self.build()
        self.assertEqual(packed, ["banner", "helmet", "helmet_n"])
        self.assertEqual(keys, 3)
        # Off again: the next pack build takes them back; the player's own picture stays.
        (self.folder() / "mine.png").write_bytes(b"own")
        cb.save_settings(self.data, cb.Settings())
        keys, packed = self.build()
        self.assertEqual(packed, ["mine"])
        self.assertIsNone(cb.refresh(self.data, self.tool, self.log))

    def test_the_upscaler_still_makes_the_hd_copies_for_them(self):
        cb.save_settings(self.data, cb.Settings(insignia="white"))
        self.build()
        (self.folder() / "mine.png").write_bytes(b"own")
        self.assertEqual(user_textures.replaced_names(self.data), {"mine"})


if __name__ == "__main__":
    unittest.main()

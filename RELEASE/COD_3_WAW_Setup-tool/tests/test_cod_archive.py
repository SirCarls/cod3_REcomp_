"""[acct2-decomp] Tests for c3w/cod_archive.py (the .cod reader and writer the censored campaign
uses) and censored.copy_textures. Made-up archives only (tests/fake_cod.py); no game data.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import fake_cod as F  # noqa: E402
from c3w import censored, cod_archive as C  # noqa: E402
from c3w.common import SetupError  # noqa: E402


class Lzo(unittest.TestCase):
    def test_literal_runs(self):
        for n in (1, 3, 4, 200, 238, 239, 272, 273, 274, 600, 5000):
            data = bytes((i * 7) & 0xFF for i in range(n))
            src = F.lzo_literals(data)
            self.assertEqual(C.lzo1x_decompress(src, n), (data, len(src)), n)

    def test_a_match(self):
        # 4 literals "abcd", then an M2 match of 4 bytes 4 back ("abcd" again), then the end
        src = bytes([17 + 4]) + b"abcd" + bytes([(3 << 5) | (3 << 2), 0]) + b"\x11\x00\x00"
        self.assertEqual(C.lzo1x_decompress(src, 8)[0], b"abcdabcd")

    def test_damaged(self):
        good = F.lzo_literals(b"x" * 50)
        for bad, n in ((good[:-1], 50), (good[:20], 50), (good, 49), (good, 51), (b"", 1),
                       (bytes([17 + 4]) + b"abcd" + bytes([(3 << 5) | (7 << 2), 0]) + b"\x11\x00\x00", 8)):
            with self.assertRaises(C.CodError):
                C.lzo1x_decompress(bad, n)


class Archive(unittest.TestCase):
    def setUp(self):
        self.flag = F.texture(0x80, 0x400, addr=0x123)
        self.f = F.level(b"english words", self.flag, "of_flag_c.xetex")

    def test_index(self):
        idx = C.read_index(self.f)
        self.assertEqual([s.name for s in idx.sections], ["STRING", ""])
        s = idx.streamed()
        self.assertEqual(len(s.chunks), 2)
        self.assertNotEqual(s.chunks[0].stored, 0)       # packed
        self.assertEqual(s.chunks[1].stored, 0)          # stored
        self.assertEqual([e.name for e in s.entries], ["lang.stbb", "of_flag_c.xetex", "plain_c.xetex"])
        self.assertEqual(idx.align, C.ALIGN)
        self.assertEqual(C.unpack(self.f, idx.sections[0].chunks[0]), b"english words" * 3)

    def test_items(self):
        s = C.read_index(self.f).streamed()
        ch = C.unpack(self.f, s.chunks[0])
        items = C.apkf_items(ch)
        self.assertEqual([(it.type, it.record, it.index, it.name) for it in items],
                         [(F.TXT, 0, 0, "lang"), (C.TEX, 1, 0, "flag_c")])
        (o, n), (g, m) = items[1].parts
        self.assertEqual((ch[o:o + n], ch[g:g + m]), self.flag)
        self.assertEqual(items[0].parts[1], (0, 0))      # one part: nothing in the second area
        self.assertEqual(ch[items[0].parts[0][0]:][:13], b"english words")
        tex = C.apkf_items(C.unpack(self.f, s.chunks[1]))
        self.assertEqual([it.name for it in tex], ["plain_c"])

    def test_texture_shape_ignores_the_address(self):
        a, _ = F.texture(1, addr=0x100)
        b, _ = F.texture(2, addr=0x777)
        c, _ = F.texture(1, width=128)
        d, _ = F.texture(1, fmt=0x14)
        self.assertEqual(C.texture_shape(a), C.texture_shape(b))
        self.assertNotEqual(C.texture_shape(a), C.texture_shape(c))
        self.assertNotEqual(C.texture_shape(a), C.texture_shape(d))
        self.assertIsNone(C.texture_shape(a[:0x50]))

    def test_rewrite_unchanged_is_the_same_file(self):
        for align in (C.ALIGN, C.SMALL_ALIGN):
            f = F.level(b"words", self.flag, "of_flag_c.xetex", align=align)
            idx = C.read_index(f)
            self.assertEqual(idx.align, align)
            self.assertEqual(C.replace_chunks(f, idx, {}), f)

    def test_replace_one_chunk(self):
        idx = C.read_index(self.f)
        s = idx.streamed()
        si = idx.sections.index(s)
        new = bytearray(C.unpack(self.f, s.chunks[0]))
        new[-4:] = b"NEW!"
        out = C.replace_chunks(self.f, idx, {(si, 0): bytes(new)})
        back = C.read_index(out)
        b = back.streamed()
        self.assertEqual(b.chunks[0].stored, 0)          # written as it is
        self.assertEqual(C.unpack(out, b.chunks[0]), bytes(new))
        for c0, c1 in ((idx.sections[0].chunks[0], back.sections[0].chunks[0]), (s.chunks[1], b.chunks[1])):
            self.assertEqual(self.f[c0.offset:c0.offset + (c0.stored or c0.size)],   # the others byte for byte
                             out[c1.offset:c1.offset + (c1.stored or c1.size)])
            self.assertEqual((c0.size, c0.slot, c0.flags, c0.stored), (c1.size, c1.slot, c1.flags, c1.stored))
        self.assertEqual([e.name for e in b.entries], [e.name for e in s.entries])
        self.assertEqual(out[:0x30], self.f[:0x30])
        self.assertEqual(len(out) % C.ALIGN, 0)
        for c in [c for sec in back.sections for c in sec.chunks]:
            self.assertEqual(c.offset % C.ALIGN, 0)
        with self.assertRaises(C.CodError):               # more than its slot
            C.replace_chunks(self.f, idx, {(si, 0): bytes(s.chunks[0].slot + 1)})

    def test_damaged(self):
        f = bytearray(self.f)
        for bad in (b"", b"KAPX" + self.f[4:], self.f[:0x40], self.f[:len(self.f) // 2]):
            with self.assertRaises(C.CodError):
                idx = C.read_index(bad)
                for s in idx.sections:
                    for c in s.chunks:
                        C.unpack(bad, c)
        s = C.read_index(self.f).streamed()
        c = s.chunks[0]
        for at, put in ((c.offset, b"NCX\0"),                          # not an NCH block
                        (c.offset + 0x1C, b"\0\0\0\2"),                # odd block header
                        (c.offset + c.stored - 3, b"\0\0\0")):         # the stream doesn't end there
            g = bytearray(f)
            g[at:at + len(put)] = put
            with self.assertRaises(C.CodError):
                C.unpack(bytes(g), c)
        for bad in (b"APKF" + bytes(30), b"KAPF" + bytes(60)):
            with self.assertRaises(C.CodError):
                C.apkf_items(bad)
        ch = bytearray(C.unpack(self.f, s.chunks[0]))
        struct.pack_into(">I", ch, 0x1C + 4 * 3, 0x7FFFFFF0)      # the first type's table pointer
        with self.assertRaises(C.CodError):
            C.apkf_items(bytes(ch))


class CopyTextures(unittest.TestCase):
    def setUp(self):
        self.mine = F.texture(0x11, 0x400, addr=0x200)
        self.theirs = F.texture(0x99, 0x400, addr=0x555)          # other pixels, elsewhere in memory
        self.own = F.level(b"english words here", self.mine, "of_flag_c.xetex")
        self.donor = F.level(b"deutsch", self.theirs, "no_flag_c.xetex", extra_texture=True)

    def test_copies_the_pixels_only(self):
        out, copied, problems = censored.copy_textures(self.own, self.donor, [("of_flag_c.xetex", "no_flag_c.xetex")])
        self.assertEqual((copied, problems), (["of_flag_c.xetex"], []))
        idx, old = C.read_index(out), C.read_index(self.own)
        ch, before = C.unpack(out, idx.streamed().chunks[0]), C.unpack(self.own, old.streamed().chunks[0])
        items = C.apkf_items(ch)
        (o, n), (g, m) = items[1].parts
        self.assertEqual(ch[g:g + m], self.theirs[1])             # their pixels
        self.assertEqual(ch[o:o + n], self.mine[0])               # our object (its GPU address)
        self.assertEqual(ch[:g] + ch[g + m:], before[:g] + before[g + m:])   # nothing else
        self.assertIn(b"english words here", ch)                  # the language stays
        self.assertEqual(C.unpack(out, idx.sections[0].chunks[0]), b"english words here" * 3)
        self.assertEqual([e.name for e in idx.streamed().entries], [e.name for e in old.streamed().entries])
        s0, s1 = old.streamed().chunks[1], idx.streamed().chunks[1]
        self.assertEqual(self.own[s0.offset:s0.offset + s0.size], out[s1.offset:s1.offset + s1.size])
        # and back: the donor's pixels out again gives the own chunk
        back, copied, _ = censored.copy_textures(out, self.own, [("of_flag_c.xetex", "of_flag_c.xetex")])
        self.assertEqual(C.unpack(back, C.read_index(back).streamed().chunks[0]), before)

    def test_problems(self):
        other_shape = F.level(b"deutsch", F.texture(0x99, 0x400, width=32), "no_flag_c.xetex")
        other_size = F.level(b"deutsch", F.texture(0x99, 0x500), "no_flag_c.xetex")
        for donor, why in ((other_shape, "another size or format"), (other_size, "another size or format")):
            out, copied, problems = censored.copy_textures(self.own, donor, [("of_flag_c.xetex", "no_flag_c.xetex")])
            self.assertEqual((out, copied), (self.own, []))
            self.assertIn(why, problems[0])
        out, copied, problems = censored.copy_textures(self.own, self.donor, [("of_nothing.xetex", "no_flag_c.xetex"),
                                                                              ("lang.stbb", "lang.stbb")])
        self.assertEqual((out, copied), (self.own, []))
        self.assertIn("not in the archive's directory", problems[0])
        self.assertIn("not a texture", problems[1])

    def test_not_an_archive(self):
        with self.assertRaises(C.CodError):
            censored.copy_textures(b"not a cod file at all, really" * 3, self.donor, [("a", "b")])


if __name__ == "__main__":
    unittest.main()

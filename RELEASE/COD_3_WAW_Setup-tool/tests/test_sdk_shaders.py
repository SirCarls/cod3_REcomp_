"""Tests for c3w/sdk_shaders.py (the Vulkan resolve shaders edited for SDK
patch 0010's 4096-tile EDRAM), on small hand-made SPIR-V modules with the
same instruction patterns as the SDK's shaders.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import sdk_shaders as ss  # noqa: E402

UINT, C13, C2047, CWRAP, EDRAM_INFO, SHIFTED, BASE, X, MOD = range(1, 10)


def ins(op, *operands):
    return [((len(operands) + 1) << 16) | op, *operands]


def module(scaled: bool) -> list:
    words = [0x07230203, 0x00010000, 0, 100, 0]
    words += ins(ss.OP_TYPE_INT, UINT, 32, 0)
    words += ins(ss.OP_CONSTANT, UINT, C13, 13)
    words += ins(ss.OP_CONSTANT, UINT, C2047, 2047)
    words += ins(ss.OP_CONSTANT, UINT, CWRAP, 2048 if scaled else ss.OLD_WRAP_DWORDS)
    words += ins(ss.OP_FUNCTION, 50, 51, 0, 52)
    words += ins(ss.OP_SHIFT_RIGHT_LOGICAL, UINT, SHIFTED, EDRAM_INFO, C13)
    words += ins(ss.OP_BITWISE_AND, UINT, BASE, SHIFTED, C2047)
    if scaled:
        words += ins(ss.OP_IMUL, UINT, MOD, X, CWRAP)
    else:
        words += ins(ss.OP_UMOD, UINT, MOD, X, CWRAP)
    return words


class EditTests(unittest.TestCase):
    def check_base(self, m: ss.Module) -> None:
        code = m.code()
        ops = [op for _, op, _ in code]
        i = ops.index(ss.OP_BITWISE_AND)
        self.assertEqual(ops[i:i + 4], [ss.OP_BITWISE_AND, ss.OP_SHIFT_RIGHT_LOGICAL, ss.OP_BITWISE_AND,
                                        ss.OP_BITWISE_OR])
        low = code[i][2][1]
        shift_hi, and_hi, orr = code[i + 1][2], code[i + 2][2], code[i + 3][2]
        consts = m.uint_constants()
        self.assertEqual(shift_hi[2:], [EDRAM_INFO, consts[20]])
        self.assertEqual(and_hi[2:], [shift_hi[1], consts[2048]])
        # The OR takes over the original result id, so its users need no change.
        self.assertEqual(orr[1:], [BASE, low, and_hi[1]])

    def test_unscaled(self):
        m = ss.Module(ss.edit_module(module(scaled=False)))
        consts = m.uint_constants()
        self.assertIn(ss.NEW_WRAP_DWORDS, consts)
        self.assertNotIn(ss.OLD_WRAP_DWORDS, consts)
        self.check_base(m)

    def test_scaled(self):
        m = ss.Module(ss.edit_module(module(scaled=True)))
        consts = m.uint_constants()
        mul = next(ops for _, op, ops in m.code() if op == ss.OP_IMUL)
        self.assertEqual(mul[3], consts[4096])
        self.check_base(m)

    def test_unknown_shader_is_refused(self):
        words = module(scaled=False)
        words[words.index(13)] = 14  # no `>> 13` extraction any more
        with self.assertRaises(ss.ShaderEditError):
            ss.edit_module(words)

    def test_header_round_trip_and_marker(self):
        words = module(scaled=False)
        body = ",\n".join(f"    0x{w:08X}" for w in words)
        text = "// Generated with `xb buildshaders`.\n#if 0\n; SPIR-V\n#endif\n\nconst uint32_t resolve_test_cs[] = {\n" + \
               body + ",\n};\n"
        edited = ss.edit_header(text)
        self.assertIsNotNone(edited)
        self.assertTrue(edited.startswith(ss.MARKER))
        self.assertIsNone(ss.edit_header(edited))  # done once only
        self.assertIn(f"0x{ss.NEW_WRAP_DWORDS:08X}", edited)


if __name__ == "__main__":
    unittest.main()

"""Tests for the setup tool's themes (c3w/theme.py): the color math
(contrast, mixing, the accent override) and that every theme keeps text
readable. No Tkinter needed: theme.py only imports Tk inside apply().

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import theme  # noqa: E402

# Accents to try: a typical pick, extremes, mid gray (hardest for text on
# it), near-black and near-white (hardest against dark and light themes).
ACCENTS = ["", "#0078d7", "#ff00ff", "#00ff00", "#777777", "#101010", "#202040", "#fafafa", "#ffffe0", "#c49854"]

# (text, drawn on) pairs that are body text: 4.5:1 at least (7:1 in High contrast).
BODY_PAIRS = [("text", "bg"), ("text", "sidebar"), ("text", "field"), ("text", "button"), ("text", "well"),
              ("text", "surface"), ("text", "button_hover"), ("text", "field_hover"),
              ("text_dim", "bg"), ("text_dim", "sidebar"), ("text_dim", "surface"),
              ("accent_text", "bg"), ("accent_text", "sidebar"), ("accent_text", "surface"),
              ("sidebar_sel_text", "sidebar_sel"), ("select_fg", "select_bg"),
              ("good", "bg"), ("warn", "bg"), ("bad", "bg"), ("good", "surface"), ("bad", "surface"),
              ("warn", "well"), ("bad", "well")]
# Text on a filled control: always 4.5:1 (pressed is short, but still readable).
FILL_PAIRS = [("accent_fg", "accent"), ("accent_fg_hover", "accent_hover"), ("text", "button_active")]


class ColorMathTests(unittest.TestCase):
    def test_tk_not_imported(self):
        self.assertNotIn("tkinter", sys.modules)

    def test_parse_and_hex(self):
        self.assertEqual(theme.parse("#C49854"), (196, 152, 84))
        self.assertEqual(theme.parse("#fff"), (255, 255, 255))
        self.assertEqual(theme.parse((1, 2, 300)), (1, 2, 255))
        self.assertEqual(theme.to_hex((196, 152, 84)), "#c49854")
        for bad in ("", "#12", "nope", "#1234567"):
            with self.assertRaises(ValueError):
                theme.parse(bad)

    def test_normalize_accent(self):
        self.assertEqual(theme.normalize_accent("#ABCDEF"), "#abcdef")
        self.assertEqual(theme.normalize_accent("#abc"), "#aabbcc")
        for bad in (None, "", "blue", "#12345", 42, ["#fff"]):
            self.assertEqual(theme.normalize_accent(bad), "")

    def test_contrast(self):
        self.assertAlmostEqual(theme.contrast("#000000", "#ffffff"), 21.0, places=2)
        self.assertAlmostEqual(theme.contrast("#ffffff", "#000000"), 21.0, places=2)
        self.assertAlmostEqual(theme.contrast("#777777", "#777777"), 1.0, places=6)
        # The WCAG example: #767676 on white is just above 4.5:1.
        self.assertGreaterEqual(theme.contrast("#767676", "#ffffff"), 4.5)
        self.assertLess(theme.contrast("#777777", "#ffffff"), 4.5)

    def test_mix(self):
        self.assertEqual(theme.mix("#000000", "#ffffff", 0), "#000000")
        self.assertEqual(theme.mix("#000000", "#ffffff", 1), "#ffffff")
        self.assertEqual(theme.mix("#000000", "#ffffff", 0.5), "#808080")
        self.assertEqual(theme.mix("#102030", "#102030", 0.7), "#102030")
        self.assertEqual(theme.mix("#000000", "#ffffff", 7), "#ffffff")    # clamped
        self.assertEqual(theme.mix("#000000", "#ffffff", -1), "#000000")

    def test_ensure_contrast(self):
        for fg in ("#000000", "#333333", "#c49854", "#0078d7", "#ffff00", "#ffffff"):
            for bg in ("#000000", "#100d09", "#e2d6ba", "#ffffff", "#808080"):
                for target in (3.0, 4.5, 7.0):
                    out = theme.ensure_contrast(fg, bg, target)
                    if theme.contrast("#000000", bg) >= target or theme.contrast("#ffffff", bg) >= target:
                        self.assertGreaterEqual(theme.contrast(out, bg), target, (fg, bg, target, out))
        # A color that already reads well is kept as it is.
        self.assertEqual(theme.ensure_contrast("#e2d6be", "#100d09"), "#e2d6be")

    def test_ensure_contrast_against_several(self):
        out = theme.ensure_contrast("#808080", ["#100d09", "#2c251b"], 4.5)
        self.assertGreaterEqual(theme.contrast(out, "#100d09"), 4.5)
        self.assertGreaterEqual(theme.contrast(out, "#2c251b"), 4.5)

    def test_text_on(self):
        for fill in ("#000000", "#777777", "#0078d7", "#ffff00", "#ffffff", "#805a1e"):
            self.assertGreaterEqual(theme.contrast(theme.text_on(fill), fill), 4.5, fill)
        # The theme's own dark ink is preferred when it reads.
        self.assertEqual(theme.text_on("#c49854", ["#100d09"]), "#100d09")


class ThemeTests(unittest.TestCase):
    def test_four_game_themes(self):
        self.assertEqual(theme.THEME_ORDER, ["cod3", "olive", "parchment", "contrast"])
        self.assertEqual([theme.THEMES[t].label for t in theme.THEME_ORDER],
                         ["Call of Duty 3", "Field olive", "Briefing paper", "High contrast"])
        self.assertEqual(theme.DEFAULT_THEME, "cod3")
        for tid in theme.THEME_ORDER:
            self.assertEqual(theme.theme_id_for_label(theme.theme_label(tid)), tid)
        self.assertEqual(theme.theme_id_for_label("no such theme"), "cod3")
        self.assertEqual(theme.theme_label("no such theme"), "Call of Duty 3")

    def test_readable_in_every_theme_and_accent(self):
        for tid in theme.THEME_ORDER:
            target = theme.HIGH_CONTRAST if tid == "contrast" else theme.BODY_CONTRAST
            for accent in ACCENTS:
                c = theme.colors(tid, accent)
                for fg, bg in BODY_PAIRS:
                    self.assertGreaterEqual(theme.contrast(c[fg], c[bg]), target - 1e-9,
                                            f"{tid} {accent or 'default'}: {fg} on {bg}")
                for fg, bg in FILL_PAIRS:
                    self.assertGreaterEqual(theme.contrast(c[fg], c[bg]), theme.BODY_CONTRAST,
                                            f"{tid} {accent or 'default'}: {fg} on {bg}")
                # Off text looks off, but stays legible.
                self.assertGreaterEqual(theme.contrast(c["text_disabled"], c["bg"]), 3.0)

    def test_accent_override(self):
        base = theme.colors("cod3")
        self.assertEqual(base["accent"], "#c49854")
        self.assertEqual(base["accent_custom"], "")
        c = theme.colors("cod3", "#0078D7")
        self.assertEqual(c["accent"], "#0078d7")
        self.assertEqual(c["accent_custom"], "1")
        # Everything not about the accent stays as the theme has it.
        for key in ("bg", "sidebar", "text", "text_dim", "field", "button", "good", "warn", "bad"):
            self.assertEqual(c[key], base[key], key)
        # A bad saved accent falls back to the theme's own.
        self.assertEqual(theme.colors("cod3", "not a color")["accent"], base["accent"])

    def test_high_contrast_stays_high(self):
        c = theme.colors("contrast")
        self.assertEqual(c["bg"], "#000000")
        self.assertEqual(c["text"], "#ffffff")
        self.assertGreaterEqual(theme.contrast(c["text_dim"], c["bg"]), 7.0)

    def test_light_theme_is_light(self):
        self.assertTrue(theme.colors("parchment")["light"])
        for tid in ("cod3", "olive", "contrast"):
            self.assertFalse(theme.colors(tid)["light"])

    def test_unknown_theme_is_default(self):
        self.assertEqual(theme.colors("nope"), theme.colors("cod3"))

    def test_same_colors_as_the_game(self):
        """The palettes are the game's F2 themes (src/ui/theme.cpp, shipped
        in recomp/): same ids, names, and window/text/accent colors."""
        cpp = Path(__file__).resolve().parent.parent / "recomp" / "src" / "ui" / "theme.cpp"
        if not cpp.is_file():
            self.skipTest("recomp/src/ui/theme.cpp isn't in this copy")
        src = cpp.read_text(encoding="utf-8")
        infos = re.findall(r'\{"(\w+)", "([^"]+)", "[^"]*"\}', src)
        self.assertEqual([i for i, _ in infos], theme.THEME_ORDER)
        self.assertEqual([lb for _, lb in infos], [theme.THEMES[t].label for t in theme.THEME_ORDER])
        names = {"cod3": "kCod3", "olive": "kOlive", "parchment": "kParchment", "contrast": "kContrast"}
        for tid, var in names.items():
            body = re.search(r"const Colors " + var + r" = \{(.*?)\}\};", src, re.S).group(1)
            rgbs = [tuple(int(v) for v in m) for m in re.findall(r"Rgb\((\d+), (\d+), (\d+)", body)]
            base = theme.THEMES[tid].base
            # The struct's order: window, popup, frame, ..., text (11), text_dim, accent, accent_text.
            self.assertEqual(rgbs[0], base["window"], tid)
            self.assertEqual(rgbs[11], base["text"], tid)
            self.assertEqual(rgbs[12], base["text_dim"], tid)
            self.assertEqual(rgbs[13], base["accent"], tid)
            self.assertEqual(rgbs[14], base["accent_text"], tid)


if __name__ == "__main__":
    unittest.main()

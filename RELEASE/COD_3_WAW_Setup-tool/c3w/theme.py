"""The setup tool's look: the four themes of the game's F2 settings window
(src/ui/theme.cpp), so the tool and the game look like one product, plus an
accent color the user can pick.

The colors and the color math (contrast, mixing) don't need Tk, so the tests
run without Tkinter. apply() is the only part that touches Tk: it builds ttk
styles on the "clam" theme (there on every system, and under Wine) and
recolors the plain Tk widgets (text boxes, lists, canvases, windows).
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, Optional, Tuple

RGB = Tuple[int, int, int]

DEFAULT_THEME = "cod3"
BODY_CONTRAST = 4.5        # WCAG AA for body text
HIGH_CONTRAST = 7.0        # WCAG AAA, for the High contrast theme


@dataclass(frozen=True)
class Theme:
    id: str            # the value saved in settings.json ("ui_theme"); the game's cod3_ui_theme ids
    label: str         # shown in the tool, as in the game's F2
    description: str
    base: Dict[str, object]


def _blend(top: RGB, alpha: float, under: RGB) -> RGB:
    return tuple(int(round(t * alpha + u * (1 - alpha))) for t, u in zip(top, under))  # type: ignore[return-value]


def _game(window, popup, frame, frame_hover, frame_active, button, button_hover, button_active,
          title, title_active, border, border_alpha, text, text_dim, accent, accent_text,
          good, warn, bad) -> Dict[str, object]:
    """The game's Colors struct, in its order (theme.cpp). The border has an
    alpha there; here it is blended onto the window color."""
    return {"window": window, "popup": popup, "frame": frame, "frame_hover": frame_hover,
            "frame_active": frame_active, "button": button, "button_hover": button_hover,
            "button_active": button_active, "title": title, "title_active": title_active,
            "border": _blend(border, border_alpha, window), "text": text, "text_dim": text_dim,
            "accent": accent, "accent_text": accent_text, "good": good, "warn": warn, "bad": bad}


# Copied from the game's F2 themes (src/ui/theme.cpp): same ids, names and
# colors. Sampled there from Call of Duty 3's own title and loading screens.
THEMES: Dict[str, Theme] = {t.id: t for t in (
    Theme("cod3", "Call of Duty 3", "The game's own colors: black, brass gold and parchment.", _game(
        (16, 13, 9), (22, 18, 13), (44, 37, 27), (64, 53, 36), (82, 66, 40),
        (56, 46, 31), (98, 78, 46), (140, 108, 60), (20, 16, 11), (38, 30, 19), (107, 85, 24), 0.75,
        (226, 214, 190), (150, 136, 112), (196, 152, 84), (214, 172, 104),
        (132, 186, 96), (222, 178, 84), (214, 92, 70))),
    Theme("olive", "Field olive", "Olive drab panels with khaki text and brass.", _game(
        (25, 27, 18), (31, 33, 22), (52, 56, 36), (70, 75, 47), (88, 93, 57),
        (62, 66, 41), (86, 91, 56), (128, 122, 76), (30, 32, 21), (52, 56, 35), (110, 112, 70), 0.8,
        (226, 220, 196), (150, 148, 120), (196, 166, 96), (212, 186, 120),
        (140, 192, 100), (226, 190, 90), (214, 96, 72))),
    Theme("parchment", "Briefing paper", "Light: aged paper and dark ink.", _game(
        (226, 214, 186), (232, 222, 198), (204, 189, 158), (192, 174, 138), (178, 158, 118),
        (196, 178, 140), (180, 158, 116), (150, 120, 70), (200, 186, 152), (176, 156, 116), (140, 112, 64), 0.8,
        (44, 32, 18), (106, 90, 66), (128, 90, 30), (110, 74, 20),
        (48, 112, 40), (150, 96, 0), (170, 40, 30))),
    Theme("contrast", "High contrast", "Black and white with gold highlights, for the easiest reading.", _game(
        (0, 0, 0), (8, 8, 8), (36, 36, 36), (60, 60, 60), (84, 84, 84),
        (44, 44, 44), (76, 76, 76), (150, 116, 40), (0, 0, 0), (28, 28, 28), (210, 170, 70), 0.9,
        (255, 255, 255), (190, 190, 190), (255, 200, 80), (255, 210, 100),
        (120, 230, 120), (255, 210, 60), (255, 100, 90))),
)}
THEME_ORDER = ["cod3", "olive", "parchment", "contrast"]

# Each game tab's own colors (c3w/games.py), drawn up for this tool after
# each game's mood; nothing is taken from the games' files. The Call of Duty
# 3 tab's own colors are the "cod3" theme above. A tab can also use any of
# the four themes instead (its theme picker lists its own colors first).
GAME_PALETTES: Dict[str, Theme] = {t.id: t for t in (
    # (Its borders are a warm gray, not red: a red box reads as a mistake.)
    Theme("waw", "World at War", "Charcoal, blood red and bone white.", _game(
        (14, 12, 12), (22, 18, 18), (40, 34, 33), (58, 48, 46), (76, 60, 56),
        (52, 42, 40), (84, 56, 52), (128, 44, 40), (19, 15, 15), (36, 26, 25), (104, 94, 90), 0.75,
        (232, 226, 216), (160, 150, 140), (172, 30, 30), (226, 92, 80),
        (128, 184, 100), (226, 176, 80), (236, 96, 80))),
    Theme("cod2", "Call of Duty 2", "Gunmetal, steel and signal orange.", _game(
        (15, 16, 18), (21, 23, 26), (38, 41, 46), (54, 58, 65), (70, 75, 84),
        (48, 52, 58), (70, 76, 85), (112, 82, 50), (19, 21, 24), (34, 37, 42), (98, 104, 114), 0.7,
        (226, 228, 230), (148, 154, 162), (214, 128, 44), (232, 156, 82),
        (126, 190, 110), (230, 186, 86), (228, 96, 80))),
    Theme("coduo", "United Offensive", "Olive drab, khaki and field green.", _game(
        (17, 20, 13), (23, 27, 18), (42, 48, 32), (58, 66, 43), (74, 84, 54),
        (52, 59, 38), (74, 84, 54), (104, 112, 60), (21, 25, 16), (38, 44, 28), (112, 124, 74), 0.75,
        (230, 226, 204), (158, 158, 130), (150, 170, 82), (182, 200, 112),
        (140, 196, 100), (226, 190, 92), (220, 100, 76))),
)}
# Every palette by id: the four themes and the games' own.
ALL_PALETTES: Dict[str, Theme] = {**THEMES, **GAME_PALETTES}


def palettes_for(own: str) -> List[str]:
    """The colors a tab can pick, in its theme list's order: its own
    palette first, then the four themes (once each)."""
    return [own] + [t for t in THEME_ORDER if t != own] if own in ALL_PALETTES else list(THEME_ORDER)


# On the other games' tabs, Call of Duty 3's colors are named by their look
# (a theme called "Call of Duty 3" on World at War's tab would confuse).
OTHER_TAB_NAMES = {"cod3": ("Brass and black", "Black, brass gold and parchment: Call of Duty 3's colors.")}


def picker_label(theme_id: str, own: str) -> str:
    """A palette's name in the theme list of a tab whose own palette is `own`."""
    if theme_id != own and theme_id in OTHER_TAB_NAMES:
        return OTHER_TAB_NAMES[theme_id][0]
    return theme_label(theme_id)


def picker_description(theme_id: str, own: str) -> str:
    if theme_id != own and theme_id in OTHER_TAB_NAMES:
        return OTHER_TAB_NAMES[theme_id][1]
    return (ALL_PALETTES.get(theme_id) or THEMES[DEFAULT_THEME]).description


def picker_id(label: str, own: str) -> str:
    """The palette a label in that tab's list stands for (its own if unknown)."""
    return next((t for t in palettes_for(own) if picker_label(t, own) == label), own)


# The type of the header's title and the page titles, per game: the first
# of these the system has (Windows' own fonts first, then common Linux
# ones), as (family, weight); else the UI font in bold. Barlow Semi
# Condensed is the game's own F2 font, which the tool loads from the project
# (c3w/fonts.py); the rest are system fonts. None comes from a game.
DISPLAY_FONTS: Dict[str, Tuple[Tuple[str, str], ...]] = {
    "cod3": (("Barlow Semi Condensed SemiBold", "normal"), ("Barlow Semi Condensed", "bold"),
             ("Bahnschrift SemiBold Condensed", "normal"), ("Bahnschrift", "bold"), ("Roboto Condensed", "bold"),
             ("Ubuntu Condensed", "normal"), ("DejaVu Sans", "bold"), ("Arial", "bold")),
    "waw": (("Impact", "normal"), ("Haettenschweiler", "normal"), ("Anton", "normal"), ("Oswald", "bold"),
            ("League Gothic", "normal"), ("Bebas Neue", "normal"), ("Nimbus Sans Narrow", "bold"),
            ("Liberation Sans Narrow", "bold"), ("Arial Narrow", "bold"), ("Liberation Sans", "bold"),
            ("Nimbus Sans", "bold"), ("FreeSans", "bold"), ("Arial", "bold")),
    "cod2": (("Georgia", "bold"), ("Bitstream Charter", "bold"), ("Charter", "bold"), ("C059", "bold"),
             ("TeX Gyre Schola", "bold"), ("Century Schoolbook L", "bold"), ("Noto Serif", "bold"),
             ("DejaVu Serif", "bold"), ("Cambria", "bold")),
    "coduo": (("Palatino Linotype", "bold"), ("Book Antiqua", "bold"), ("P052", "bold"),
              ("TeX Gyre Pagella", "bold"), ("URW Palladio L", "bold"), ("Liberation Serif", "bold"),
              ("Times New Roman", "bold"), ("FreeSerif", "bold"), ("Nimbus Roman", "bold"),
              ("DejaVu Serif", "bold")),
}


def pick_display_font(game_id: str, families: Iterable[str], fallback: str) -> Tuple[str, str]:
    """(family, weight) for a game's titles: the first candidate the system
    has (case doesn't matter), else `fallback` in bold."""
    have = {f.lower(): f for f in families}
    for family, weight in DISPLAY_FONTS.get(game_id, ()):
        if family.lower() in have:
            return have[family.lower()], weight
    return fallback, "bold"


def assign_display_fonts(game_ids: Iterable[str], families: Iterable[str],
                         fallback: str) -> Dict[str, Tuple[str, str]]:
    """Each game's title type, all different where the system allows: a
    game whose first choice another game already has takes its next one.
    (On Linux Impact or Georgia are rarely there, and two games falling back
    to the same font would look alike.)"""
    have = {f.lower(): f for f in families}
    used: set = set()
    out: Dict[str, Tuple[str, str]] = {}
    for gid in game_ids:
        there = [(have[f.lower()], w) for f, w in DISPLAY_FONTS.get(gid, ()) if f.lower() in have]
        pick = next((fw for fw in there if fw[0].lower() not in used), there[0] if there else (fallback, "bold"))
        used.add(pick[0].lower())
        out[gid] = pick
    return out


# ---------------------------------------------------------------------------
# Color math (no Tk)
# ---------------------------------------------------------------------------
def parse(color) -> RGB:
    """'#rrggbb', '#rgb' or an (r, g, b) tuple -> (r, g, b)."""
    if isinstance(color, tuple):
        return tuple(max(0, min(255, int(v))) for v in color[:3])  # type: ignore[return-value]
    s = str(color).strip().lstrip("#")
    if len(s) == 3:
        s = "".join(ch * 2 for ch in s)
    if len(s) != 6:
        raise ValueError(f"not a color: {color!r}")
    return int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)


def to_hex(color) -> str:
    r, g, b = parse(color)
    return f"#{r:02x}{g:02x}{b:02x}"


def normalize_accent(value) -> str:
    """A saved or picked accent as '#rrggbb', or '' (none / not a color)."""
    if not value:
        return ""
    try:
        return to_hex(value)
    except (ValueError, TypeError):
        return ""


def luminance(color) -> float:
    """Relative luminance (WCAG 2)."""
    def channel(v: int) -> float:
        c = v / 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = parse(color)
    return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b)


def contrast(a, b) -> float:
    """WCAG contrast ratio between two colors, 1 to 21."""
    la, lb = luminance(a), luminance(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def mix(a, b, t: float) -> str:
    """`t` of the way from color a to color b (0 = a, 1 = b)."""
    t = max(0.0, min(1.0, t))
    ca, cb = parse(a), parse(b)
    return to_hex(tuple(int(round(x + (y - x) * t)) for x, y in zip(ca, cb)))


def is_light(color) -> bool:
    return luminance(color) > 0.18


def ensure_contrast(fg, backgrounds, target: float = BODY_CONTRAST) -> str:
    """`fg`, moved toward white (on dark backgrounds) or black (on light
    ones) just far enough to reach `target` against every background."""
    bgs = [backgrounds] if isinstance(backgrounds, (str, tuple)) else list(backgrounds)
    first = "#000000" if is_light(bgs[0]) else "#ffffff"
    for toward in (first, "#ffffff" if first == "#000000" else "#000000"):
        for step in range(0, 21):
            c = mix(fg, toward, step / 20)
            if all(contrast(c, bg) >= target for bg in bgs):
                return c
    # Can't be reached (a mid-gray background): the extreme that reads best.
    return max(("#000000", "#ffffff"), key=lambda x: min(contrast(x, bg) for bg in bgs))


def darken_to(color, most: float) -> str:
    """`color`, mixed toward black just enough that its luminance is at
    most `most` (the headers' drawings, kept dark behind their titles)."""
    col = to_hex(color)
    if luminance(col) <= most:
        return col
    lo, hi = 0.0, 1.0
    for _ in range(14):
        mid = (lo + hi) / 2
        if luminance(mix(col, "#000000", mid)) > most:
            lo = mid
        else:
            hi = mid
    return mix(col, "#000000", hi)


def text_on(fill, preferred: Iterable = ()) -> str:
    """Text color for a filled area (a button in the accent color): the first
    preferred color that is readable on it, else black or white."""
    for c in list(preferred):
        if contrast(c, fill) >= BODY_CONTRAST:
            return to_hex(c)
    return "#000000" if contrast("#000000", fill) >= contrast("#ffffff", fill) else "#ffffff"


def colors(theme_id: str = DEFAULT_THEME, accent: str = "") -> Dict[str, str]:
    """Every color the tool uses, for a theme and an optional accent
    override, as '#rrggbb'. Text colors are checked against what they're
    drawn on and nudged to stay readable (4.5:1; 7:1 in High contrast)."""
    theme = ALL_PALETTES.get(theme_id) or THEMES[DEFAULT_THEME]
    b = {k: to_hex(v) for k, v in theme.base.items()}
    target = HIGH_CONTRAST if theme.id == "contrast" else BODY_CONTRAST
    light = is_light(b["window"])
    c: Dict[str, str] = {}
    c["bg"] = b["window"]
    c["sidebar"] = b["title"]
    c["surface"] = b["popup"]           # dialogs' panels, tooltips, list popups
    c["field"] = b["frame"]
    c["field_hover"] = b["frame_hover"]
    c["field_disabled"] = mix(b["frame"], b["window"], 0.55)
    c["button"] = b["button"]
    c["button_hover"] = b["button_hover"]
    # Pressed: the game's color, toned toward the hover color until the
    # label still reads on it.
    c["button_active"] = next((x for x in (mix(b["button_active"], b["button_hover"], i / 10) for i in range(11))
                               if contrast(x, b["text"]) >= BODY_CONTRAST), b["button_hover"])
    c["button_disabled"] = mix(b["button"], b["window"], 0.55)
    c["border"] = b["border"]
    c["divider"] = mix(b["border"], b["window"], 0.45)
    # A well for long text (the log): a step deeper than the window.
    c["well"] = mix(b["window"], "#ffffff", 0.35) if light else mix(b["window"], "#000000", 0.4)
    c["text"] = ensure_contrast(b["text"], [c["bg"], c["sidebar"], c["field"], c["button"], c["well"]], target)
    c["text_dim"] = ensure_contrast(b["text_dim"], [c["bg"], c["sidebar"], c["surface"]], target)
    # Disabled text is meant to look off, but stays legible (3:1).
    c["text_disabled"] = ensure_contrast(mix(b["text_dim"], b["window"], 0.35),
                                         [c["bg"], c["field_disabled"], c["button_disabled"]], 3.0)

    # The accent fills the main button and marks the selected tab and page,
    # so it stays visible on the window and sidebar (3:1, as any control
    # needs), however dark or light the color picked.
    acc = ensure_contrast(normalize_accent(accent) or b["accent"], [c["bg"], c["sidebar"]], 3.0)
    c["accent"] = acc
    c["accent_custom"] = "1" if normalize_accent(accent) else ""
    toward = "#000000" if light else "#ffffff"
    c["accent_hover"] = mix(acc, toward, 0.14)
    c["accent_active"] = mix(acc, b["window"], 0.25)
    # Text drawn on the accent (the Build button): the theme's own dark or
    # light ink when it reads well, else black or white.
    c["accent_fg"] = text_on(acc, [b["window"], b["text"]])
    c["accent_fg_hover"] = text_on(c["accent_hover"], [c["accent_fg"], b["window"], b["text"]])
    # Accent-colored text (headings, links, the selected page).
    base_text = acc if normalize_accent(accent) else b["accent_text"]
    c["accent_text"] = ensure_contrast(base_text, [c["bg"], c["sidebar"], c["surface"]], target)
    c["sidebar_hover"] = mix(c["sidebar"], b["button_hover"], 0.55)
    c["sidebar_sel"] = mix(c["sidebar"], acc, 0.16)
    c["sidebar_sel_text"] = ensure_contrast(c["accent_text"], [c["sidebar_sel"], c["sidebar_hover"]], target)
    # Selected text and rows: tinted with the accent, as much as the text on it allows.
    c["select_bg"] = next((x for x in (mix(c["bg"], acc, t / 100) for t in range(38, 4, -4))
                           if contrast(c["text"], x) >= target), c["bg"])
    c["select_fg"] = ensure_contrast(c["text"], [c["select_bg"]], target)
    c["focus"] = c["accent_text"]
    for name in ("good", "warn", "bad"):
        c[name] = ensure_contrast(b[name], [c["bg"], c["surface"], c["well"]], target)
    c["row_alt"] = mix(c["well"], c["text"], 0.04)
    # The game tabs across the top: a strip a step darker than the sidebar.
    c["tabbar"] = mix(c["sidebar"], "#000000", 0.12 if light else 0.45)
    c["tabbar_hover"] = mix(c["tabbar"], b["button_hover"], 0.5)
    c["tabbar_sel"] = mix(c["tabbar"], acc, 0.14)
    c["tab_text"] = ensure_contrast(b["text_dim"], [c["tabbar"], c["tabbar_hover"]], target)
    c["tab_sel_text"] = ensure_contrast(b["text"], [c["tabbar_sel"], c["tabbar"]], target)
    # Neutral news (a step that comes next): blue, which no palette uses
    # for good, warning or bad, and which never reads as an error.
    c["info"] = ensure_contrast("#2f6db5" if light else "#7fa8dc", [c["bg"], c["surface"], c["well"]], target)
    # Small status labels ("Ready", "Testing", "Next", "Later"): four hues
    # that can't be mistaken for each other or for the accent (a green or
    # red accent would look like "Ready" or like an error): green, amber,
    # blue and gray. Tinted fill, readable text.
    gray = ensure_contrast(mix(b["text_dim"], "#8c8c8c", 0.75), [c["bg"]], target)
    for name, base in (("chip_good", c["good"]), ("chip_warn", c["warn"]), ("chip_info", c["info"]),
                       ("chip_dim", gray)):
        c[name + "_bg"] = mix(c["bg"], base, 0.18)
        c[name] = ensure_contrast(base, [c[name + "_bg"]], target)
    c["light"] = "1" if light else ""
    return c


def theme_label(theme_id: str) -> str:
    return (ALL_PALETTES.get(theme_id) or THEMES[DEFAULT_THEME]).label


def theme_id_for_label(label: str) -> str:
    return next((t.id for t in ALL_PALETTES.values() if t.label == label), DEFAULT_THEME)


# ---------------------------------------------------------------------------
# Tk side
# ---------------------------------------------------------------------------
current: Dict[str, str] = colors()
# Called with the colors after every apply(): the app recolors what styles
# can't reach (the selected page's stripe, the log's tags, swatches).
listeners: List[Callable[[Dict[str, str]], None]] = []
FONTS = ("C3WBody", "C3WBold", "C3WSmall", "C3WHeading", "C3WTitle", "C3WLogo", "C3WNav", "C3WNavBold",
         "C3WMono", "C3WMonoBold", "C3WTab", "C3WAction")
_ui_family: Dict[int, str] = {}   # the UI font's family, per Tk interpreter (set_game_fonts falls back to it)


def setup_fonts(root, windows: bool = False) -> None:
    """The platform's UI font at a comfortable size (Segoe UI on Windows
    when it's there; the Tk default sans elsewhere), and the sizes derived
    from it. Call once, after the desktop scaling is applied."""
    import tkinter.font as tkfont
    default = tkfont.nametofont("TkDefaultFont", root=root)
    family = default.actual("family")
    size = int(default.cget("size"))
    families = set(tkfont.families(root))
    if windows and "Segoe UI" in families:
        family = "Segoe UI"
    # Tk's default is 9 pt (12 px on Linux); 10 pt reads better. A size in
    # pixels (negative) was already scaled for the desktop.
    if size < 0:
        size = -max(-size, round(-size * 13 / 12))
    else:
        size = max(size, 10)

    def scaled(f: float) -> int:
        return int(round(size * f)) or size
    for name in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkTooltipFont", "TkIconFont"):
        try:
            tkfont.nametofont(name, root=root).configure(family=family, size=size)
        except Exception:  # noqa: BLE001 - a font Tk doesn't have here
            pass
    specs = {"C3WBody": (1.0, "normal"), "C3WBold": (1.0, "bold"), "C3WSmall": (0.92, "normal"),
             "C3WHeading": (1.12, "bold"), "C3WTitle": (1.6, "bold"), "C3WLogo": (1.3, "bold"),
             "C3WNav": (1.06, "normal"), "C3WNavBold": (1.06, "bold"), "C3WTab": (0.96, "bold"),
             "C3WAction": (1.1, "bold")}
    for name, (f, weight) in specs.items():
        _named_font(root, name, family=family, size=scaled(f), weight=weight)
    _ui_family[id(root.tk)] = family
    fixed = tkfont.nametofont("TkFixedFont", root=root)
    mono_family = fixed.actual("family")
    if windows and "Consolas" in families:
        mono_family = "Consolas"
    _named_font(root, "C3WMono", family=mono_family, size=size, weight="normal")
    _named_font(root, "C3WMonoBold", family=mono_family, size=size, weight="bold")
    tkfont.nametofont("TkHeadingFont", root=root).configure(family=family, size=size, weight="bold")


GAME_ORDER = ("cod3", "waw", "cod2", "coduo")   # games.GAME_ORDER (which imports nothing from here)
_assigned: Dict[int, Dict[str, Tuple[str, str]]] = {}   # per Tk interpreter


def display_fonts(root) -> Dict[str, Tuple[str, str]]:
    """Each game's title type on this system (assign_display_fonts), worked
    out once per window."""
    import tkinter.font as tkfont
    key = id(root.tk)
    if key not in _assigned:
        fallback = _ui_family.get(key) or tkfont.nametofont("TkDefaultFont", root=root).actual("family")
        try:
            families = tkfont.families(root)
        except Exception:  # noqa: BLE001
            families = ()
        _assigned[key] = assign_display_fonts(GAME_ORDER, families, fallback)
    return _assigned[key]


def set_game_fonts(root, game_id: str) -> Tuple[str, str]:
    """Page titles in the game's display type (DISPLAY_FONTS); everything
    else keeps the UI font. Returns the (family, weight) used."""
    fonts = display_fonts(root)
    family, weight = fonts.get(game_id) or fonts[GAME_ORDER[0]]
    key = (id(root.tk), "C3WTitle")
    if key in _made_fonts:
        _made_fonts[key].configure(family=family, weight=weight)
    return family, weight


def set_chrome_font(root, family: str, weight: str = "normal") -> None:
    """The tabs and the bottom bar's buttons in the given type (the game's
    F2 font when it could be loaded): condensed capitals, like a launcher's."""
    for name in ("C3WTab", "C3WAction"):
        key = (id(root.tk), name)
        if key in _made_fonts:
            _made_fonts[key].configure(family=family, weight=weight)


_made_fonts: dict = {}   # Tk deletes a named font when its Python object goes away


def _named_font(root, name: str, **kw) -> None:
    import tkinter.font as tkfont
    key = (id(root.tk), name)    # per Tk interpreter (a test may make several)
    if key in _made_fonts:
        _made_fonts[key].configure(**kw)
        return
    try:
        _made_fonts[key] = tkfont.Font(root=root, name=name, exists=False, **kw)
    except Exception:  # noqa: BLE001 - already made in this interpreter
        _made_fonts[key] = tkfont.nametofont(name, root=root)
        _made_fonts[key].configure(**kw)


def apply(root, theme_id: str = DEFAULT_THEME, accent: str = "",
          px: Callable[[float], int] = lambda n: int(round(n))) -> Dict[str, str]:
    """Styles every widget the tool uses for this theme and accent, at once
    (no restart): ttk styles update live, plain Tk widgets are recolored."""
    from tkinter import ttk
    global current
    c = colors(theme_id, accent)
    current = c
    s = ttk.Style(root)
    if s.theme_use() != "clam":
        s.theme_use("clam")
    linespace = _linespace(root)

    s.configure(".", background=c["bg"], foreground=c["text"], bordercolor=c["border"],
                darkcolor=c["bg"], lightcolor=c["bg"], troughcolor=c["field"], focuscolor=c["focus"],
                selectbackground=c["select_bg"], selectforeground=c["select_fg"], insertcolor=c["text"],
                fieldbackground=c["field"], arrowcolor=c["text"], font="TkDefaultFont")
    s.map(".", foreground=[("disabled", c["text_disabled"])])
    # Frames and labels
    s.configure("TFrame", background=c["bg"])
    s.configure("TLabel", background=c["bg"], foreground=c["text"])
    for name, fg, font in (("Hint", c["text_dim"], "TkDefaultFont"), ("Small", c["text_dim"], "C3WSmall"),
                           ("Bold", c["text"], "C3WBold"), ("Title", c["text"], "C3WTitle"),
                           ("Heading", c["accent_text"], "C3WHeading"), ("Accent", c["accent_text"], "C3WBold"),
                           ("Good", c["good"], "TkDefaultFont"), ("Warn", c["warn"], "TkDefaultFont"),
                           ("Bad", c["bad"], "TkDefaultFont"), ("GoodBold", c["good"], "C3WBold"),
                           ("WarnBold", c["warn"], "C3WBold"), ("BadBold", c["bad"], "C3WBold")):
        s.configure(f"{name}.TLabel", background=c["bg"], foreground=fg, font=font)
    s.configure("Sidebar.TFrame", background=c["sidebar"])
    s.configure("Sidebar.TLabel", background=c["sidebar"], foreground=c["text"])
    s.configure("SidebarDim.TLabel", background=c["sidebar"], foreground=c["text_dim"], font="C3WSmall")
    s.configure("Logo.TLabel", background=c["sidebar"], foreground=c["accent_text"], font="C3WLogo")
    s.configure("Surface.TFrame", background=c["surface"])
    s.configure("Surface.TLabel", background=c["surface"], foreground=c["text"])
    s.configure("SurfaceHint.TLabel", background=c["surface"], foreground=c["text_dim"])
    for name in ("Good", "Warn", "Bad"):
        s.configure(f"Surface{name}.TLabel", background=c["surface"], foreground=c[name.lower()], font="C3WBold")
    s.configure("TSeparator", background=c["divider"])
    s.configure("TLabelframe", background=c["bg"], bordercolor=c["divider"], lightcolor=c["bg"], darkcolor=c["bg"])
    s.configure("TLabelframe.Label", background=c["bg"], foreground=c["accent_text"], font="C3WBold")

    # Buttons
    pad = (px(12), px(5))
    s.configure("TButton", background=c["button"], foreground=c["text"], bordercolor=c["border"],
                lightcolor=c["button"], darkcolor=c["button"], focuscolor=c["focus"], focusthickness=1,
                padding=pad, relief="flat", anchor="center")
    s.map("TButton",
          background=[("disabled", c["button_disabled"]), ("pressed", c["button_active"]),
                      ("active", c["button_hover"])],
          lightcolor=[("disabled", c["button_disabled"]), ("pressed", c["button_active"]),
                      ("active", c["button_hover"])],
          darkcolor=[("disabled", c["button_disabled"]), ("pressed", c["button_active"]),
                     ("active", c["button_hover"])],
          bordercolor=[("disabled", c["divider"]), ("focus", c["accent"])],
          foreground=[("disabled", c["text_disabled"])])
    s.configure("Accent.TButton", background=c["accent"], foreground=c["accent_fg"], bordercolor=c["accent"],
                lightcolor=c["accent"], darkcolor=c["accent"], focuscolor=c["accent_fg"], font="C3WBold",
                padding=(px(18), px(6)))
    s.map("Accent.TButton",
          background=[("disabled", c["button_disabled"]), ("pressed", c["accent_active"]),
                      ("active", c["accent_hover"])],
          lightcolor=[("disabled", c["button_disabled"]), ("pressed", c["accent_active"]),
                      ("active", c["accent_hover"])],
          darkcolor=[("disabled", c["button_disabled"]), ("pressed", c["accent_active"]),
                     ("active", c["accent_hover"])],
          bordercolor=[("disabled", c["divider"])],
          foreground=[("disabled", c["text_disabled"]), ("active", c["accent_fg_hover"])])
    s.configure("Link.TButton", background=c["bg"], foreground=c["accent_text"], bordercolor=c["bg"],
                lightcolor=c["bg"], darkcolor=c["bg"], focuscolor=c["focus"], padding=(px(2), px(1)),
                relief="flat", anchor="w")
    s.map("Link.TButton", background=[("active", c["bg"]), ("pressed", c["bg"])],
          lightcolor=[("active", c["bg"])], darkcolor=[("active", c["bg"])],
          bordercolor=[("active", c["bg"]), ("focus", c["bg"])],
          foreground=[("disabled", c["text_disabled"]), ("active", c["text"])])
    s.configure("SidebarLink.TButton", background=c["sidebar"], foreground=c["accent_text"], bordercolor=c["sidebar"],
                lightcolor=c["sidebar"], darkcolor=c["sidebar"], focuscolor=c["focus"], padding=(0, px(1)),
                relief="flat", anchor="w")
    s.map("SidebarLink.TButton", background=[("active", c["sidebar"]), ("pressed", c["sidebar"])],
          lightcolor=[("active", c["sidebar"])], darkcolor=[("active", c["sidebar"])],
          bordercolor=[("active", c["sidebar"]), ("focus", c["sidebar"])],
          foreground=[("active", c["text"])])
    s.configure("Nav.TButton", background=c["sidebar"], foreground=c["text"], bordercolor=c["sidebar"],
                lightcolor=c["sidebar"], darkcolor=c["sidebar"], focuscolor=c["focus"], font="C3WNav",
                padding=(px(14), px(8)), relief="flat", anchor="w")
    s.map("Nav.TButton", background=[("pressed", c["sidebar_sel"]), ("active", c["sidebar_hover"])],
          lightcolor=[("pressed", c["sidebar_sel"]), ("active", c["sidebar_hover"])],
          darkcolor=[("pressed", c["sidebar_sel"]), ("active", c["sidebar_hover"])],
          bordercolor=[("active", c["sidebar_hover"]), ("focus", c["sidebar"])])
    # The game tabs across the top, and the strip they sit in.
    s.configure("TabBar.TFrame", background=c["tabbar"])
    s.configure("TabBar.TLabel", background=c["tabbar"], foreground=c["tab_text"], font="C3WSmall")
    for name, bg, fg in (("GameTab", c["tabbar"], c["tab_text"]), ("GameTabSelected", c["tabbar_sel"],
                                                                     c["tab_sel_text"])):
        s.configure(f"{name}.TButton", background=bg, foreground=fg, bordercolor=bg, lightcolor=bg, darkcolor=bg,
                    focuscolor=c["focus"], focusthickness=1, font="C3WTab", padding=(px(18), px(8)),
                    relief="flat", anchor="center")
        hover = c["tabbar_hover"] if name == "GameTab" else bg
        s.map(f"{name}.TButton", background=[("pressed", c["tabbar_sel"]), ("active", hover)],
              lightcolor=[("active", hover)], darkcolor=[("active", hover)],
              bordercolor=[("active", hover), ("focus", bg)], foreground=[("active", c["tab_sel_text"])])
    # Quiet buttons (Cancel, Open output folder): an outline only, so the
    # main buttons beside them stand out; the same height as those.
    s.configure("Ghost.TButton", background=c["bg"], foreground=c["text"], bordercolor=c["border"],
                lightcolor=c["bg"], darkcolor=c["bg"], relief="solid")
    s.map("Ghost.TButton",
          background=[("disabled", c["bg"]), ("pressed", c["button_active"]), ("active", c["button"])],
          lightcolor=[("disabled", c["bg"]), ("pressed", c["button_active"]), ("active", c["button"])],
          darkcolor=[("disabled", c["bg"]), ("pressed", c["button_active"]), ("active", c["button"])],
          bordercolor=[("disabled", c["divider"]), ("focus", c["accent"])],
          foreground=[("disabled", c["text_disabled"])])
    # The bottom bar's buttons: bigger, like a launcher's, all one height.
    s.configure("Big.Accent.TButton", font="C3WAction", padding=(px(26), px(9)))
    s.configure("Big.TButton", font="C3WAction", padding=(px(20), px(9)))
    s.configure("Big.Ghost.TButton", font="C3WAction", padding=(px(18), px(9)))
    # Status labels on the Port status pages.
    for name in ("good", "warn", "info", "dim"):
        s.configure(f"Chip{name.title()}.TLabel", background=c[f"chip_{name}_bg"], foreground=c[f"chip_{name}"],
                    font="C3WBold", padding=(px(8), px(2)), anchor="center")
    s.configure("NavSelected.TButton", background=c["sidebar_sel"], foreground=c["sidebar_sel_text"],
                bordercolor=c["sidebar_sel"], lightcolor=c["sidebar_sel"], darkcolor=c["sidebar_sel"],
                focuscolor=c["focus"], font="C3WNavBold", padding=(px(14), px(8)), relief="flat", anchor="w")
    s.map("NavSelected.TButton", background=[("active", c["sidebar_sel"])],
          lightcolor=[("active", c["sidebar_sel"])], darkcolor=[("active", c["sidebar_sel"])],
          bordercolor=[("focus", c["sidebar_sel"])])

    # Check boxes and radio buttons: filled with the accent when on.
    for w in ("TCheckbutton", "TRadiobutton"):
        s.configure(w, background=c["bg"], foreground=c["text"], indicatorbackground=c["field"],
                    indicatorforeground=c["accent_fg"], upperbordercolor=c["border"], lowerbordercolor=c["border"],
                    focuscolor=c["focus"], padding=(px(1), px(2)),
                    # Tk 8.6 draws the box at a fixed pixel size; grow it with the desktop scale.
                    indicatorsize=px(12), indicatormargin=(px(1), px(1), px(6), px(1)))
        s.map(w, background=[("active", c["bg"])],
              indicatorbackground=[("disabled", "selected", c["text_disabled"]), ("disabled", c["field_disabled"]),
                                   ("selected", c["accent"]), ("pressed", c["field_hover"]),
                                   ("active", c["field_hover"])],
              indicatorforeground=[("disabled", c["field_disabled"]), ("selected", c["accent_fg"])],
              upperbordercolor=[("disabled", c["divider"]), ("selected", c["accent"]), ("active", c["accent"])],
              lowerbordercolor=[("disabled", c["divider"]), ("selected", c["accent"]), ("active", c["accent"])],
              foreground=[("disabled", c["text_disabled"])])
        s.configure(f"Sidebar.{w}", background=c["sidebar"])
        s.map(f"Sidebar.{w}", background=[("active", c["sidebar"])])

    # Text fields, lists, number boxes
    field_pad = (px(6), px(4))
    for w in ("TEntry", "TCombobox", "TSpinbox"):
        s.configure(w, fieldbackground=c["field"], foreground=c["text"], bordercolor=c["border"],
                    lightcolor=c["field"], darkcolor=c["field"], insertcolor=c["text"], padding=field_pad,
                    selectbackground=c["select_bg"], selectforeground=c["select_fg"], background=c["button"],
                    arrowcolor=c["text"], arrowsize=px(12))
        s.map(w, fieldbackground=[("disabled", c["field_disabled"]), ("readonly", c["field"])],
              foreground=[("disabled", c["text_disabled"])],
              bordercolor=[("focus", c["accent"]), ("hover", c["accent_active"])],
              lightcolor=[("focus", c["field"])],
              background=[("disabled", c["field_disabled"]), ("pressed", c["button_active"]),
                          ("active", c["button_hover"])],
              arrowcolor=[("disabled", c["text_disabled"])])
    # A read-only list shows its choice as text, not as a selection.
    s.map("TCombobox", selectbackground=[("readonly", c["field"])], selectforeground=[("readonly", c["text"])],
          fieldbackground=[("disabled", c["field_disabled"]), ("readonly", "hover", c["field_hover"]),
                           ("readonly", c["field"])])

    # Progress and scroll bars: flat, no arrows.
    s.configure("Horizontal.TProgressbar", background=c["accent"], troughcolor=c["field"], bordercolor=c["field"],
                lightcolor=c["accent"], darkcolor=c["accent"], thickness=px(6))
    for orient in ("Vertical", "Horizontal"):
        sticky = "ns" if orient == "Vertical" else "ew"
        s.layout(f"{orient}.TScrollbar", [(f"{orient}.Scrollbar.trough", {"sticky": sticky, "children": [
            (f"{orient}.Scrollbar.thumb", {"expand": "1", "sticky": "nswe"})]})])
        s.configure(f"{orient}.TScrollbar", background=c["button"], troughcolor=c["bg"], bordercolor=c["bg"],
                    lightcolor=c["button"], darkcolor=c["button"], arrowcolor=c["text"], gripcount=0,
                    arrowsize=px(11), width=px(11))
        s.map(f"{orient}.TScrollbar",
              background=[("pressed", c["accent"]), ("active", c["button_hover"])],
              lightcolor=[("pressed", c["accent"]), ("active", c["button_hover"])],
              darkcolor=[("pressed", c["accent"]), ("active", c["button_hover"])])

    # Tables
    s.configure("Treeview", background=c["well"], fieldbackground=c["well"], foreground=c["text"],
                bordercolor=c["divider"], lightcolor=c["well"], darkcolor=c["well"], rowheight=linespace + px(8))
    s.map("Treeview", background=[("selected", c["select_bg"])], foreground=[("selected", c["select_fg"])])
    s.configure("Treeview.Heading", background=c["sidebar"], foreground=c["text_dim"], bordercolor=c["divider"],
                lightcolor=c["sidebar"], darkcolor=c["sidebar"], relief="flat", font="C3WBold",
                padding=(px(6), px(4)))
    s.map("Treeview.Heading", background=[("active", c["sidebar_hover"])])
    s.configure("TSizegrip", background=c["bg"])

    # The lists of list boxes made from now on. (Only those: Tk's own file
    # and color dialogs are partly plain Tk widgets that would turn patchy
    # with a dark default; the tool's own plain widgets are colored by
    # recolor().)
    opts = {"*TCombobox*Listbox.background": c["surface"], "*TCombobox*Listbox.foreground": c["text"],
            "*TCombobox*Listbox.selectBackground": c["select_bg"],
            "*TCombobox*Listbox.selectForeground": c["select_fg"],
            "*TCombobox*Listbox.font": "TkDefaultFont", "*TCombobox*Listbox.borderWidth": 0}
    for k, v in opts.items():
        root.option_add(k, v, "interactive")
    recolor_tree(root, c)
    for fn in list(listeners):
        try:
            fn(c)
        except Exception:  # noqa: BLE001 - a closed window's listener
            pass
    return c


def _linespace(root) -> int:
    import tkinter.font as tkfont
    try:
        return int(tkfont.nametofont("TkDefaultFont", root=root).metrics("linespace"))
    except Exception:  # noqa: BLE001
        return 16


def recolor(widget, c: Optional[Dict[str, str]] = None) -> None:
    """One plain Tk widget in the theme's colors. A widget can ask for a
    role with a `c3w_role` attribute ("sidebar", "divider", "accent",
    "surface", "well")."""
    c = c or current
    role = getattr(widget, "c3w_role", None)
    cls = widget.winfo_class()
    try:
        if role in ("sidebar", "divider", "accent", "surface", "well", "bg", "border"):
            widget.configure(background=c[role])
            if cls in ("Canvas", "Frame", "Toplevel"):
                widget.configure(highlightthickness=0)
        elif role == "tooltip":
            widget.configure(background=c["surface"], foreground=c["text"])
        elif role:                 # set by the app's own listener (the sidebar's page marker)
            pass
        elif cls in ("Tk", "Toplevel"):
            widget.configure(background=c["bg"])
        elif cls == "Canvas":
            widget.configure(background=c["bg"], highlightthickness=0)
        elif cls == "Frame":
            widget.configure(background=c["bg"])
        elif cls == "Text":
            widget.configure(background=c["well"], foreground=c["text"], insertbackground=c["text"],
                             selectbackground=c["select_bg"], selectforeground=c["select_fg"],
                             highlightbackground=c["divider"], highlightcolor=c["accent"],
                             inactiveselectbackground=c["select_bg"])
        elif cls == "Listbox":
            widget.configure(background=c["well"], foreground=c["text"], selectbackground=c["select_bg"],
                             selectforeground=c["select_fg"], highlightbackground=c["divider"],
                             highlightcolor=c["accent"], disabledforeground=c["text_disabled"])
    except Exception:  # noqa: BLE001 - an option this widget lacks
        pass
    if cls == "TCombobox":
        _recolor_popdown(widget, c)


def _recolor_popdown(combo, c: Dict[str, str]) -> None:
    """A list box's dropdown is made once, the first time it opens; one
    made under the old theme gets the new colors."""
    try:
        pop = f"{combo}.popdown"
        if not int(combo.tk.call("winfo", "exists", pop)):
            return     # not opened yet: it is made with the new colors
        lb = f"{pop}.f.l"
        combo.tk.call(lb, "configure", "-background", c["surface"], "-foreground", c["text"],
                      "-selectbackground", c["select_bg"], "-selectforeground", c["select_fg"])
        combo.tk.call(pop, "configure", "-background", c["border"])
    except Exception:  # noqa: BLE001
        pass


def recolor_tree(widget, c: Optional[Dict[str, str]] = None) -> None:
    """`widget` and everything in it (a window, a dialog)."""
    recolor(widget, c)
    for child in widget.winfo_children():
        recolor_tree(child, c)

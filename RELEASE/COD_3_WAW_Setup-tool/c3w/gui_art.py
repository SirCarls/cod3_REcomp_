"""The header across the top of each game tab: a backdrop drawn with plain
shapes on a Tk canvas (gradients, soft beams of light, silhouettes, a
stroke of paint) and the game's name set as a title.

Everything here is drawn by this code, after each game's mood: no logo, no
artwork and nothing from any game's files. Colors come from the tab's
palette (theme.colors), so the header follows the theme and accent.

The title always reads well: every shape goes through a Painter, which
keeps the backdrop dark where the title is (shapes there are darkened below
a set brightness, easing off to the right, where the drawings can be
brighter) and remembers the colors it drew there. The title's colors are
then checked against all of those colors, not only the plain background.
"""
from __future__ import annotations

import math
import random
import tkinter.font as tkfont
from typing import Callable, Dict, List, Optional, Sequence, Set, Tuple

from . import theme

# How bright (relative luminance) the backdrop may get behind the title:
# white text then has 5.5:1 at least (7:1 in High contrast).
CAP_TEXT, CAP_TEXT_STRONG, CAP_FREE = 0.14, 0.10, 0.30


class Painter:
    """Draws on the header's canvas. Shapes that start left of
    `zone_right` (where the title is) are kept dark and their colors are
    remembered (`under_text`); further right the limit eases off."""

    def __init__(self, cv, w: float, h: float, zone_right: float, cap_text: float):
        self.cv, self.w, self.h = cv, w, h
        self.zone_right = zone_right
        self.cap_text = cap_text
        self.under_text: Set[str] = set()

    def _fill(self, color: str, x0: float) -> str:
        ramp = max(1.0, self.w * 0.18)
        t = min(1.0, max(0.0, (x0 - self.zone_right) / ramp))
        col = theme.darken_to(color, self.cap_text + (CAP_FREE - self.cap_text) * t)
        if x0 < self.zone_right:
            self.under_text.add(col)
        return col

    def fill_color(self, color: str, x0: float) -> str:
        """The color a shape starting at x0 gets for `color`."""
        return self._fill(color, x0)

    def polygon(self, pts: Sequence[float], color: str) -> None:
        self.cv.create_polygon(list(pts), fill=self._fill(color, min(pts[0::2])), outline="")

    def rect(self, x0: float, y0: float, x1: float, y1: float, color: str) -> None:
        self.cv.create_rectangle(x0, y0, x1, y1, fill=self._fill(color, min(x0, x1)), outline="")

    def oval(self, x0: float, y0: float, x1: float, y1: float, color: str) -> None:
        self.cv.create_oval(x0, y0, x1, y1, fill=self._fill(color, min(x0, x1)), outline="")

    def line(self, pts: Sequence[float], color: str, width: float = 1.0) -> None:
        self.cv.create_line(list(pts), fill=self._fill(color, min(pts[0::2])), width=max(1.0, width))

    def gradient(self, x0: float, y0: float, x1: float, y1: float, first: str, last: str,
                 vertical: bool = False) -> None:
        """A smooth fill from `first` to `last` (top to bottom when
        vertical): one band per pixel or two, so no steps show."""
        span = (y1 - y0) if vertical else (x1 - x0)
        if span <= 0:
            return
        steps = max(2, min(int(span), 320))
        for i in range(steps):
            a, b = span * i / steps, span * (i + 1) / steps + 1
            color = theme.mix(first, last, i / (steps - 1))
            if vertical:
                self.rect(x0, y0 + a, x1, y0 + b, color)
            else:
                self.rect(x0 + a, y0, x0 + b, y1, color)


# ---------------------------------------------------------------------------
# Shapes
# ---------------------------------------------------------------------------
def _beam(p: Painter, x: float, y: float, angle: float, spread: float, length: float,
          color_at: Callable[[float, float], str], light: str, strength: float) -> None:
    """A searchlight's beam from (x, y): lighter than the sky behind it
    (color_at(x, y) gives the backdrop's color there), brightest along its
    middle (four layers, the narrower ones brighter) and fading to nothing
    along its length, so it has no hard edge or end."""
    layers, slices = 4, 26
    up, across = math.sin(math.radians(angle)), math.cos(math.radians(angle))
    for k in range(layers):
        half = spread * (1 - k / (layers + 0.5))
        a0, a1 = math.radians(angle - half), math.radians(angle + half)
        st = strength * (k + 1) / layers
        for i in range(slices):
            t0, t1 = i / slices, (i + 1) / slices
            r0, r1 = length * t0, length * t1 + 1
            pts = [x + r0 * math.cos(a0), y + r0 * math.sin(a0), x + r1 * math.cos(a0), y + r1 * math.sin(a0),
                   x + r1 * math.cos(a1), y + r1 * math.sin(a1), x + r0 * math.cos(a1), y + r0 * math.sin(a1)]
            mid = (r0 + r1) / 2
            fade = (1 - (t0 + t1) / 2) ** 1.6
            p.polygon(pts, theme.mix(color_at(x + mid * across, y + mid * up), light, st * fade))


def _star(cx: float, cy: float, r: float, inner: float = 0.382, rotation: float = -90) -> List[float]:
    """A five-pointed star's outline."""
    pts: List[float] = []
    for i in range(10):
        rad = r if i % 2 == 0 else r * inner
        a = math.radians(rotation + i * 36)
        pts += [cx + rad * math.cos(a), cy + rad * math.sin(a)]
    return pts


def _ridge(rng: random.Random, x0: float, x1: float, base: float, amp: float, bumps: float,
           bottom: float) -> List[float]:
    """A soft hill line from x0 to x1 (a filled silhouette down to `bottom`)."""
    phases = [rng.uniform(0, math.tau) for _ in range(3)]
    pts: List[float] = [x0, bottom]
    n = 90
    for i in range(n + 1):
        x = x0 + (x1 - x0) * i / n
        t = i / n * bumps * math.tau
        y = base - amp * (0.55 * math.sin(t + phases[0]) + 0.3 * math.sin(2.3 * t + phases[1]) +
                          0.15 * math.sin(5.1 * t + phases[2]))
        pts += [x, y]
    pts += [x1, bottom]
    return pts


def _brush(rng: random.Random, x0: float, y0: float, x1: float, y1: float, rough: float) -> List[float]:
    """A stroke of paint: a band whose edges wander slowly (a few waves)
    with a little grain, thick in the middle and drawn out at the right
    end, where the brush ran dry."""
    phases = [rng.uniform(0, math.tau) for _ in range(4)]
    top, bottom = [], []
    n = 150
    mid, half_h = (y0 + y1) / 2, (y1 - y0) / 2
    for i in range(n + 1):
        t = i / n
        x = x0 + (x1 - x0) * t
        taper = min(1.0, t / 0.04) ** 0.5 * (1 - t) ** 0.55   # a quick start, a long dry end
        wave = math.sin(t * 9 + phases[0]) * 0.35 + math.sin(t * 23 + phases[1]) * 0.15
        top.append((x, mid - half_h * taper * (1 + 0.12 * wave) + rng.uniform(-rough, rough)
                    + math.sin(t * 5 + phases[2]) * half_h * 0.08))
        bottom.append((x, mid + half_h * taper * (1 - 0.10 * wave) + rng.uniform(-rough, rough)
                       + math.sin(t * 4 + phases[3]) * half_h * 0.08))
    pts: List[float] = []
    for x, y in top + bottom[::-1]:
        pts += [x, y]
    return pts


def _streaks(p: Painter, rng: random.Random, x_from: float, x_to: float, y0: float, y1: float,
             color: str, count: int) -> None:
    """Dry-brush streaks: thin strands where a stroke runs out of paint."""
    for k in range(count):
        y = y0 + (y1 - y0) * (k + rng.uniform(0.2, 0.8)) / count
        th = (y1 - y0) / count * rng.uniform(0.25, 0.6)
        end = x_from + (x_to - x_from) * rng.uniform(0.45, 1.0)
        p.polygon([x_from, y - th / 2, (x_from + end) / 2, y - th * 0.45, end, y - th * 0.08, end + th * 1.5, y,
                   end, y + th * 0.12, (x_from + end) / 2, y + th * 0.4, x_from, y + th / 2], color)


def _flak(p: Painter, rng: random.Random, x: float, y: float, r: float, sky: str, light: str) -> None:
    """A burst of flak: a small glow and short rays."""
    for k, f in enumerate((1.0, 0.62, 0.32)):
        p.oval(x - r * f * 0.45, y - r * f * 0.45, x + r * f * 0.45, y + r * f * 0.45,
               theme.mix(sky, light, 0.10 + 0.14 * k))
    ray = theme.mix(sky, light, 0.30)
    for k in range(10):
        a = math.radians(k * 36 + rng.uniform(-8, 8))
        p.line([x + r * 0.5 * math.cos(a), y + r * 0.5 * math.sin(a), x + r * math.cos(a), y + r * math.sin(a)],
               ray, max(1.0, r / 14))


def _parachute(p: Painter, x: float, y: float, size: float, color: str) -> None:
    """A paratrooper under a round canopy, as a silhouette."""
    w, hgt = size, size * 0.42
    pts: List[float] = []
    for i in range(19):                         # the dome
        a = math.pi + math.pi * i / 18
        pts += [x + w / 2 * math.cos(a), y + hgt * math.sin(a)]
    for i in range(4, -1, -1):                  # its scalloped hem
        pts += [x - w / 2 + w * i / 4, y + (hgt * 0.10 if i % 2 else 0)]
    p.polygon(pts, color)
    body_y = y + size * 0.78
    for fx in (-0.5, -0.2, 0.2, 0.5):
        p.line([x + w * fx, y + hgt * 0.05, x, body_y - size * 0.10], color, max(1.0, size / 40))
    head = size * 0.07
    p.oval(x - head, body_y - size * 0.12 - head, x + head, body_y - size * 0.12 + head, color)
    p.rect(x - size * 0.06, body_y - size * 0.10, x + size * 0.06, body_y + size * 0.12, color)


# ---------------------------------------------------------------------------
# Backdrops, one per game
# ---------------------------------------------------------------------------
def _backdrop_cod3(p: Painter, w: float, h: float, c: Dict[str, str], rng: random.Random) -> None:
    """Brass light over black: rays from the lower right, a faint field-map
    grid, and a large army star in a ring, cut off at the right edge."""
    p.gradient(0, 0, w, h, theme.mix(c["tabbar"], "#000000", 0.2), theme.mix(c["bg"], c["accent"], 0.20))
    cx, cy = w * 0.86, h * 1.05
    for i in range(16):
        a0 = math.radians(192 + i * 8.6)
        a1 = a0 + math.radians(3.6)
        r = w
        p.polygon([cx, cy, cx + r * math.cos(a0), cy + r * math.sin(a0), cx + r * math.cos(a1),
                   cy + r * math.sin(a1)], theme.mix(c["bg"], c["accent"], 0.11 + 0.05 * (i % 2)))
    grid = theme.mix(c["bg"], c["accent"], 0.09)
    step = max(18.0, h / 4)
    x = 0.0
    while x < w * 0.55:
        p.line([x, 0, x, h], grid)
        x += step
    y = step / 2
    while y < h:
        p.line([0, y, w * 0.55, y], grid)
        y += step
    r = h * 0.95
    sx, sy = w - h * 0.55, h * 0.5
    p.oval(sx - r, sy - r, sx + r, sy + r, theme.mix(c["bg"], c["accent"], 0.30))
    p.oval(sx - r * 0.93, sy - r * 0.93, sx + r * 0.93, sy + r * 0.93, theme.mix(c["bg"], c["accent"], 0.12))
    p.polygon(_star(sx, sy, r * 0.84), theme.mix(c["bg"], c["accent"], 0.26))
    p.polygon(_star(sx, sy, r * 0.62), theme.mix(c["bg"], c["accent"], 0.20))


def _backdrop_waw(p: Painter, w: float, h: float, c: Dict[str, str], rng: random.Random) -> None:
    """Charcoal with a low red sun, a long stroke of red paint behind the
    title that runs dry to the right, spatters, and barbed wire."""
    p.gradient(0, 0, w, h, theme.mix(c["tabbar"], "#000000", 0.25), theme.mix(c["bg"], c["accent"], 0.10))
    sx, sy = w * 0.82, h * 0.60
    rings = 30
    for i in range(rings):                       # a soft glow: many close steps
        f = i / (rings - 1)
        r = h * (1.35 - 1.12 * f)
        p.oval(sx - r, sy - r, sx + r, sy + r, theme.mix(c["bg"], c["accent"], 0.05 + 0.40 * f ** 1.4))
    end = w * 0.80
    paint, paint2 = theme.mix(c["bg"], c["accent"], 0.44), theme.mix(c["bg"], c["accent"], 0.64)
    p.polygon(_brush(rng, -h * 0.3, h * 0.26, end, h * 0.86, h * 0.025), paint)
    _streaks(p, rng, end * 0.88, end + h * 0.3, h * 0.36, h * 0.80, paint, 8)
    p.polygon(_brush(rng, h * 0.2, h * 0.70, w * 0.56, h * 0.90, h * 0.012), paint2)
    _streaks(p, rng, w * 0.50, w * 0.56 + h * 0.3, h * 0.72, h * 0.88, paint2, 3)
    for _ in range(34):                          # spatter, thickest near the stroke's end
        x = min(w * 0.99, max(w * 0.30, rng.gauss(end + h * 0.4, w * 0.12)))
        y = rng.uniform(h * 0.05, h * 0.95)
        r = rng.choice((1, 1, 1.5, 2, 2, 3, 4.5)) * max(1.0, h / 110)
        p.oval(x - r, y - r, x + r, y + r, theme.mix(c["bg"], c["accent"], 0.50))
    p.polygon(_ridge(rng, 0, w, h * 0.95, h * 0.04, 7, h + 2), theme.mix(c["bg"], "#000000", 0.5))
    # Barbed wire on the right: leaning posts and two sagging wires with barbs.
    wire = theme.mix(c["bg"], "#000000", 0.55)
    posts = []
    x = w * 0.64
    while x < w + h * 0.5:
        lean = rng.uniform(-0.08, 0.08) * h
        top = h * rng.uniform(0.56, 0.64)
        p.line([x, h * 0.98, x + lean, top], wire, max(2.0, h / 34))
        posts.append((x + lean * 0.75, top + h * 0.06))
        x += h * rng.uniform(0.62, 0.8)
    for drop in (0.0, 0.17):
        for (ax, ay), (bx, by) in zip(posts, posts[1:]):
            pts: List[float] = []
            for i in range(13):
                t = i / 12
                pts += [ax + (bx - ax) * t, ay + (by - ay) * t + h * drop + math.sin(math.pi * t) * h * 0.07]
            p.line(pts, wire, max(1.0, h / 90))
            for i in range(2, 12, 3):
                bxp, byp = pts[i * 2], pts[i * 2 + 1]
                d = h * 0.022
                p.line([bxp - d, byp - d, bxp + d, byp + d], wire, max(1.0, h / 110))
                p.line([bxp - d, byp + d, bxp + d, byp - d], wire, max(1.0, h / 110))


def _backdrop_cod2(p: Painter, w: float, h: float, c: Dict[str, str], rng: random.Random) -> None:
    """Gunmetal sky over a desert horizon glowing orange at the right, with
    a far-off town, dunes and the soft beams of two searchlights."""
    horizon = h * 0.76
    # The sky is one glow around the low sun at the right: rings from far
    # (dark gunmetal, the title's side) to near (orange), in small steps.
    gx, rx, ry = w * 0.80, max(w * 0.95, h * 4), h * 1.5
    dark, warm = theme.mix(c["tabbar"], "#000000", 0.1), theme.mix(c["bg"], c["accent"], 0.62)
    p.rect(0, 0, w, h + 2, dark)
    rings = 72
    for i in range(rings):
        f = i / rings
        p.oval(gx - rx * (1 - f), horizon - ry * (1 - f), gx + rx * (1 - f), horizon + ry * (1 - f),
               theme.mix(dark, warm, f ** 2.2))

    def sky(x: float, y: float) -> str:
        """The sky's color at (x, y)."""
        d = math.hypot((x - gx) / rx, (y - horizon) / ry)
        return theme.mix(dark, warm, max(0.0, 1.0 - d) ** 2.2)
    for bx, ang in ((w * 0.60, -66), (w * 0.95, -112)):
        _beam(p, bx, horizon, ang, 5.0, horizon * 0.95 / math.sin(math.radians(abs(ang))), sky, c["text"], 0.42)
    town = theme.mix(c["bg"], "#000000", 0.30)
    x = w * 0.66
    while x < w * 0.99:                          # flat roofs, domes and a minaret
        bw = h * rng.uniform(0.10, 0.22)
        bh = h * rng.uniform(0.03, 0.09)
        p.rect(x, horizon - bh, x + bw, horizon + 4, town)
        roll = rng.random()
        if roll < 0.25:
            d = bw * 0.32
            p.oval(x + bw / 2 - d, horizon - bh - d, x + bw / 2 + d, horizon - bh + d, town)
        elif roll < 0.33:
            mw = h * 0.025
            p.rect(x + bw * 0.7, horizon - bh - h * 0.16, x + bw * 0.7 + mw, horizon - bh, town)
            p.oval(x + bw * 0.7 - mw * 0.3, horizon - bh - h * 0.19, x + bw * 0.7 + mw * 1.3,
                   horizon - bh - h * 0.14, town)
        x += bw + h * rng.uniform(0.0, 0.06)
    p.polygon(_ridge(rng, 0, w, h * 0.80, h * 0.06, 3, h + 2), theme.mix(c["bg"], c["border"], 0.35))
    p.polygon(_ridge(rng, 0, w, h * 0.92, h * 0.05, 5, h + 2), theme.mix(c["bg"], "#000000", 0.35))


def _backdrop_coduo(p: Painter, w: float, h: float, c: Dict[str, str], rng: random.Random,
                    zone_right: float) -> None:
    """Olive night: crossed searchlights, flak bursts, paratroopers coming
    down and a line of pines (low where the title is)."""
    top, low = theme.mix(c["tabbar"], "#000000", 0.2), theme.mix(c["bg"], c["accent"], 0.24)
    p.gradient(0, 0, w, h, top, low, vertical=True)

    def sky(x: float, y: float) -> str:
        return theme.mix(top, low, min(1.0, max(0.0, y) / h))
    for bx, ang in ((w * 0.57, -62), (w * 0.97, -120), (w * 0.79, -86)):
        _beam(p, bx, h, ang, 4.6, h * 0.92 / math.sin(math.radians(abs(ang))), sky, c["accent_text"], 0.40)
    for _ in range(9):
        x, y = rng.uniform(w * 0.48, w * 0.98), rng.uniform(h * 0.08, h * 0.50)
        _flak(p, rng, x, y, rng.uniform(0.045, 0.085) * h, sky(x, y), c["accent_text"])
    shade = theme.mix(top, "#000000", 0.35)
    for fx, fy, size in ((0.62, 0.20, 0.17), (0.71, 0.36, 0.13), (0.86, 0.14, 0.15), (0.93, 0.42, 0.11)):
        _parachute(p, w * fx, h * fy, h * size, shade)
    pts: List[float] = [0.0, h + 2]
    x = 0.0
    while x <= w + h * 0.1:
        tall = rng.uniform(0.10, 0.22) * h * (0.45 if x < zone_right else 1.0)
        pts += [x, h * 0.95, x + h * 0.04, h * 0.95 - tall, x + h * 0.08, h * 0.95]
        x += h * rng.uniform(0.07, 0.13)
    pts += [w, h + 2]
    p.polygon(pts, theme.mix(c["bg"], "#000000", 0.45))


BACKDROPS = {"cod3": _backdrop_cod3, "waw": _backdrop_waw, "cod2": _backdrop_cod2}


# ---------------------------------------------------------------------------
# The title
# ---------------------------------------------------------------------------
def _fit(family: str, weight: str, size: int, texts: Sequence[str], room: float, tracking: float = 0) -> int:
    """The biggest pixel size up to `size` at which the widest text fits."""
    while size > 10:
        f = tkfont.Font(family=family, size=-size, weight=weight)
        widest = max(f.measure(t) + tracking * size * len(t) for t in texts)
        if widest <= room:
            break
        size = int(size * 0.92)
    return size


def _text(cv, x: float, baseline: float, text: str, font: tuple, fill: str) -> None:
    """Text whose baseline is at `baseline` (Tk anchors at the bottom of the
    line, under the descenders: fonts of different sizes wouldn't line up)."""
    cv.create_text(x, baseline + tkfont.Font(font=font).metrics("descent"), text=text, font=font, fill=fill,
                   anchor="sw")


def _spaced(cv, x: float, baseline: float, text: str, font: tuple, fill: str, tracking: float,
            big_initials: Optional[tuple] = None) -> float:
    """Text with extra space between the letters (and, with `big_initials`,
    each word's first letter in that bigger font) on one baseline; returns
    where it ends."""
    f, fb = tkfont.Font(font=font), tkfont.Font(font=big_initials) if big_initials else None
    start = True
    for ch in text:
        use, measure = (big_initials, fb) if fb and start and ch != " " else (font, f)
        _text(cv, x, baseline, ch, use, fill)
        x += measure.measure(ch) + tracking
        start = ch == " "
    return x


def _spaced_width(text: str, font: tuple, tracking: float, big_initials: Optional[tuple] = None) -> float:
    f, fb = tkfont.Font(font=font), tkfont.Font(font=big_initials) if big_initials else None
    width, start = 0.0, True
    for ch in text:
        width += (fb if fb and start and ch != " " else f).measure(ch) + tracking
        start = ch == " "
    return width - tracking


def hero_colors(c: Dict[str, str]) -> Dict[str, str]:
    """The header is always a dark banner: under a light theme its colors
    are turned around (the theme's ink as the backdrop, its paper as text)."""
    if not c.get("light"):
        return c
    ink = theme.mix(c["text"], "#000000", 0.35)
    d = dict(c)
    d.update(bg=ink, tabbar=theme.mix(ink, "#000000", 0.3), text=c["bg"], text_dim=theme.mix(c["bg"], ink, 0.3),
             border=theme.mix(ink, c["bg"], 0.3), accent_text=theme.ensure_contrast(c["accent"], [ink], 4.5))
    return d


def draw_hero(cv, game_id: str, wordmark: Tuple[str, str], tagline: str, c: Dict[str, str],
              display: Tuple[str, str], small_family: str, ui_family: str, strong: bool = False) -> Dict[str, object]:
    """Draws the header for a game on canvas `cv` at its current size.
    `display`: the title's (family, weight); `small_family`: the small
    capitals' type; `ui_family`: the tagline's; `strong`: High contrast
    (7:1 instead of 4.5:1). Returns the text colors and the colors drawn
    under the text."""
    cv.delete("all")
    c = hero_colors(c)
    cv.configure(background=c["bg"])     # under any gap the shapes leave
    w, h = max(cv.winfo_width(), 50), max(cv.winfo_height(), 30)
    rng = random.Random(sum(ord(ch) for ch in game_id) * 7919)   # the same picture every time
    target = theme.HIGH_CONTRAST if strong else theme.BODY_CONTRAST

    # Lay out the text first: the backdrop is kept dark where it goes.
    family, weight = display
    left = h * 0.26
    room = max(80.0, w * 0.62 - left)
    top, big = wordmark
    stacked = len(big) > 2
    if stacked:
        # United Offensive: spaced, with bigger initials; World at War: set tight, like a poster.
        tracking = 0.05 if game_id == "coduo" else -0.015
        size = _fit(family, weight, int(h * 0.38), [big], room, tracking=tracking * 1.25)
        font = (family, -size, weight)
        initials = (family, -int(size * 1.22), weight) if game_id == "coduo" else None
        title_w = _spaced_width(big, font, size * tracking, initials)
        small = max(11, int(size * 0.34))
        small_font = (small_family, -small, "bold")
        title_w = max(title_w, _spaced_width(top, small_font, small * 0.45))
    else:
        size = _fit(family, weight, int(h * 0.42), [top + "  " + big], room)
        font = (family, -size, weight)
        num_font = (family, -int(size * 1.25), weight)
        title_w = tkfont.Font(font=font).measure(top + " ") + tkfont.Font(font=num_font).measure(big)
    tag = max(11, int(h * 0.115))
    tag_font = (ui_family, -tag, "normal")
    tag_w = tkfont.Font(font=tag_font).measure(tagline)
    zone_right = left + max(title_w, tag_w) + h * 0.5

    p = Painter(cv, w, h, zone_right, CAP_TEXT_STRONG if strong else CAP_TEXT)
    if game_id == "coduo":
        _backdrop_coduo(p, w, h, c, rng, zone_right)
    else:
        BACKDROPS.get(game_id, _backdrop_cod3)(p, w, h, c, rng)

    under = sorted(p.under_text | {c["bg"]})
    text = theme.ensure_contrast(c["text"], under, target)
    accent = theme.ensure_contrast(c["accent_text"], under, 4.5 if strong else 3.0)
    small_color = theme.ensure_contrast(c["accent_text"], under, target)
    shadow = theme.mix(c["bg"], "#000000", 0.7)
    base = h * 0.56                     # the title's baseline
    if stacked:
        # "CALL OF DUTY" small and spaced over "WORLD AT WAR" / "UNITED OFFENSIVE" big.
        # About the capitals' height: 0.66 to 0.79 of the size in common
        # fonts (Tk's ascent varies too much from font to font to go by).
        cap = size * (1.22 if initials else 1.0) * 0.76
        _spaced(cv, left + 1, base - cap - small * 0.6, top, small_font, small_color, small * 0.45)
        under_copy = shadow
        if game_id == "waw" and not strong:
            # Printed slightly off register: a deep red copy under the bone-white one
            # (kept deep in any theme: a light accent would only look blurred).
            under_copy = theme.darken_to(theme.mix(c["accent"], "#000000", 0.25), 0.06)
        off = max(2.0, h / 45)
        _spaced(cv, left + off, base + off, big, font, under_copy, size * tracking, big_initials=initials)
        end = _spaced(cv, left, base, big, font, text, size * tracking, big_initials=initials) - size * tracking
        if game_id == "waw" and not strong:
            # Worn paint: small flecks of the stroke's red across the letters.
            fleck = p.fill_color(theme.mix(c["bg"], c["accent"], 0.44), 0)
            wear = random.Random(1945)
            for _ in range(int((end - left) / 5)):
                x, y = wear.uniform(left, end), wear.uniform(base - cap, base - size * 0.04)
                r = wear.uniform(0.5, 1.5) * max(1.0, h / 110)
                cv.create_oval(x - r, y - r, x + r, y + r, fill=fleck, outline="")
    else:
        # "CALL OF DUTY 3": one line, the number bigger and in the accent color.
        f = tkfont.Font(font=font)
        nx = left + f.measure(top + " ")
        if game_id == "cod2":
            # Engraved: a faint light edge above, a shadow below.
            lift = theme.mix(text, "#ffffff", 0.4)
            copies = ((-1, -1, theme.mix(c["bg"], lift, 0.35)), (2, 2, shadow))
        else:
            copies = ((2, 2, shadow),)
        for dx, dy, col in copies:
            _text(cv, left + dx, base + dy, top, font, col)
            _text(cv, nx + dx, base + dy, big, num_font, col)
        _text(cv, left, base, top, font, text)
        _text(cv, nx, base, big, num_font, accent)
        end = nx + tkfont.Font(font=num_font).measure(big)
    # A rule under the title with a small diamond, then the tagline.
    rule = theme.ensure_contrast(c["accent"], under, 3.0)
    ry = h * 0.70
    cv.create_line(left, ry, end, ry, fill=rule, width=max(2, int(h / 60)))
    d = max(3.0, h / 32)
    cv.create_polygon(end + d * 2, ry - d, end + d * 3, ry, end + d * 2, ry + d, end + d, ry, fill=rule, outline="")
    tag_color = theme.ensure_contrast(c["text_dim"], under, target)
    cv.create_text(left, ry + h * 0.065, text=tagline, font=tag_font, fill=tag_color, anchor="nw")
    # The edge between the header and the window below.
    cv.create_rectangle(0, h - max(2, h / 50), w, h, fill=c["accent"], outline="")
    return {"text": text, "tagline": tag_color, "small": small_color, "accent": accent, "under": under}

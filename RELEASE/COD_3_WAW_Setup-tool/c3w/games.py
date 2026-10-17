"""The game tabs across the top of the setup tool's window: Call of Duty 3,
World at War, Call of Duty 2 and United Offensive. Each tab has its own
sidebar of pages, its own look (theme.py has the colors) and its own main
action in the bottom bar.

This module is plain data and logic (no Tk), so the tests run without
Tkinter: the game table, which pages each tab shows, the tab state that
settings.json keeps (the last tab, and each tab's last page, theme and
accent color), and how far each port is.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

COD3, WAW, COD2, CODUO = "cod3", "waw", "cod2", "coduo"
DEFAULT_GAME = COD3


@dataclass(frozen=True)
class Game:
    id: str                  # the value saved in settings.json ("ui_game")
    name: str                # "Call of Duty 2"
    tab: str                 # the tab's label
    short: str               # the tab's label in a narrow window
    wordmark: Tuple[str, str]  # the header's title: a small top line and the big line
    tagline: str             # one short line under the title
    palette: str             # the tab's own colors (theme.GAME_PALETTES, or the "cod3" theme)
    pages: Tuple[str, ...]   # the sidebar's pages, in order (Ctrl+1 ...)
    actions: Tuple[str, ...]  # the bottom bar's buttons, main one first


# The page keys and their labels. "log" is in every tab's sidebar (one page,
# shared: everything the tool and the programs it starts write goes there).
PAGE_LABELS: Dict[str, str] = {
    "setup": "Set up", "play": "Play", "textures": "Textures", "tools": "Tools", "log": "Log",
    "about": "About",
    "waw_content": "Content", "maps": "Mods & maps", "waw_status": "Port status",
    "cod2_copy": "New copy", "cod2_server": "LAN server", "cod2_status": "Port status",
    "coduo_copy": "New copy", "coduo_server": "LAN server", "coduo_status": "Port status",
}
SHARED_PAGES = ("log",)

GAMES: Tuple[Game, ...] = (
    Game(COD3, "Call of Duty 3", "Call of Duty 3", "CoD3", ("CALL OF DUTY", "3"),
         "Recompiled for PC from your own disc", "cod3",
         ("setup", "play", "textures", "tools", "log", "about"), ("build", "play")),
    Game(WAW, "World at War", "World at War", "WaW", ("CALL OF DUTY", "WORLD AT WAR"),
         "Its content, ripped from your own copy", "waw",
         ("waw_content", "maps", "waw_status", "log"), ("rip",)),
    Game(COD2, "Call of Duty 2", "Call of Duty 2", "CoD2", ("CALL OF DUTY", "2"),
         "Rust engine tools: new copy and LAN server", "cod2",
         ("cod2_copy", "cod2_server", "cod2_status", "log"), ("copy", "server")),
    Game(CODUO, "Call of Duty: United Offensive", "United Offensive", "UO", ("CALL OF DUTY", "UNITED OFFENSIVE"),
         "Rust engine tools: new copy and LAN server", "coduo",
         ("coduo_copy", "coduo_server", "coduo_status", "log"), ("copy", "server")),
)
GAME_ORDER: List[str] = [g.id for g in GAMES]
BY_ID: Dict[str, Game] = {g.id: g for g in GAMES}


def game(game_id: str) -> Game:
    """The game with this id; the default game for an unknown one."""
    return BY_ID.get(game_id) or BY_ID[DEFAULT_GAME]


def available_pages(enabled: Iterable[str] = ()) -> Dict[str, List[str]]:
    """Each tab's pages as the window shows them: optional pages (only "maps"
    for now, World at War's mods; waw.MODS_ENABLED) only when `enabled`
    names them."""
    on = set(enabled)
    return {g.id: [p for p in g.pages if p != "maps" or "maps" in on] for g in GAMES}


def owner_of(page: str, pages: Dict[str, List[str]], prefer: str = "") -> Optional[str]:
    """The tab that shows `page`: `prefer` when it has it (a shared page such
    as the log stays on the tab in view), else the first tab that does."""
    if prefer in pages and page in pages[prefer]:
        return prefer
    return next((g for g in GAME_ORDER if page in pages.get(g, ())), None)


@dataclass
class TabState:
    """Which tab is in view, and each tab's last page, theme and accent:
    what settings.json keeps ("ui_game", "ui_pages", "ui_themes",
    "ui_accents"). Every value read from a file is checked; anything unknown
    falls back to the default."""
    pages_available: Dict[str, List[str]]
    game: str = DEFAULT_GAME
    pages: Dict[str, str] = field(default_factory=dict)
    themes: Dict[str, str] = field(default_factory=dict)
    accents: Dict[str, str] = field(default_factory=dict)

    @classmethod
    def from_settings(cls, s: dict, pages_available: Dict[str, List[str]], theme_ids: Sequence[str] = (),
                      launcher: bool = False) -> "TabState":
        """The state saved in settings `s`. `theme_ids`: the palettes the
        theme module knows (a saved one it doesn't is dropped). Settings
        from before the tabs ("ui_page", "ui_theme", "ui_accent") become the
        Call of Duty 3 tab's. `launcher`: opened from a built game's folder,
        on Call of Duty 3's Play page."""
        st = cls(pages_available)
        pages = s.get("ui_pages") if isinstance(s.get("ui_pages"), dict) else {}
        old_page = s.get("ui_page")
        if isinstance(old_page, str) and COD3 not in pages:
            owner = owner_of(old_page, pages_available)   # "maps" moved to World at War
            if owner:
                pages = dict(pages, **{owner: old_page})
        for g, p in pages.items():
            if g in pages_available and isinstance(p, str) and p in pages_available[g]:
                st.pages[g] = p
        themes = s.get("ui_themes") if isinstance(s.get("ui_themes"), dict) else {}
        if isinstance(s.get("ui_theme"), str) and COD3 not in themes:
            themes = dict(themes, **{COD3: s["ui_theme"]})
        known = set(theme_ids)
        for g, t in themes.items():
            if g in BY_ID and isinstance(t, str) and (not known or t in known) and t != BY_ID[g].palette:
                st.themes[g] = t   # (a tab's own colors are its default: not kept)
        accents = s.get("ui_accents") if isinstance(s.get("ui_accents"), dict) else {}
        if isinstance(s.get("ui_accent"), str) and COD3 not in accents:
            accents = dict(accents, **{COD3: s["ui_accent"]})
        for g, a in accents.items():
            if g in BY_ID and isinstance(a, str) and a.strip() and _is_color(a):
                st.accents[g] = a.strip().lower()
        saved_game = s.get("ui_game")
        st.game = saved_game if isinstance(saved_game, str) and saved_game in pages_available else DEFAULT_GAME
        if launcher:
            st.game = COD3
            st.pages[COD3] = "play"
        return st

    def to_settings(self) -> dict:
        """What settings.json keeps. The Call of Duty 3 tab's page, theme and
        accent also go under the old names, for an older tool reading the
        same file."""
        return {"ui_game": self.game, "ui_pages": dict(self.pages), "ui_themes": dict(self.themes),
                "ui_accents": dict(self.accents), "ui_page": self.page_for(COD3),
                "ui_theme": self.theme_for(COD3), "ui_accent": self.accents.get(COD3, "")}

    def page_for(self, game_id: str) -> str:
        """The page a tab opens on: the last one shown, else its first."""
        pages = self.pages_available.get(game_id) or self.pages_available[DEFAULT_GAME]
        page = self.pages.get(game_id)
        return page if page in pages else pages[0]

    def theme_for(self, game_id: str) -> str:
        """The tab's colors: the theme picked for it, else its own palette."""
        return self.themes.get(game_id) or game(game_id).palette

    def accent_for(self, game_id: str) -> str:
        return self.accents.get(game_id, "")

    def show(self, game_id: str, page: Optional[str] = None) -> Tuple[str, str]:
        """Switches to a tab (and one of its pages); returns (tab, page) as
        they end up (an unknown tab or page leaves the last one)."""
        if game_id in self.pages_available:
            self.game = game_id
        if page is not None and page in self.pages_available[self.game]:
            self.pages[self.game] = page
        return self.game, self.page_for(self.game)

    def step(self, delta: int) -> str:
        """The tab `delta` places to the right (negative: left), wrapping."""
        order = [g for g in GAME_ORDER if g in self.pages_available]
        i = order.index(self.game) if self.game in order else 0
        return order[(i + delta) % len(order)]


def _is_color(text: str) -> bool:
    t = text.strip()
    return t == "" or (len(t) == 7 and t[0] == "#" and all(c in "0123456789abcdefABCDEF" for c in t[1:]))


# ---------------------------------------------------------------------------
# The header's height (gui_art draws it): it follows the window's, 15% of
# it within these bounds (96 DPI pixels). Under HERO_HIDE_BELOW the header
# makes way for the page; the tabs still say which game it is.
# ---------------------------------------------------------------------------
HERO_MIN_H, HERO_MAX_H, HERO_SHARE = 100, 140, 0.15
HERO_H = 120
HERO_HIDE_BELOW = 720


def hero_height(window_h: int, scale: float = 1.0) -> int:
    """The header's height for a window this tall (0: hidden), in steps of
    4 pixels so a resize doesn't redraw it at every pixel."""
    if window_h < HERO_HIDE_BELOW * scale:
        return 0
    h = max(HERO_MIN_H * scale, min(HERO_MAX_H * scale, window_h * HERO_SHARE))
    return int(h // 4 * 4)


# ---------------------------------------------------------------------------
# How far each port is (the "Port status" pages). One place to update.
# ---------------------------------------------------------------------------
READY, TESTING, NEXT, LATER = "ready", "testing", "next", "later"
STATE_LABELS = {READY: "Ready", TESTING: "Testing", NEXT: "Next", LATER: "Later"}


@dataclass(frozen=True)
class Milestone:
    what: str
    state: str     # READY, TESTING, NEXT or LATER
    note: str      # one short line


_PC_PORT = (
    Milestone("New copy", READY, "Copies only the game's data archives."),
    Milestone("Network code", READY, "Checked against the game: 177 functions match."),
    Milestone("LAN server", TESTING, "Lists and joins; players spectate. Needs a test with your game."),
    Milestone("Gameplay", NEXT, "Spawning, movement, weapons, game types."),
    Milestone("Rust client", LATER, "Joining servers from the Rust version."),
)
PORT_STATUS: Dict[str, Tuple[Milestone, ...]] = {
    WAW: (
        Milestone("Content ripper", READY, "Fastfiles to open formats."),
        Milestone("Decompile", READY, "PC and Xbox 360, single and multiplayer, with names."),
        Milestone("System notes", NEXT, "Weapons, zombies, scripts, menus."),
        Milestone("Rust port", LATER, "In the new engine, with Call of Duty 3."),
    ),
    COD2: _PC_PORT,
    CODUO: _PC_PORT,
}
# The documents behind each status page (in the project folder).
STATUS_DOCS: Dict[str, Tuple[Tuple[str, str], ...]] = {
    WAW: (("docs/WAW_DECOMP.md", "Decompile notes"),),
    COD2: (("docs/NEW_COPY.md", "New copy"), ("docs/PC_SERVER.md", "LAN server"),
           ("docs/PC_MULTIPLAYER.md", "Cross-play")),
    CODUO: (("docs/NEW_COPY.md", "New copy"), ("docs/PC_SERVER.md", "LAN server"),
            ("docs/PC_MULTIPLAYER.md", "Cross-play")),
}

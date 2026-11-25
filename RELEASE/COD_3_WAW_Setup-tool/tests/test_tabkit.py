"""[acct2-ocr] 2026-11-22 The other tabs in Call of Duty 3's layout (owner 00:26: "the other game's tabs need to be more
akin to cod3's"; he chose all of it) and the sizes (00:13: "the setup tool has sizing issues. Please go over every
tab and sub-tab"): every tab's sidebar ends as Call of Duty 3's, its documents are in the project, and, in a hidden
window when one can be opened, the Game settings sub-tabs, Save / Undo, the rows of buttons that wrap, and the game
tabs that always fit."""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOL))

from c3w import games  # noqa: E402

PAGES = games.available_pages()


class SidebarTests(unittest.TestCase):
    def test_every_tab_ends_as_call_of_duty_3s(self):
        self.assertEqual(PAGES["cod3"][-3:], ["tools", "log", "about"])
        for gid in games.KIT_TABS:
            self.assertEqual(PAGES[gid][-3:], [f"{gid}_tools", "log", f"{gid}_about"], gid)
            self.assertEqual(games.PAGE_LABELS[f"{gid}_tools"], "Tools")
            self.assertEqual(games.PAGE_LABELS[f"{gid}_about"], "About")
            self.assertLessEqual(len(PAGES[gid]), 9, gid)   # Ctrl+1 to Ctrl+9 reach every page

    def test_each_tab_keeps_its_own_pages_first(self):
        # [setup-layout] 2026-11-24 no Port status pages: About has that section
        self.assertEqual(PAGES["bro"][:3], ["bro_setup", "bro_play", "bro_tools"])
        self.assertEqual(PAGES["fh"][:3], ["fh_setup", "fh_play", "fh_tools"])
        self.assertEqual(PAGES["waw"][:3], ["waw_setup", "waw_play", "waw_content"])
        self.assertEqual(PAGES["coduo"][:3], ["coduo_copy", "coduo_server", "coduo_tools"])

    def test_one_sidebar_order_on_every_tab(self):
        # [setup-layout] 2026-11-24 Set up (or the tab's own first pages), Play, the game's own pages, then Tools,
        # Log, About; with Mods & maps switched on too
        for pages in (PAGES, games.available_pages(["maps"])):
            for g in games.GAMES:
                own = [p for p in pages[g.id] if p not in ("log",) and not p.endswith(("tools", "about"))]
                self.assertEqual(pages[g.id], own + list(kit_pages_or_cod3(g.id)), g.id)
                if g.id != games.CODUO:
                    self.assertTrue(own[0].endswith("setup") and own[1].endswith("play"), g.id)
                self.assertFalse([p for p in pages[g.id] if p.endswith("_status")], g.id)


def kit_pages_or_cod3(gid):
    return ("tools", "log", "about") if gid == games.COD3 else (f"{gid}_tools", "log", f"{gid}_about")

    def test_a_shared_page_stays_on_its_tab(self):
        for gid in games.KIT_TABS:
            self.assertEqual(games.owner_of(f"{gid}_tools", PAGES, prefer="cod3"), gid)


class DocumentsTests(unittest.TestCase):
    def test_the_about_pages_documents_are_in_the_project(self):
        try:
            from c3w import gui_tabkit
        except ImportError as e:   # no tkinter here
            self.skipTest(str(e))
        project = TOOL.parent
        if not (project / "docs").is_dir():
            self.skipTest("a copy of the tool without the project around it")
        for gid, docs in gui_tabkit.GAME_DOCS.items():
            for rel, _label, what in docs:
                self.assertTrue((project / rel).is_file(), f"{gid}: {rel}")
                self.assertTrue(what.endswith("."), rel)
        for gid in games.KIT_TABS:
            self.assertIn(gid, gui_tabkit.CREDITS)


class WindowTests(unittest.TestCase):
    """The pieces in a hidden window (withdrawn: never shown, never focused)."""

    def setUp(self):
        try:
            import tkinter as tk
            from c3w import gui_tabkit, theme, widgets
            self.root = tk.Tk()
        except Exception as e:   # no tkinter, or no display to open a window on
            self.skipTest(f"no window can be opened here: {e}")
        self.addCleanup(self.root.destroy)
        self.root.withdraw()
        theme.setup_fonts(self.root)
        theme.apply(self.root, "cod3", "", widgets.px)
        self.tk, self.kit, self.widgets = tk, gui_tabkit, widgets

    def test_sub_tabs_save_and_undo(self):
        from tkinter import ttk
        tk, root = self.tk, self.root
        saved = {"size": "Default", "mute": False}
        size, mute = tk.StringVar(value="Default"), tk.BooleanVar(value=False)

        class Stub(self.kit.TabKit):
            def __init__(self):
                self.root = root

            def _grid_cols(self, frame):
                frame.columnconfigure(1, weight=1)

        app = Stub()
        s = ttk.Frame(root)
        s.pack()

        def save():
            saved.update(size=size.get(), mute=mute.get())

        def undo():
            size.set(saved["size"])
            mute.set(saved["mute"])
        panel = app._kit_settings(
            s, "x_play", [("Display", [("choice", "Window size", size, ["Default", "1920x1080"], "The size.")]),
                          ("Sound", [("check", "Mute", mute, "No sound.")])],
            intro="Intro.", where="Where.", save=save, undo=undo, defaults=lambda: (size.set("Default"),
                                                                                    mute.set(False)),
            dirty=lambda: (size.get(), mute.get()) != (saved["size"], saved["mute"]))
        root.update_idletasks()
        self.assertEqual(panel["titles"], ["Display", "Sound"])
        frames = [w for w in s.winfo_children() if isinstance(w, ttk.Frame)]   # the tab bar, the tabs, the buttons
        buttons = frames[0].winfo_children()
        styles = sorted(str(b.cget("style")) for b in buttons)
        self.assertEqual(styles, ["SubTab.Accent.TButton", "SubTab.Ghost.TButton"])
        panel["show"]("Sound")
        self.assertEqual(sorted(str(b.cget("style")) for b in buttons), styles)   # one chosen, one not
        save_btn = next(w for w in frames[-1].winfo_children() if w.cget("text") == "Save game settings")
        self.assertEqual(str(save_btn.cget("state")), "disabled")      # nothing changed yet
        mute.set(True)
        self.assertEqual(str(save_btn.cget("state")), "normal")
        save_btn.invoke()
        self.assertTrue(saved["mute"])
        self.assertEqual(str(save_btn.cget("state")), "disabled")

    def test_a_long_checkbox_wraps(self):
        from tkinter import ttk
        w = self.widgets
        box = w.autowrap(ttk.Checkbutton(self.root, text="A long checkbox text " * 6), 300)
        style = str(box.cget("style"))
        self.assertTrue(style.startswith("C3WWrap") and style.endswith(".TCheckbutton"), style)
        self.assertEqual(w._wrap_of(box), w.px(300))
        w._set_wrap(box, w.px(200))
        self.assertEqual(w._wrap_of(box), w.px(200))
        other = w.autowrap(ttk.Checkbutton(self.root, text="Another"), 300)
        self.assertNotEqual(str(other.cget("style")), style)   # each its own: one wrap doesn't move the other
        self.assertEqual(w._wrap_of(box), w.px(200))

    def test_a_packed_row_reflows(self):
        from tkinter import ttk
        bar = ttk.Frame(self.root)
        bar.pack()
        self.widgets.button_row(bar, [(f"Button {i}", lambda: None, "") for i in range(4)])
        ttk.Button(bar, text="One more").pack(side="left")
        self.widgets.reflow(bar)
        self.assertEqual({b.winfo_manager() for b in bar.winfo_children()}, {"grid"})
        self.assertEqual({b.grid_info()["row"] for b in bar.winfo_children()}, {0})

    def test_a_row_of_buttons_wraps_and_comes_back(self):
        from tkinter import ttk
        bar = ttk.Frame(self.root)
        bar.pack()
        for i in range(6):
            ttk.Button(bar, text=f"Button {i}", width=0)
        self.widgets.flow(bar, gap=4)
        self.root.update_idletasks()
        one_row = sum(b.winfo_reqwidth() for b in bar.winfo_children())
        self.assertEqual({b.grid_info()["row"] for b in bar.winfo_children()}, {0})
        self.assertTrue(self.widgets._place_flow(bar, one_row // 2))
        rows = {b.grid_info()["row"] for b in bar.winfo_children()}
        self.assertGreater(len(rows), 1)
        self.assertFalse(self.widgets._place_flow(bar, one_row // 2))   # the same room: nothing to do
        self.assertTrue(self.widgets._place_flow(bar, None))
        self.assertEqual({b.grid_info()["row"] for b in bar.winfo_children()}, {0})


if __name__ == "__main__":
    unittest.main()

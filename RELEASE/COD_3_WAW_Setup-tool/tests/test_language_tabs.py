"""[acct2-decomp] 2026-11-21 Tests for the language settings' grouping (owner 10:38: "f2 and setup tool for languages
needs organization/grouping/tabbing"; asked how, he chose tabs in both):

- the setup tool's Game settings are tabs (gui_pages.GS_GROUPS), every language row in Language under its headings,
  with "Add voices from another disc" under Voices;
- F2 has a LANGUAGE tab after SOUND with Text, Voices and Console language, SOUND has none of them, and the F2 tour
  visits the three (the sources are read; nothing is built).

The window test opens a hidden window in a Python of its own (this process stays without tkinter: test_theme).

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import ast
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import game_settings  # noqa: E402

TOOL = Path(__file__).resolve().parent.parent
SRC = TOOL / "recomp" / "src"
TABS = ["Display", "Graphics", "Sound", "Language", "Controls", "Cutscenes", "Player"]
LANGUAGE = ["cod3_voice_language", "cod3_text_language", "cod3_text_subtitles", "cod3_text_menus",
            "cod3_text_prompts", "cod3_text_objectives", "cod3_text_loading", "cod3_text_credits",
            "cod3_text_film_cards", "cod3_font_fill", "user_language"]


def _literal(path: Path, name: str):
    """A module-level constant of a source file, read without importing it (gui_pages imports tkinter)."""
    tree = ast.parse(path.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", None) == name for t in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError(f"{name} not in {path.name}")


def _body(text: str, signature: str) -> str:
    """A C++ function's text, from its signature to the closing brace at the start of a line."""
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


class SetupToolGroups(unittest.TestCase):
    def setUp(self):
        self.groups = _literal(TOOL / "c3w" / "gui_pages.py", "GS_GROUPS")
        self.headings = _literal(TOOL / "c3w" / "gui_pages.py", "GS_HEADINGS")

    def test_the_tabs(self):
        self.assertEqual([title for title, _ in self.groups], TABS)

    def test_every_language_row_is_in_language_and_nowhere_else(self):
        tabs = dict(self.groups)
        self.assertEqual(tabs["Language"], LANGUAGE)
        for title, keys in self.groups:
            if title != "Language":
                self.assertFalse(set(keys) & set(LANGUAGE), title)

    def test_each_setting_once_and_known(self):
        keys = [k for _, group in self.groups for k in group]
        self.assertEqual(len(keys), len(set(keys)))
        known = {st.key for st in game_settings.SETTINGS}
        self.assertFalse(set(keys) - known)

    def test_headings(self):
        self.assertEqual(self.headings, {"cod3_voice_language": "Voices", "cod3_text_language": "Text",
                                         "cod3_text_subtitles": "Text by kind", "user_language": "Console language"})
        self.assertTrue(set(self.headings) <= set(LANGUAGE))


class F2Language(unittest.TestCase):
    def setUp(self):
        self.f2 = (SRC / "ui" / "hud_overlay.cpp").read_text(encoding="utf-8")

    def test_sound_has_no_language_rows(self):
        sound = _body(self.f2, "void SettingsWindow::DrawSound() {")
        for row in ("cod3::voice::DrawSettingsRow", "cod3::text::DrawSettingsRow", '"user_language"'):
            self.assertNotIn(row, sound)

    def test_language_tab_with_its_three_pages(self):
        page = _body(self.f2, "void SettingsWindow::DrawLanguage() {")
        self.assertEqual(re.findall(r'BeginTabItem\("([^"]+)"\)', page), ["Text", "Voices", "Console language"])
        self.assertIn("cod3::text::DrawSettingsRow", page)
        self.assertIn("cod3::voice::DrawSettingsRow", page)
        self.assertIn("DrawConsoleLanguage()", page)
        self.assertIn('CvarCombo("Console language", "user_language"',
                      _body(self.f2, "void SettingsWindow::DrawConsoleLanguage() {"))
        self.assertRegex(self.f2, r'\{"SOUND", &SettingsWindow::DrawSound\},\s*\{"LANGUAGE", &SettingsWindow::'
                                  r'DrawLanguage\}')

    def test_the_tour_visits_them(self):
        tour = (SRC / "ui" / "f2_tour.cpp").read_text(encoding="utf-8")
        pages = re.findall(r'"([A-Z]+(?:/[^"]+)?)"', tour[tour.index("kAllPages[] = {"):tour.index("};", tour.index(
            "kAllPages[] = {"))])
        at = pages.index("SOUND")
        self.assertEqual(pages[at + 1:at + 4], ["LANGUAGE/Text", "LANGUAGE/Voices", "LANGUAGE/Console language"])


class GameSettingsTabsInItsOwnProcessTests(unittest.TestCase):
    """Runs GameSettingsTabsTests (below) in a Python of its own; skipped where no window can be opened."""

    def test_the_tabs_in_a_window(self):
        env = {**os.environ, "C3W_LANGUAGE_TAB_TESTS": "1", "PYTHONDONTWRITEBYTECODE": "1"}
        run = subprocess.run([sys.executable, "-B", "-m", "unittest", "-v", "test_language_tabs.GameSettingsTabsTests"],
                             cwd=str(Path(__file__).resolve().parent), env=env, capture_output=True, text=True,
                             timeout=180)
        said = run.stdout + run.stderr
        if "no window can be opened here" in said:
            self.skipTest("no window can be opened here")
        self.assertEqual(run.returncode, 0, said[-4000:])
        self.assertIn("Ran 1 test", said)
        self.assertNotIn("skipped", said)


class GameSettingsTabsTests(unittest.TestCase):
    """The Game settings section itself, in a hidden window. Run by GameSettingsTabsInItsOwnProcessTests."""

    def setUp(self):
        if os.environ.get("C3W_LANGUAGE_TAB_TESTS") != "1":
            self.skipTest("run by GameSettingsTabsInItsOwnProcessTests, in a Python of its own")
        try:
            import tkinter as tk
            from c3w import gui_pages
            self.root = tk.Tk()
        except Exception as e:   # no tkinter, or no display to open a window on
            self.skipTest(f"no window can be opened here: {e}")
        self.addCleanup(self.root.destroy)
        self.root.withdraw()
        self.gui_pages = gui_pages

    def test_tabs_language_and_the_voices_button(self):
        from tkinter import ttk
        test = self

        class Stub(self.gui_pages.Pages):   # what the section uses of gui.App
            def __init__(self):
                self.root = test.root
                self.added = 0

            def _grid_cols(self, frame):
                frame.columnconfigure(1, weight=1)

            def _update_game_settings_state(self, error=""):
                pass

            def _add_voices(self):
                self.added += 1

        app = Stub()
        frame = ttk.Frame(self.root)
        frame.pack()
        app._game_settings_section(frame)
        self.root.update_idletasks()
        titles = list(app._gs_tab_buttons)
        self.assertEqual(titles[:len(TABS)], TABS)
        self.assertTrue(set(titles[len(TABS):]) <= {"More"})
        lang = app._gs_tab_frames["Language"]
        for key in LANGUAGE:
            self.assertIs(app._gs_vars[key][2].master, lang, key)
        buttons = [w for w in lang.winfo_children() if isinstance(w, ttk.Button)]
        add = [b for b in buttons if b.cget("text") == "Add voices from another disc..."]
        self.assertEqual(len(add), 1)
        voice_row = int(app._gs_vars["cod3_voice_language"][2].grid_info()["row"])
        self.assertGreater(int(add[0].grid_info()["row"]), voice_row)
        add[0].invoke()
        self.assertEqual(app.added, 1)
        labels = [w.cget("text") for w in lang.winfo_children() if isinstance(w, ttk.Label)]
        for heading in ("Voices", "Text", "Text by kind", "Console language"):
            self.assertIn(heading, labels)
        # One tab at a time: the first, then the one chosen.
        self.assertEqual(app._gs_tab_frames["Display"].winfo_manager(), "grid")
        self.assertEqual(lang.winfo_manager(), "")
        app._gs_show_tab("Language")
        self.assertEqual(lang.winfo_manager(), "grid")
        self.assertEqual(app._gs_tab_frames["Display"].winfo_manager(), "")
        # [acct2-ocr] 2026-11-22 the sub-tab styles: one font and padding chosen or not (theme.py)
        self.assertEqual(str(app._gs_tab_buttons["Language"].cget("style")), "SubTab.Accent.TButton")
        self.assertEqual(str(app._gs_tab_buttons["Display"].cget("style")), "SubTab.Ghost.TButton")


if __name__ == "__main__":
    unittest.main()

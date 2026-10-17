"""The Play page and the game share one settings file (cod3.toml), and the
last change made in either place is the one the next start uses
(owner 2026-10-16: "The toggle in-game for the waifu textures doesn't change
from the launchers settings (please check every settings, so we won't have
this issue)").

- Saving here writes only the settings changed here: a setting changed in the
  game since the page read the file keeps the game's value.
- Reading the file again keeps what was changed here and not saved yet, and
  shows the game's changes for everything else.
- "Start in a window" is the game's own Display mode: no --fullscreen=false
  at every start (it undid F2's Display mode); an older settings file's tick
  becomes the game's setting once.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import queue
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOL = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOL))

from c3w import game_settings as gs  # noqa: E402

# The page logic tested here draws nothing, so Tk is a stand-in: the tests run
# on a Python without Tk too, and Tk is out of sys.modules again afterwards
# (test_theme checks that the colors code never imports it).
_TK_NAMES = ("tkinter", "tkinter.ttk", "tkinter.font", "tkinter.filedialog", "tkinter.colorchooser")
_tk_before = {name: sys.modules[name] for name in _TK_NAMES if name in sys.modules}
_modules_before = set(sys.modules)
_tk_stub = mock.MagicMock()
for _name in _TK_NAMES:
    sys.modules[_name] = _tk_stub
try:
    from c3w import gui_pages  # noqa: E402
finally:
    # The GUI modules imported with the stand-in are forgotten too, so a later
    # import gets them with the real Tk.
    for _name in set(sys.modules) - _modules_before:
        sys.modules.pop(_name, None)
    sys.modules.update(_tk_before)


class _Var:
    """Stands in for a tkinter variable."""

    def __init__(self, value=None):
        self.value = value

    def get(self):
        return self.value

    def set(self, value):
        self.value = value


class _Log:
    def __init__(self):
        self.lines = []

    def info(self, msg):
        self.lines.append(("info", msg))

    def warn(self, msg):
        self.lines.append(("warn", msg))


KEYS = ("cod3_hd_textures", "cod3_max_fps", "fullscreen", "cod3_skip_movies")


def _page(out: Path) -> gui_pages.Pages:
    """A Play page without a window: the game settings form for KEYS."""
    p = gui_pages.Pages.__new__(gui_pages.Pages)
    p.v_out = _Var(str(out))
    p.v_status = _Var("")
    p.v_cod3_args = _Var("")
    p.v_cod3_windowed = _Var(False)
    p.log = _Log()
    p.q = queue.Queue()
    p.infos = []
    p._info = lambda title, text, kind="info": p.infos.append((title, text, kind))
    p._update_game_settings_state = lambda error="": None
    p._gs_vars = {}
    for key in KEYS:
        st = gs.BY_KEY[key]
        var = _Var(bool(st.default) if st.kind == "bool" else p._gs_label(st, st.default))
        p._gs_vars[key] = (st, var, mock.Mock())
    p._gs_loaded = {}
    p._gs_loaded_for = None
    p._gs_running = False
    p._gs_loading = p._gs_watching = False
    p._gs_mtime = None
    p._gs_launched = None
    return p


def _load(p, keep_edits=False):
    """_load_game_settings, waiting for its worker thread."""
    p._load_game_settings(keep_edits=keep_edits)
    kind, fn = p.q.get(timeout=10)
    assert kind == "call"
    fn()


def _set(p, key, value):
    st, var, _ = p._gs_vars[key]
    var.set(value if st.kind == "bool" else p._gs_label(st, value))


class LastChangeWinsTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.out = Path(self._tmp.name)
        self.file = self.out / gs.FILE_NAME
        self.file.write_text("cod3_max_fps = 144\ncod3_hd_textures = true\n", encoding="utf-8")
        patcher = mock.patch.object(gs, "game_running", return_value=False)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        self._tmp.cleanup()

    def test_saving_here_keeps_a_change_made_in_the_game(self):
        p = _page(self.out)
        _load(p)
        self.assertIs(p._gs_values()["cod3_hd_textures"], True)
        # The game turns HD textures off (F2 saves cod3.toml) while the page shows "on".
        self.file.write_text("cod3_max_fps = 144\ncod3_hd_textures = false\n", encoding="utf-8")
        # Here only the frame rate limit is changed, then saved (or Play pressed).
        _set(p, "cod3_max_fps", 60)
        self.assertTrue(p._save_game_settings(quiet=True))
        values = gs.read_values(self.out)
        self.assertEqual(values["cod3_max_fps"], 60)
        self.assertIs(values["cod3_hd_textures"], False)   # the game's change stays

    def test_reading_again_keeps_unsaved_edits_and_shows_the_games_changes(self):
        p = _page(self.out)
        _load(p)
        _set(p, "cod3_skip_movies", True)   # changed here, not saved
        self.file.write_text("cod3_max_fps = 30\ncod3_hd_textures = false\n", encoding="utf-8")
        _load(p, keep_edits=True)
        v = p._gs_values()
        self.assertIs(v["cod3_hd_textures"], False)   # the game's change shows
        self.assertEqual(v["cod3_max_fps"], 30)
        self.assertIs(v["cod3_skip_movies"], True)    # the edit here stays
        self.assertEqual(set(p._gs_edits()), {"cod3_skip_movies"})

    def test_nothing_changed_writes_nothing(self):
        p = _page(self.out)
        _load(p)
        before = self.file.read_bytes()
        self.file.write_text("cod3_hd_textures = false\n", encoding="utf-8")   # the game, since
        self.assertTrue(p._save_game_settings(quiet=True))
        self.assertEqual(self.file.read_text(encoding="utf-8"), "cod3_hd_textures = false\n")
        self.assertNotEqual(before, self.file.read_bytes())

    def test_the_watch_reads_the_file_again_when_it_changes(self):
        p = _page(self.out)
        _load(p)
        p.page = "play"
        p.root = mock.Mock()
        p._out_path = lambda: self.out
        self.file.write_text("cod3_hd_textures = false\n", encoding="utf-8")
        import os
        os.utime(self.file, ns=(p._gs_mtime + 10**9, p._gs_mtime + 10**9))
        p._gs_watch()
        kind, fn = p.q.get(timeout=10)   # the watch's check
        fn()
        kind, fn = p.q.get(timeout=10)   # the reload it asked for
        fn()
        self.assertIs(p._gs_values()["cod3_hd_textures"], False)

    def test_saving_refuses_while_the_game_runs(self):
        p = _page(self.out)
        _load(p)
        _set(p, "cod3_max_fps", 60)
        with mock.patch.object(gs, "game_running", return_value=True):
            self.assertFalse(p._save_game_settings(quiet=True))
        self.assertEqual(gs.read_values(self.out)["cod3_max_fps"], 144)


class WindowedTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.out = Path(self._tmp.name)
        patcher = mock.patch.object(gs, "game_running", return_value=False)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        self._tmp.cleanup()

    def test_old_tick_becomes_the_games_display_mode_once(self):
        (self.out / gs.FILE_NAME).write_text("cod3_max_fps = 144\n", encoding="utf-8")
        p = _page(self.out)
        p.v_cod3_windowed.set(True)
        p._migrate_windowed(self.out)
        self.assertIs(gs.read_values(self.out)["fullscreen"], False)
        self.assertFalse(p.v_cod3_windowed.get())

    def test_a_display_mode_already_chosen_stays(self):
        (self.out / gs.FILE_NAME).write_text("fullscreen = true\n", encoding="utf-8")
        p = _page(self.out)
        p.v_cod3_windowed.set(True)
        p._migrate_windowed(self.out)
        self.assertIs(gs.read_values(self.out)["fullscreen"], True)
        self.assertFalse(p.v_cod3_windowed.get())

    def test_play_sends_no_fullscreen_flag(self):
        (self.out / gs.FILE_NAME).write_text("fullscreen = true\n", encoding="utf-8")
        exe = self.out / "cod3"
        exe.write_bytes(b"")
        exe.chmod(0o755)
        p = _page(self.out)
        p._play_exe = lambda: (exe, "")
        p._gs_dirty = lambda: False
        p._save = lambda: None
        with mock.patch.object(gui_pages.subprocess, "Popen") as popen:
            p._play_cod3()
        args = popen.call_args[0][0]
        self.assertEqual(args, [str(exe)])
        self.assertIsNotNone(p._gs_launched)

    def test_play_refuses_a_second_start(self):
        exe = self.out / "cod3"
        exe.write_bytes(b"")
        p = _page(self.out)
        p._play_exe = lambda: (exe, "")
        with mock.patch.object(gs, "game_running", return_value=True), \
                mock.patch.object(gui_pages.subprocess, "Popen") as popen:
            p._play_cod3()
        popen.assert_not_called()
        self.assertEqual(p.infos[0][0], "Already running")

    def test_the_box_is_gone(self):
        src = (TOOL / "c3w" / "gui_pages.py").read_text(encoding="utf-8")
        self.assertNotIn('text="Start in a window"', src)
        self.assertNotIn('"--fullscreen=false"', src)


if __name__ == "__main__":
    unittest.main()

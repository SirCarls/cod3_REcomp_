"""Tests for the game tabs (c3w/games.py: tab state, pages, port status),
the per-game colors (c3w/theme.py), and the Call of Duty 2 / United
Offensive tools (c3w/pc_games.py: input checks, newcopy and iw-server
command lines, finding installs, the server process). No Tkinter needed.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import deps, fonts, games, pc_games, profiles, theme, waw  # noqa: E402
from c3w.common import Log, SetupError  # noqa: E402

PAGES = games.available_pages()
ALL_IDS = list(theme.ALL_PALETTES)


class GameTableTests(unittest.TestCase):
    def test_four_tabs_in_order(self):
        self.assertEqual(games.GAME_ORDER, ["cod3", "waw", "cod2", "coduo"])
        self.assertEqual([g.tab for g in games.GAMES],
                         ["Call of Duty 3", "World at War", "Call of Duty 2", "United Offensive"])

    def test_every_page_has_a_label_and_the_log_is_everywhere(self):
        for g in games.GAMES:
            for page in g.pages:
                self.assertIn(page, games.PAGE_LABELS)
            self.assertIn("log", g.pages)
        # Call of Duty 3's sidebar is the one the tool had before the tabs.
        self.assertEqual(PAGES["cod3"], ["setup", "play", "textures", "tools", "log", "about"])

    def test_mods_and_maps_only_when_switched_on(self):
        self.assertNotIn("maps", PAGES["waw"])
        self.assertIn("maps", games.available_pages(["maps"])["waw"])
        self.assertNotIn("maps", games.available_pages(["maps"])["cod3"])

    def test_owner_of_a_page(self):
        self.assertEqual(games.owner_of("play", PAGES), "cod3")
        self.assertEqual(games.owner_of("cod2_server", PAGES), "cod2")
        self.assertEqual(games.owner_of("log", PAGES, prefer="coduo"), "coduo")   # shared: stays on the tab
        self.assertEqual(games.owner_of("log", PAGES), "cod3")
        self.assertIsNone(games.owner_of("nope", PAGES))

    def test_unknown_game_is_the_default(self):
        self.assertEqual(games.game("nope").id, games.DEFAULT_GAME)

    def test_port_status_rows(self):
        for gid in ("waw", "cod2", "coduo"):
            rows = games.PORT_STATUS[gid]
            self.assertTrue(rows)
            for m in rows:
                self.assertIn(m.state, games.STATE_LABELS)
                self.assertLessEqual(len(m.note), 80, "keep status notes short")


class TabStateTests(unittest.TestCase):
    def state(self, s, **kw):
        return games.TabState.from_settings(s, PAGES, ALL_IDS, **kw)

    def test_defaults(self):
        st = self.state({})
        self.assertEqual(st.game, "cod3")
        self.assertEqual(st.page_for("cod3"), "setup")
        self.assertEqual(st.page_for("cod2"), "cod2_copy")
        self.assertEqual(st.theme_for("waw"), "waw")   # each tab's own colors
        self.assertEqual(st.theme_for("cod3"), "cod3")
        self.assertEqual(st.accent_for("cod2"), "")

    def test_round_trip(self):
        st = self.state({})
        st.show("coduo", "coduo_server")
        st.show("waw", "waw_status")
        st.themes["cod2"] = "contrast"
        st.accents["waw"] = "#123456"
        again = self.state(st.to_settings())
        self.assertEqual(again.game, "waw")
        self.assertEqual(again.page_for("waw"), "waw_status")
        self.assertEqual(again.page_for("coduo"), "coduo_server")
        self.assertEqual(again.theme_for("cod2"), "contrast")
        self.assertEqual(again.accent_for("waw"), "#123456")

    def test_opens_on_the_last_tab_used(self):
        st = self.state({"ui_game": "cod2", "ui_pages": {"cod2": "cod2_status"}})
        self.assertEqual((st.game, st.page_for(st.game)), ("cod2", "cod2_status"))

    def test_settings_from_before_the_tabs(self):
        st = self.state({"ui_page": "play", "ui_theme": "parchment", "ui_accent": "#C49854"})
        self.assertEqual(st.game, "cod3")
        self.assertEqual(st.page_for("cod3"), "play")
        self.assertEqual(st.theme_for("cod3"), "parchment")
        self.assertEqual(st.accent_for("cod3"), "#c49854")
        # Mods & maps moved to World at War's tab.
        moved = games.TabState.from_settings({"ui_page": "maps"}, games.available_pages(["maps"]), ALL_IDS)
        self.assertEqual(moved.page_for("waw"), "maps")
        # The old keys are still written, for an older tool reading the file.
        out = st.to_settings()
        self.assertEqual((out["ui_page"], out["ui_theme"], out["ui_accent"]), ("play", "parchment", "#c49854"))

    def test_bad_values_fall_back(self):
        st = self.state({"ui_game": "halo", "ui_pages": {"cod3": "nope", "cod2": 7, "x": "setup"},
                         "ui_themes": {"cod2": "neon", "waw": 5}, "ui_accents": {"cod2": "blue", "waw": "#12"}})
        self.assertEqual(st.game, "cod3")
        self.assertEqual(st.page_for("cod3"), "setup")
        self.assertEqual(st.page_for("cod2"), "cod2_copy")
        self.assertEqual(st.theme_for("cod2"), "cod2")
        self.assertEqual(st.accent_for("cod2"), "")
        self.assertEqual(st.accent_for("waw"), "")
        for junk in (None, [], "x", {"ui_pages": "x", "ui_themes": [], "ui_accents": 3}):
            self.assertEqual(self.state(junk if isinstance(junk, dict) else {}).game, "cod3")

    def test_own_colors_are_not_stored(self):
        st = self.state({"ui_themes": {"waw": "waw", "cod2": "olive"}, "ui_accents": {"cod2": ""}})
        self.assertNotIn("waw", st.themes)
        self.assertEqual(st.themes["cod2"], "olive")
        self.assertNotIn("cod2", st.accents)

    def test_launcher_opens_on_play(self):
        st = self.state({"ui_game": "cod2"}, launcher=True)
        self.assertEqual((st.game, st.page_for("cod3")), ("cod3", "play"))

    def test_show_and_step(self):
        st = self.state({})
        self.assertEqual(st.show("cod2"), ("cod2", "cod2_copy"))
        self.assertEqual(st.show("cod2", "cod2_server"), ("cod2", "cod2_server"))
        self.assertEqual(st.show("cod2", "play"), ("cod2", "cod2_server"))   # not this tab's page
        self.assertEqual(st.show("nope"), ("cod2", "cod2_server"))
        self.assertEqual(st.step(1), "coduo")
        st.show("coduo")
        self.assertEqual(st.step(1), "cod3")    # wraps around
        st.show("cod3")
        self.assertEqual(st.step(-1), "coduo")


class GamePaletteTests(unittest.TestCase):
    PAIRS = [("text", "bg"), ("text", "sidebar"), ("text", "field"), ("text", "button"), ("text", "well"),
             ("text", "surface"), ("text_dim", "bg"), ("text_dim", "sidebar"), ("accent_text", "bg"),
             ("accent_text", "sidebar"), ("sidebar_sel_text", "sidebar_sel"), ("good", "bg"), ("warn", "bg"),
             ("bad", "bg"), ("tab_text", "tabbar"), ("tab_text", "tabbar_hover"), ("tab_sel_text", "tabbar_sel"),
             ("chip_good", "chip_good_bg"), ("chip_warn", "chip_warn_bg"), ("chip_info", "chip_info_bg"),
             ("chip_dim", "chip_dim_bg")]

    def test_every_game_has_its_own_palette(self):
        for g in games.GAMES:
            self.assertIn(g.palette, theme.ALL_PALETTES)
            self.assertIn(g.id, theme.DISPLAY_FONTS)
        self.assertEqual(len({theme.colors(g.palette)["accent"] for g in games.GAMES}), 4)

    def test_readable_in_every_palette_and_accent(self):
        for pid in theme.ALL_PALETTES:
            target = theme.HIGH_CONTRAST if pid == "contrast" else theme.BODY_CONTRAST
            for accent in ("", "#0078d7", "#777777", "#fafafa", "#101010"):
                c = theme.colors(pid, accent)
                for fg, bg in self.PAIRS:
                    self.assertGreaterEqual(theme.contrast(c[fg], c[bg]), target - 1e-9, f"{pid} {accent}: {fg}/{bg}")
                self.assertGreaterEqual(theme.contrast(c["accent_fg"], c["accent"]), theme.BODY_CONTRAST)

    def test_theme_choices_per_tab(self):
        self.assertEqual(theme.palettes_for("cod3"), theme.THEME_ORDER)
        self.assertEqual(theme.palettes_for("waw"), ["waw"] + theme.THEME_ORDER)
        self.assertEqual(theme.theme_id_for_label(theme.theme_label("coduo")), "coduo")
        self.assertEqual(theme.theme_label("cod2"), "Call of Duty 2")

    def test_display_font_choice(self):
        self.assertEqual(theme.pick_display_font("waw", ["Impact", "Arial"], "UI"), ("Impact", "normal"))
        self.assertEqual(theme.pick_display_font("cod2", ["dejavu serif"], "UI"), ("dejavu serif", "bold"))
        self.assertEqual(theme.pick_display_font("cod2", [], "UI"), ("UI", "bold"))
        self.assertEqual(theme.pick_display_font("nope", ["Impact"], "UI"), ("UI", "bold"))


class CheckTests(unittest.TestCase):
    def test_map_names(self):
        for ok in ("mp_toujane", "mp_uo_stanjel", "MP-Custom_2"):
            self.assertEqual(pc_games.check_map(ok), "", ok)
        for bad in ("", "  ", "mp toujane", "../maps", "mp_x;quit", "x" * 65, 'a"b'):
            self.assertNotEqual(pc_games.check_map(bad), "", bad)

    def test_ports(self):
        for ok in ("28960", " 1024 ", 65535, "28961"):
            self.assertEqual(pc_games.check_port(ok), "", ok)
        for bad in ("", "80", "1023", "65536", "-1", "28960x", "2.5", None):
            self.assertNotEqual(pc_games.check_port(bad), "", bad)

    def test_player_counts(self):
        self.assertEqual(pc_games.check_max_clients("1"), "")
        self.assertEqual(pc_games.check_max_clients("64"), "")
        for bad in ("0", "65", "", "many"):
            self.assertNotEqual(pc_games.check_max_clients(bad), "", bad)

    def test_host_names(self):
        for ok in ("Rust LAN server", "^1Red ^7server", "My server (EU) #2"):
            self.assertEqual(pc_games.check_hostname(ok), "", ok)
        for bad in ("", "   ", 'say "hi"', "a\\b", "a;quit", "tab\there", "x" * 49, "café"):
            self.assertNotEqual(pc_games.check_hostname(bad), "", bad)

    def test_server_settings(self):
        self.assertEqual(pc_games.ServerSettings("mp_toujane").problems(), [])
        self.assertEqual(len(pc_games.ServerSettings("", "1", "", "0").problems()), 4)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name) / "with space"
        self.cod2 = self.root / "steamapps" / "common" / "Call of Duty 2"
        (self.cod2 / "main").mkdir(parents=True)
        (self.cod2 / "main" / "iw_00.iwd").write_bytes(b"")
        self.uo = self.root / "CB" / "Call of Duty"
        (self.uo / "UO").mkdir(parents=True)                 # case doesn't matter
        (self.uo / "Main").mkdir()
        (self.uo / "UO" / "PAKUO00.PK3").write_bytes(b"")
        (self.uo / "Main" / "pak0.pk3").write_bytes(b"")

    def tearDown(self):
        self._tmp.cleanup()

    def test_recognize_by_names_only(self):
        self.assertEqual(pc_games.recognize(self.cod2, pc_games.COD2), (True, "Call of Duty 2 found (Steam)."))
        self.assertEqual(pc_games.recognize(self.uo, pc_games.CODUO), (True, "United Offensive found (folder)."))
        self.assertFalse(pc_games.recognize(self.uo, pc_games.COD2)[0])
        self.assertFalse(pc_games.recognize(self.root / "nope", pc_games.COD2)[0])
        # United Offensive needs Call of Duty's own archives too.
        (self.uo / "Main" / "pak0.pk3").unlink()
        self.assertFalse(pc_games.recognize(self.uo, pc_games.CODUO)[0])

    def test_check_install(self):
        self.assertEqual(pc_games.check_install("", pc_games.COD2), "")       # empty: Steam
        self.assertEqual(pc_games.check_install(str(self.cod2), pc_games.COD2), "")
        self.assertIn("full path", pc_games.check_install("relative/dir", pc_games.COD2))
        self.assertIn("isn't there", pc_games.check_install(str(self.root / "gone"), pc_games.COD2))
        self.assertIn("Not a", pc_games.check_install(str(self.root), pc_games.COD2))

    def test_copy_folder(self):
        install = str(self.cod2)
        self.assertEqual(pc_games.check_copy_folder(str(self.root / "copies"), install), "")
        self.assertNotEqual(pc_games.check_copy_folder(str(self.cod2 / "copies"), install), "")   # inside the game
        self.assertNotEqual(pc_games.check_copy_folder(str(self.root), install), "")             # holds the game
        self.assertNotEqual(pc_games.check_copy_folder("", install), "")
        self.assertNotEqual(pc_games.check_copy_folder("copies", install), "")
        (self.root / "file").write_text("x")
        self.assertNotEqual(pc_games.check_copy_folder(str(self.root / "file"), ""), "")

    def test_find_in_steam(self):
        with mock.patch.object(waw, "steam_libraries", return_value=[self.root, self.root / "missing"]):
            self.assertEqual(pc_games.find_steam(pc_games.COD2), [self.cod2])
            self.assertEqual(pc_games.find_steam(pc_games.CODUO), [])

    def test_steam_libraries_listed_once(self):
        steam = self.root / "Steam"
        (steam / "steamapps").mkdir(parents=True)
        (steam / "steamapps" / "libraryfolders.vdf").write_text(
            '"libraryfolders"\n{\n "0" { "path" "%s" }\n "1" { "path" "%s" }\n}\n' % (steam, self.root / "lib2"))
        with mock.patch.object(waw, "_steam_roots", return_value=[steam, steam]):
            self.assertEqual(waw.steam_libraries(), [steam, self.root / "lib2"])


class CommandLineTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        base = Path(self._tmp.name)
        self.install = base / "Call of Duty 2"
        (self.install / "main").mkdir(parents=True)
        (self.install / "main" / "iw_00.iwd").write_bytes(b"")
        self.out = base / "my copies"
        self.exe = base / "tools" / "newcopy"

    def tearDown(self):
        self._tmp.cleanup()

    def test_make(self):
        cmd = pc_games.newcopy_make(self.exe, pc_games.COD2, str(self.out), str(self.install))
        self.assertEqual(cmd, [str(self.exe), "make", "cod2", "--out", str(self.out), "--from", str(self.install)])
        cmd = pc_games.newcopy_make(self.exe, pc_games.CODUO, str(self.out), "", full_copy=True, language="german")
        self.assertEqual(cmd, [str(self.exe), "make", "coduo", "--out", str(self.out), "--copy",
                               "--language", "german"])

    def test_make_refuses_bad_values(self):
        with self.assertRaises(SetupError):
            pc_games.newcopy_make(self.exe, pc_games.COD2, str(self.install / "out"), str(self.install))
        with self.assertRaises(SetupError):
            pc_games.newcopy_make(self.exe, pc_games.COD2, "relative", "")
        with self.assertRaises(SetupError):
            pc_games.newcopy_make(self.exe, pc_games.COD2, str(self.out), "", language="klingon")
        with self.assertRaises(SetupError):   # not a Call of Duty 2 folder
            pc_games.newcopy_make(self.exe, pc_games.COD2, str(self.out), str(self.out.parent))

    def test_find_and_check(self):
        self.assertEqual(pc_games.newcopy_find(self.exe), [str(self.exe), "find"])
        self.assertEqual(pc_games.newcopy_find(self.exe, f" {self.install} "),
                         [str(self.exe), "find", "--from", str(self.install)])
        self.assertEqual(pc_games.newcopy_check(self.exe, str(self.out / "cod2")),
                         [str(self.exe), "check", str(self.out / "cod2")])
        with self.assertRaises(SetupError):
            pc_games.newcopy_check(self.exe, "")

    def test_server(self):
        settings = pc_games.ServerSettings("mp_toujane", " 28961 ", "Rust \"LAN\"", "8")
        with self.assertRaises(SetupError):     # quotes aren't allowed in the name
            pc_games.server_command(self.exe, pc_games.COD2, settings)
        settings.hostname = "Rust LAN server"
        self.assertEqual(pc_games.server_command(Path("iw-server"), pc_games.COD2, settings),
                         ["iw-server", "--game", "cod2", "--map", "mp_toujane", "--port", "28961",
                          "--hostname", "Rust LAN server", "--max-clients", "8"])
        uo = pc_games.server_command(Path("iw-server"), pc_games.CODUO, pc_games.ServerSettings("mp_arnhem"))
        self.assertEqual(uo[1:3], ["--game", "uo"])
        self.assertEqual(uo[uo.index("--port") + 1], "28960")

    def test_server_env(self):
        env = pc_games.server_env({"PATH": "x", "IW_PC_INPUT": "old"}, str(self.install))
        self.assertEqual(env, {"PATH": "x", "IW_PC_INPUT": str(self.install)})
        self.assertEqual(pc_games.server_env({"IW_PC_INPUT": "old"}, " "), {})   # empty: it looks in Steam

    def test_default_copy_dir(self):
        self.assertEqual(pc_games.default_copy_dir(str(self.out)), str(self.out / "copies"))
        self.assertEqual(pc_games.default_copy_dir(""), "")


FIND_OUTPUT = """Steam:
  /games/Steam  (found by $HOME/.steam/steam)

Call of Duty 2 (cod2)
  version   1.3 (the engine targets 1.3)
  language  english
  source    Steam (app 2630)
  folder    /games/Steam/steamapps/common/Call of Duty 2
  archives  40 found, 1.2 GB, none missing
  programs  CoD2MP_s.exe (recognized by name only; never read or copied)

United Offensive (coduo)
  version   1.51 (the engine targets 1.51)
  language  english, german
  source    folder
  folder    /cb/Call of Duty
  archives  20 found, 900.0 MB, 2 missing
  missing   uo/uomappack00.pk3, main/pakb.pk3
  hint      United Offensive uses Call of Duty's archives in main/

Proton builds (to run the original Windows games on Linux, for example a CB copy
  GE-Proton9-1  custom  /x
"""


class OutputTests(unittest.TestCase):
    def test_parse_find(self):
        found = pc_games.parse_find(FIND_OUTPUT)
        self.assertEqual([f.game for f in found], ["cod2", "coduo"])
        cod2, uo = found
        self.assertEqual((cod2.name, cod2.version, cod2.source), ("Call of Duty 2", "1.3", "Steam (app 2630)"))
        self.assertEqual(cod2.folder, "/games/Steam/steamapps/common/Call of Duty 2")
        self.assertEqual(cod2.missing, "")
        self.assertEqual(uo.missing, "uo/uomappack00.pk3, main/pakb.pk3")
        self.assertEqual(uo.language, "english, german")
        self.assertEqual(len(uo.notes), 1)
        self.assertEqual(pc_games.parse_find(""), [])

    def test_copy_progress(self):
        seen = []
        log = Log(sinks=[lambda k, t, f: seen.append((k, t, f))])
        handle = pc_games.copy_progress(log)
        handle("  [ 3/40] linked    main/iw_00.iwd (170.2 MB)")
        handle("Checking 40 archives are whole ZIP files...")
        self.assertEqual(seen, [("progress", "linked    main/iw_00.iwd (170.2 MB)", 3 / 40)])


class ProfileTests(unittest.TestCase):
    def test_new_settings_are_kept_and_checked(self):
        notes = []
        clean = profiles.clean_settings({"cod2_install": "/x", "cod2_port": 28960, "coduo_max_clients": 8,
                                         "coduo_language": "german", "cod2_full_copy": True,
                                         "cod2_map": "mp_toujane", "coduo_hostname": "LAN"}, notes)
        self.assertEqual(len(clean), 7, notes)
        bad = profiles.clean_settings({"cod2_port": 80, "coduo_max_clients": 100, "cod2_language": "klingon",
                                       "cod2_port_x": 1, "cod2_full_copy": "yes"})
        self.assertEqual(bad, {})
        for key in ("cod2_install", "cod2_copy_dir", "coduo_install", "coduo_copy_dir"):
            self.assertIn(key, profiles.PATH_KEYS)


class ServerProcessTests(unittest.TestCase):
    def test_start_output_stop(self):
        lines, codes = [], []
        done = threading.Event()
        server = pc_games.ServerProcess(lines.append, lambda c: (codes.append(c), done.set()))
        script = "import time\nprint('server on UDP port 28960', flush=True)\nwhile True:\n    time.sleep(0.1)\n"
        server.start([sys.executable, "-c", script], env=dict(os.environ))
        deadline = time.time() + 20
        while not lines and time.time() < deadline:
            time.sleep(0.05)
        self.assertTrue(server.running)
        self.assertEqual(lines, ["server on UDP port 28960"])
        with self.assertRaises(SetupError):
            server.start([sys.executable, "-c", "pass"])     # one at a time
        server.stop()
        self.assertTrue(done.wait(20))
        self.assertFalse(server.running)
        self.assertTrue(server.stopping)

    def test_exit_by_itself(self):
        codes = []
        done = threading.Event()
        server = pc_games.ServerProcess(lambda line: None, lambda c: (codes.append(c), done.set()))
        server.start([sys.executable, "-c", "import sys; sys.exit(3)"])
        self.assertTrue(done.wait(20))
        self.assertEqual(codes, [3])
        self.assertFalse(server.stopping)

    def test_missing_program(self):
        server = pc_games.ServerProcess(lambda line: None, lambda c: None)
        with self.assertRaises(SetupError):
            server.start([str(Path(tempfile.gettempdir()) / "no-such-iw-server")])


class ToolTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self.tool_root = self.base / "COD_3_WAW_Setup-tool"
        self.tool_root.mkdir()
        self.output = self.base / "output"
        self.log = Log()

    def tearDown(self):
        self._tmp.cleanup()

    def test_no_source_and_nothing_built(self):
        self.assertFalse(pc_games.engine_has("newcopy", self.tool_root))
        with self.assertRaises(SetupError):
            pc_games.ensure_tool("newcopy", self.tool_root, self.output, self.base / "work", 1, None, self.log,
                                 threading.Event(), False)

    def test_copy_made_earlier_is_used_without_the_source(self):
        exe = self.output / "tools" / ("newcopy.exe" if os.name == "nt" else "newcopy")
        exe.parent.mkdir(parents=True)
        exe.write_bytes(b"")
        self.assertEqual(pc_games.ensure_tool("newcopy", self.tool_root, self.output, self.base / "work", 1, None,
                                              self.log, threading.Event(), False), exe)

    def test_package_names(self):
        self.assertEqual(pc_games.TOOLS["newcopy"][0], "newcopy")
        self.assertEqual(pc_games.TOOLS["iw-server"][0], "iw_server")    # the binary is iw-server
        (self.base / "engine" / "tools" / "iw_server").mkdir(parents=True)
        (self.base / "engine" / "tools" / "iw_server" / "Cargo.toml").write_text("")
        self.assertTrue(pc_games.engine_has("iw-server", self.tool_root))


class AwkwardPathTests(unittest.TestCase):
    """Paths with spaces, quotes and shell characters go to the programs as
    one argument each, whatever the system (no shell is ever used)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        base = Path(self._tmp.name) / "Bob's CB copies (v1.3) & more" / "a=b;c $HOME"
        self.install = base / "Call of Duty 2"
        (self.install / "main").mkdir(parents=True)
        (self.install / "main" / "iw_00.iwd").write_bytes(b"")
        (self.install / "CoD2MP_s.EXE").write_bytes(b"")          # any case
        self.out = Path(self._tmp.name) / "My Copies" / "it's \"here\""
        self.exe = Path(self._tmp.name) / "tools dir" / "newcopy"

    def tearDown(self):
        self._tmp.cleanup()

    def test_each_path_is_one_argument(self):
        cmd = pc_games.newcopy_make(self.exe, pc_games.COD2, str(self.out), str(self.install), language="french")
        self.assertEqual(cmd[cmd.index("--out") + 1], str(self.out))
        self.assertEqual(cmd[cmd.index("--from") + 1], str(self.install))
        self.assertEqual(pc_games.newcopy_find(self.exe, str(self.install))[-1], str(self.install))
        game_exe = pc_games.game_executable(str(self.install), pc_games.COD2)
        self.assertEqual(game_exe, self.install / "CoD2MP_s.EXE")
        cmd = pc_games.server_command(Path("iw-server"), pc_games.COD2, pc_games.ServerSettings("mp_toujane"),
                                      game_exe)
        self.assertEqual(cmd[-2:], ["--exe", str(game_exe)])

    def test_the_game_program_is_given_by_path(self):
        """iw-server gets the game's program with --exe, so IW_PC_INPUT (a
        list of folders) isn't needed: a folder whose name has the list
        separator (';' on Windows, ':' elsewhere) would be split there."""
        game_exe = pc_games.game_executable(str(self.install), pc_games.COD2)
        env = pc_games.server_env({"IW_PC_INPUT": "old"}, str(self.install), game_exe)
        self.assertNotIn("IW_PC_INPUT", env)
        odd = f"/games/a{os.pathsep}b/Call of Duty 2"
        self.assertNotIn("IW_PC_INPUT", pc_games.server_env({}, odd))
        self.assertEqual(pc_games.server_env({}, str(self.install))["IW_PC_INPUT"], str(self.install))
        self.assertIsNone(pc_games.game_executable("", pc_games.COD2))
        self.assertIsNone(pc_games.game_executable("relative/dir", pc_games.COD2))
        self.assertIsNone(pc_games.game_executable(str(self.install), pc_games.CODUO))   # CoDUOMP.exe isn't there

    @unittest.skipUnless(os.name == "nt", "Windows paths")
    def test_windows_drive_letters(self):
        drive = Path(self._tmp.name).drive
        self.assertTrue(drive)
        cmd = pc_games.newcopy_make(self.exe, pc_games.COD2, str(self.out), str(self.install))
        self.assertTrue(cmd[cmd.index("--from") + 1].startswith(drive))
        self.assertEqual(pc_games.newcopy_check(self.exe, str(self.out))[-1], str(self.out))
        self.assertIn("full path", pc_games.check_copy_folder("copies", ""))
        self.assertNotEqual(pc_games.check_copy_folder(str(self.install / "out"), str(self.install).upper()), "")


class FoundInstallTests(unittest.TestCase):
    STEAM_AND_CB = """Call of Duty 2 (cod2)
  version   1.3 (the engine targets 1.3)
  source    Steam (app 2630)
  folder    /games/Steam/steamapps/common/Call of Duty 2
  archives  16 found, 1.2 GB, none missing

Call of Duty 2 (cod2)
  version   1.0
  source    folder
  folder    /cb/Call of Duty 2
  archives  9 found, 700.0 MB, 7 missing
  missing   main/iw_09.iwd
"""

    def test_the_chosen_folder_is_described(self):
        found = pc_games.parse_find(self.STEAM_AND_CB)
        self.assertEqual(pc_games.pick_found(found, "cod2", "/cb/Call of Duty 2").version, "1.0")
        self.assertEqual(pc_games.pick_found(found, "cod2", "/cb/Call of Duty 2/").version, "1.0")
        self.assertEqual(pc_games.pick_found(found, "cod2", "").version, "1.3")          # no folder: Steam's
        self.assertEqual(pc_games.pick_found(found, "cod2", "/elsewhere").version, "1.0")  # spelled differently
        self.assertIsNone(pc_games.pick_found(found, "coduo", ""))
        windows = pc_games.parse_find(self.STEAM_AND_CB.replace("/cb/Call of Duty 2", "\\\\?\\/cb/Call of Duty 2"))
        self.assertEqual(pc_games.pick_found(windows, "cod2", "/cb/Call of Duty 2").version, "1.0")

    def test_short_summary(self):
        steam, cb = pc_games.parse_find(self.STEAM_AND_CB)
        self.assertEqual(pc_games.find_summary(steam), "Call of Duty 2 1.3 (Steam): 16 found, 1.2 GB, none missing.")
        self.assertEqual(pc_games.find_summary(cb),
                         "Call of Duty 2 1.0 (your folder): 9 found, 700.0 MB, 7 missing: main/iw_09.iwd.")


class InstallCopyTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self.output = self.base / "out put"
        self.built = self.base / "target" / "iw-server"
        self.built.parent.mkdir()
        self.built.write_bytes(b"new build")

    def tearDown(self):
        self._tmp.cleanup()

    def test_same_file_is_left_alone(self):
        dest = pc_games.install_copy(self.built, self.output)
        self.assertEqual(dest.read_bytes(), b"new build")
        before = dest.stat().st_mtime_ns
        with mock.patch.object(pc_games.shutil, "copy2", side_effect=AssertionError("copied again")):
            self.assertEqual(pc_games.install_copy(self.built, self.output), dest)
        self.assertEqual(dest.stat().st_mtime_ns, before)

    def test_running_copy_on_windows_is_moved_aside(self):
        """Windows can't replace a running program's file, but can rename it."""
        dest = self.output / "tools" / "iw-server"
        dest.parent.mkdir(parents=True)
        dest.write_bytes(b"old, running")
        real_replace = os.replace
        calls = []

        def replace(src, dst):
            calls.append((Path(src).name, Path(dst).name))
            if Path(dst) == dest and Path(src).name.endswith(".new") and len(calls) == 1:
                raise PermissionError("in use")
            return real_replace(src, dst)
        with mock.patch.object(pc_games.os, "replace", side_effect=replace):
            self.assertEqual(pc_games.install_copy(self.built, self.output), dest)
        self.assertEqual(dest.read_bytes(), b"new build")
        self.assertEqual((dest.parent / "iw-server.old").read_bytes(), b"old, running")
        self.assertFalse((dest.parent / "iw-server.new").exists())
        # The next time, the old copy (no longer running) is removed.
        self.built.write_bytes(b"newer build")
        pc_games.install_copy(self.built, self.output)
        self.assertFalse((dest.parent / "iw-server.old").exists())

    @unittest.skipIf(os.name == "nt" or not Path("/bin/sleep").is_file(), "Linux: a running program's file")
    def test_running_copy_on_linux_is_replaced(self):
        """Two LAN servers: the second start updates <output>/tools/iw-server
        while the first one runs from it ("Text file busy" when written over)."""
        import shutil as sh
        import subprocess
        dest = self.output / "tools" / "iw-server"
        dest.parent.mkdir(parents=True)
        sh.copy2("/bin/sleep", dest)
        proc = subprocess.Popen([str(dest), "30"])
        try:
            time.sleep(0.2)
            sh.copy2(sys.executable, self.built)        # a different program
            self.assertEqual(pc_games.install_copy(self.built, self.output), dest)
            self.assertIsNone(proc.poll(), "the running server keeps running")
            self.assertEqual(dest.stat().st_size, self.built.stat().st_size)
        finally:
            proc.kill()
            proc.wait()

    def test_a_failed_update_uses_the_copy_there(self):
        class Rust:
            cargo, env = Path("cargo"), {}

            @staticmethod
            def rust_problems():
                return []
        tool_root = self.base / "tool"
        (self.base / "engine" / "tools" / "iw_server").mkdir(parents=True)
        (self.base / "engine" / "tools" / "iw_server" / "Cargo.toml").write_text("")
        ready = self.output / "tools" / ("iw-server.exe" if os.name == "nt" else "iw-server")
        ready.parent.mkdir(parents=True)
        ready.write_bytes(b"earlier")
        seen = []
        log = Log(sinks=[lambda k, t, f: seen.append((k, t))])
        with mock.patch.object(pc_games.deps, "detect", return_value=Rust()), \
                mock.patch.object(pc_games, "build_tool", return_value=self.built), \
                mock.patch.object(pc_games, "install_copy", side_effect=PermissionError("in use")):
            got = pc_games.ensure_tool("iw-server", tool_root, self.output, self.base / "work", 1, None, log,
                                       threading.Event(), True)
        self.assertEqual(got, ready)
        self.assertTrue(any(k == "warn" and "in use" in t for k, t in seen), seen)


class CargoHomeTests(unittest.TestCase):
    def test_the_registry_named_is_where_cargo_writes(self):
        """A system cargo (/usr/bin/cargo) still keeps its downloads in
        ~/.cargo, unless CARGO_HOME says otherwise (the project's own Rust)."""
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("CARGO_HOME", None)
            self.assertEqual(Path(pc_games.build_install().where), Path.home() / ".cargo" / "registry")
        env = {"CARGO_HOME": str(Path("/project/work/tools/rust/cargo"))}
        self.assertEqual(Path(pc_games.build_install(env).where), Path(env["CARGO_HOME"]) / "registry")
        self.assertEqual(pc_games.cargo_home(env), Path(env["CARGO_HOME"]))


class LookTests(unittest.TestCase):
    def test_theme_names_per_tab(self):
        self.assertEqual(theme.picker_label("cod3", "cod3"), "Call of Duty 3")
        self.assertEqual(theme.picker_label("cod3", "waw"), "Brass and black")
        self.assertEqual(theme.picker_label("waw", "waw"), "World at War")
        for own in ("cod3", "waw", "cod2", "coduo"):
            labels = [theme.picker_label(t, own) for t in theme.palettes_for(own)]
            self.assertEqual(len(set(labels)), len(labels))
            for t in theme.palettes_for(own):
                self.assertEqual(theme.picker_id(theme.picker_label(t, own), own), t)
        self.assertEqual(theme.picker_id("nonsense", "cod2"), "cod2")

    def test_accent_stays_visible(self):
        for pid in theme.ALL_PALETTES:
            for accent in ("", "#111111", "#000000", "#ffffff", "#1a1410"):
                c = theme.colors(pid, accent)
                for bg in ("bg", "sidebar"):
                    self.assertGreaterEqual(theme.contrast(c["accent"], c[bg]), 3.0 - 1e-9, f"{pid} {accent} {bg}")

    def test_status_labels_look_different(self):
        """Ready, Testing, Next and Later: four colors, none of them the
        tab's accent, in every palette."""
        for pid in theme.ALL_PALETTES:
            c = theme.colors(pid)
            colored = [_hue_sat(c[k]) for k in ("chip_good", "chip_warn", "chip_info")]
            for i, (h1, s1) in enumerate(colored):
                self.assertGreaterEqual(s1, 0.3, pid)
                for h2, _ in colored[i + 1:]:
                    self.assertGreaterEqual(min(abs(h1 - h2), 360 - abs(h1 - h2)), 40, pid)
            self.assertLessEqual(_hue_sat(c["chip_dim"])[1], 0.1, pid)      # "Later": gray
            self.assertGreater(_distance(c["chip_info"], c["accent"]), 60, pid)

    def test_waw_fields_are_not_red(self):
        r, g, b = theme.parse(theme.colors("waw")["border"])
        self.assertLess(r - min(g, b), 30)

    def test_each_game_gets_its_own_title_type(self):
        linux = ["DejaVu Sans", "DejaVu Serif", "Liberation Sans", "Liberation Serif", "Barlow Semi Condensed"]
        fonts = theme.assign_display_fonts(games.GAME_ORDER, linux, "UI")
        self.assertEqual(len({f for f, _ in fonts.values()}), 4, fonts)
        self.assertEqual(fonts["cod3"], ("Barlow Semi Condensed", "bold"))
        windows = ["Impact", "Georgia", "Palatino Linotype", "Bahnschrift", "Arial"]
        fonts = theme.assign_display_fonts(games.GAME_ORDER, windows, "UI")
        self.assertEqual([fonts[g][0] for g in games.GAME_ORDER], ["Bahnschrift", "Impact", "Georgia",
                                                                   "Palatino Linotype"])
        bare = theme.assign_display_fonts(games.GAME_ORDER, ["DejaVu Serif"], "UI")
        self.assertEqual(bare["cod3"], ("UI", "bold"))      # nothing of its own: the UI font

    def test_header_drawings_stay_dark_behind_the_title(self):
        for color in ("#ffd400", "#ffffff", "#c49854", "#101010"):
            for cap in (0.10, 0.14):
                self.assertLessEqual(theme.luminance(theme.darken_to(color, cap)), cap + 1e-3)
        self.assertEqual(theme.darken_to("#101010", 0.1), "#101010")

    def test_header_height_follows_the_window(self):
        self.assertEqual(games.hero_height(640), 0)            # low window: the page gets the room
        self.assertEqual(games.hero_height(800), 120)
        self.assertEqual(games.hero_height(2000), 140)
        self.assertEqual(games.hero_height(1280, 2.0), 0)      # 640 at 200%
        self.assertEqual(games.hero_height(1600, 2.0), 240)
        self.assertEqual(games.hero_height(801) % 4, 0)


def _distance(a, b) -> float:
    return sum((x - y) ** 2 for x, y in zip(theme.parse(a), theme.parse(b))) ** 0.5


def _hue_sat(color):
    import colorsys
    h, _, s = colorsys.rgb_to_hls(*(v / 255 for v in theme.parse(color)))
    return h * 360, s


class ProjectFontTests(unittest.TestCase):
    SOURCE = Path(__file__).resolve().parent.parent / fonts.SOURCE

    @unittest.skipUnless(SOURCE.is_file(), "the game's font isn't in this copy")
    def test_the_games_font_is_decoded(self):
        data = fonts.font_from_inc(self.SOURCE.read_text(encoding="ascii"))
        self.assertEqual(data[:4], b"\x00\x01\x00\x00")
        self.assertIn("Barlow".encode("utf-16-be"), data)
        with tempfile.TemporaryDirectory() as tmp:
            path = fonts.write_font(self.SOURCE.parents[4], Path(tmp))
            self.assertEqual(path.read_bytes(), data)
            self.assertTrue((path.parent / "OFL.txt").is_file())   # its licence goes with it

    def test_damaged_data_is_refused(self):
        with self.assertRaises(ValueError):
            fonts.stb_decompress(b"\x57\xbc\x00\x00" + bytes(14))
        with self.assertRaises(ValueError):
            fonts.stb_decompress(b"not compressed at all")
        self.assertEqual(fonts.decode85("#####"), bytes(4))


class DepsTests(unittest.TestCase):
    def test_no_7zip_for_rust_only_tools(self):
        tc = deps.Toolchain()
        tc.cargo = Path("cargo")
        tc.rust_version = (1, 90)
        tc.linker = Path("cc")
        with tempfile.TemporaryDirectory() as tmp:
            keys = [i.key for i in deps.plan(tc, Path(tmp), True, recomp=False)]
            lean = [i.key for i in deps.plan(tc, Path(tmp), True, recomp=False, archives=False)]
        self.assertTrue(any(k.startswith("7z") for k in keys))
        self.assertFalse(any(k.startswith("7z") for k in lean))


if __name__ == "__main__":
    unittest.main()

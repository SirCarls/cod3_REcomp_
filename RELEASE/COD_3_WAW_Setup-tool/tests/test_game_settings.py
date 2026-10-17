"""Tests for the game's settings file (c3w/game_settings.py): reading
cod3.toml as the SDK does, changing only the given settings while every
other byte stays, checking values, escaping like SDK patch 0026, and not
writing while the game runs. Also checks that the setup tool's non-GUI
modules don't need Tk.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOL = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOL))

from c3w import game_settings as gs  # noqa: E402
from c3w.common import SetupError  # noqa: E402

SAMPLE = (
    "# Auto-generated cvar configuration\n"
    "mnk_mode = true\n"
    'keybind_x = "R,F"   # reload\n'
    "cod3_max_fps = 85\n"
    'cod3_render_resolution = "1080p"\n'
    "fullscreen = false\n"
    "cod3_volume = 0.250000\n"
    "anisotropic_override = -1\n"
    'log_file = "C:\\\\Games\\\\COD3\\\\userdata\\\\logs\\\\cod3.log"\n'
    "\n"
    "[cod3]\n"
    "skip_movies = true\n"
)


class GameSettingsCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.out = Path(self._tmp.name)
        self.file = self.out / "cod3.toml"
        patcher = mock.patch.object(gs, "game_running", return_value=False)
        self.running = patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        self._tmp.cleanup()


class TableTests(unittest.TestCase):
    def test_settings_are_well_formed(self):
        keys = [s.key for s in gs.SETTINGS]
        self.assertEqual(len(keys), len(set(keys)))
        for s in gs.SETTINGS:
            self.assertIn(s.kind, ("choice", "bool", "int"), s.key)
            self.assertTrue(s.valid(s.default), s.key)
            self.assertTrue(s.label and s.help, s.key)
            self.assertTrue(set(s.platforms) <= {"linux", "windows"}, s.key)
            if s.kind == "choice":
                self.assertIn(s.default, [v for v, _ in s.choices], s.key)
        self.assertEqual(gs.BY_KEY["cod3_graphics_api"].platforms, ("windows",))
        self.assertNotIn("cod3_graphics_api", [s.key for s in gs.for_platform("linux")])
        for key in ("cod3_render_resolution", "cod3_render_method", "cod3_render_percent", "cod3_live_resolution",
                    "cod3_aspect_ratio", "cod3_max_fps", "fullscreen", "vsync", "swap_post_effect",
                    "cod3_hd_textures", "cod3_shadow_quality", "cod3_graphics_api", "cod3_skip_movies",
                    "cod3_volume", "cod3_glyph_style"):
            self.assertIn(key, gs.BY_KEY)

    def test_defaults_match_the_game(self):
        d = {s.key: s.default for s in gs.SETTINGS}
        self.assertEqual(d["cod3_render_resolution"], "auto")
        self.assertEqual(d["cod3_render_method"], "native")   # the game draws every pixel itself
        self.assertEqual(d["cod3_render_percent"], 100)
        self.assertEqual(d["cod3_max_fps"], 60)
        self.assertIs(d["fullscreen"], True)
        self.assertIs(d["vsync"], False)            # the SDK allows tearing by default
        self.assertEqual(d["swap_post_effect"], "none")
        self.assertEqual(d["cod3_shadow_quality"], "original")
        self.assertEqual(d["cod3_volume"], 100)
        self.assertEqual(d["cod3_glyph_style"], "xbox")

    def test_check_values(self):
        gs.check({"cod3_max_fps": 144, "cod3_render_resolution": "2560x1080", "fullscreen": False,
                  "anisotropic_override": 5, "cod3_volume": 0})
        bad = [{"cod3_max_fps": 1001}, {"cod3_max_fps": -1}, {"cod3_max_fps": True}, {"cod3_max_fps": "60"},
               {"cod3_render_resolution": "100x100"}, {"cod3_render_resolution": "1080p; rm"},
               {"fullscreen": 1}, {"anisotropic_override": True}, {"anisotropic_override": 9},
               {"swap_post_effect": "msaa"}, {"vsync": "yes"}, {"cod3_volume": 101}, {"nope": 1}]
        for values in bad:
            with self.assertRaises(SetupError, msg=repr(values)):
                gs.check(values)

    def test_render_resolution_values(self):
        for size in ("4320p", "8k", "4k", "640x360", "1600x900", "15360x4320", "original"):
            gs.check({"cod3_render_resolution": size})
        gs.check({"cod3_render_method": "scaled", "cod3_render_percent": 25})
        gs.check({"cod3_render_percent": 100})
        bad = [{"cod3_render_resolution": "320x240"}, {"cod3_render_resolution": "639x360"},
               {"cod3_render_resolution": "15361x4320"}, {"cod3_render_resolution": "1920x4321"},
               {"cod3_render_resolution": "1920X1080"}, {"cod3_render_method": "fast"},
               {"cod3_render_method": ""}, {"cod3_render_percent": 24}, {"cod3_render_percent": 101},
               {"cod3_render_percent": True}, {"cod3_render_percent": "50"}]
        for values in bad:
            with self.assertRaises(SetupError, msg=repr(values)):
                gs.check(values)

    def test_custom_aspect_ratios(self):
        # From the console's 4:3 to 48:9 (game/ultrawide.h kMinAspect/kMaxAspect),
        # and "resolution": the render resolution's shape (kMatchResolution).
        for shape in ("24:10", "2.39:1", "2.39", "16:9", "48:9", "21:9", "32:9", "auto", "resolution",
                      "4:3", "16:10", "1.6", "3:2"):
            gs.check({"cod3_aspect_ratio": shape})
        for shape in ("5:4", "1:1", "49:9", "0:9", "21:0", "abc", "21:9x", "", "1" * 17, "Resolution"):
            with self.assertRaises(SetupError, msg=shape):
                gs.check({"cod3_aspect_ratio": shape})

    def test_aspect_choices_match_f2(self):
        # The setup tool lists the same shapes, in the same order, as F2 >
        # Video > Aspect ratio (ui/hud_overlay.cpp) when the game's sources are here.
        st = next(s for s in gs.SETTINGS if s.key == "cod3_aspect_ratio")
        values = [v for v, _ in st.choices]
        self.assertEqual(values, ["auto", "resolution", "4:3", "16:10", "16:9", "21:9", "32:9"])
        f2 = Path(__file__).resolve().parents[1] / "recomp" / "src" / "ui" / "hud_overlay.cpp"
        if not f2.is_file():
            self.skipTest("the game's sources aren't next to the tests")
        text = f2.read_text(encoding="utf-8")
        m = re.search(r'const char\* v\[\] = \{"auto", cod3::kMatchResolution, ([^}]*)\}', text)
        self.assertIsNotNone(m)
        self.assertEqual(["auto", "resolution"] + re.findall(r'"([^"]+)"', m.group(1)), values)

    def test_choices_match_the_game_source(self):
        """The resolution list and limits are the game's own (game/render_resolution.h),
        when the tool's copy of the game source has them."""
        header = TOOL / "recomp" / "src" / "game" / "render_resolution.h"
        text = header.read_text(encoding="utf-8") if header.is_file() else ""
        table = re.search(r"kResolutionChoices\[\]\s*=\s*\{(.*?)\};", text, re.S)
        if not table:
            self.skipTest("the tool's copy of the game source has no kResolutionChoices yet")
        rows = re.findall(r'\{"([^"]*)",\s*"([^"]*)",\s*"[^"]*"\}', table.group(1))
        self.assertEqual(rows, [(v, label) for v, label in gs.BY_KEY["cod3_render_resolution"].choices])
        limits = dict(re.findall(r"constexpr uint32_t (k(?:Min|Max)Custom(?:Width|Height)) = (\d+);", text))
        self.assertEqual((int(limits["kMinCustomWidth"]), int(limits["kMinCustomHeight"])), gs.MIN_CUSTOM_SIZE)
        self.assertEqual((int(limits["kMaxCustomWidth"]), int(limits["kMaxCustomHeight"])), gs.MAX_CUSTOM_SIZE)
        source = header.with_suffix(".cpp").read_text(encoding="utf-8")
        method = re.search(r'REXCVAR_DEFINE_STRING\(cod3_render_method, "(\w+)".*?\.allowed\(\{(.*?)\}\)', source, re.S)
        self.assertEqual(method.group(1), gs.BY_KEY["cod3_render_method"].default)
        self.assertEqual(re.findall(r'"(\w+)"', method.group(2)),
                         [v for v, _ in gs.BY_KEY["cod3_render_method"].choices])
        percent = re.search(r"REXCVAR_DEFINE_INT32\(cod3_render_percent, (\d+),.*?\.range\((\d+), (\d+)\)",
                            source, re.S)
        st = gs.BY_KEY["cod3_render_percent"]
        self.assertEqual(tuple(int(x) for x in percent.groups()), (st.default, st.min, st.max))

    def test_toml_string_escapes_like_patch_0026(self):
        self.assertEqual(gs.toml_string('a"b\\c\nd\x7f'), '"a\\"b\\\\c\\u000Ad\\u007F"')
        value, used = gs._parse_value(gs.toml_string('C:\\x "y"\n'))
        self.assertEqual(value, 'C:\\x "y"\n')


class LoadTests(GameSettingsCase):
    def test_no_file_gives_defaults(self):
        self.assertEqual(gs.load(self.out), {s.key: s.default for s in gs.SETTINGS})

    def test_values_from_the_file(self):
        self.file.write_text(SAMPLE, encoding="utf-8")
        v = gs.load(self.out)
        self.assertEqual(v["cod3_max_fps"], 85)
        self.assertEqual(v["cod3_render_resolution"], "1080p")
        self.assertIs(v["fullscreen"], False)
        self.assertEqual(v["cod3_volume"], 25)
        self.assertEqual(v["anisotropic_override"], -1)
        self.assertIs(v["cod3_skip_movies"], True)         # [cod3] skip_movies, as the SDK flattens it
        raw = gs.read_values(self.out)
        self.assertEqual(raw["keybind_x"], "R,F")
        self.assertEqual(raw["log_file"], "C:\\Games\\COD3\\userdata\\logs\\cod3.log")

    def test_values_that_dont_fit_give_defaults(self):
        self.file.write_text('cod3_max_fps = "fast"\nfullscreen = 3\ncod3_shadow_quality = "ultra"\n'
                             'cod3_volume = "loud"\nvulkan_allow_present_mode_immediate = false\n'
                             'swap_post_effect = """\nfxaa"""\ncod3_glyph_style = [1,\n2]\n', encoding="utf-8")
        v = gs.load(self.out)
        self.assertEqual(v["cod3_max_fps"], 60)
        self.assertIs(v["fullscreen"], True)
        self.assertEqual(v["cod3_shadow_quality"], "original")
        self.assertEqual(v["cod3_volume"], 100)
        self.assertIs(v["vsync"], True)
        self.assertEqual(v["swap_post_effect"], "none")
        self.assertEqual(v["cod3_glyph_style"], "xbox")


class SaveTests(GameSettingsCase):
    def test_only_the_given_settings_change(self):
        self.file.write_text(SAMPLE, encoding="utf-8")
        gs.save(self.out, {"cod3_max_fps": 144, "cod3_render_resolution": "2560x1080"})
        expected = SAMPLE.replace("cod3_max_fps = 85", "cod3_max_fps = 144") \
            .replace('"1080p"', '"2560x1080"')
        self.assertEqual(self.file.read_text(encoding="utf-8"), expected)

    def test_saving_what_is_there_changes_nothing(self):
        self.file.write_bytes(SAMPLE.replace("\n", "\r\n").encode("utf-8") + b"# \xff not utf-8\r\n")
        before = self.file.read_bytes()
        gs.save(self.out, gs.load(self.out))
        self.assertEqual(self.file.read_bytes(), before)

    def test_new_settings_go_before_the_first_table(self):
        self.file.write_text(SAMPLE, encoding="utf-8")
        gs.save(self.out, {"cod3_shadow_quality": "off", "cod3_skip_movies": False})
        text = self.file.read_text(encoding="utf-8")
        expected = SAMPLE.replace('cod3.log"\n\n[cod3]', 'cod3.log"\ncod3_shadow_quality = "off"\n\n[cod3]') \
            .replace("skip_movies = true", "skip_movies = false")   # changed where it was
        self.assertEqual(text, expected)
        self.assertEqual(gs.load(self.out)["cod3_shadow_quality"], "off")

    def test_crlf_files_stay_crlf(self):
        self.file.write_bytes(b"fullscreen = true\r\n")
        gs.save(self.out, {"cod3_max_fps": 30})
        self.assertEqual(self.file.read_bytes(), b"fullscreen = true\r\ncod3_max_fps = 30\r\n")

    def test_a_file_without_a_last_newline(self):
        self.file.write_bytes(b"fullscreen = true")
        gs.save(self.out, {"cod3_max_fps": 30})
        self.assertEqual(self.file.read_bytes(), b"fullscreen = true\ncod3_max_fps = 30\n")

    def test_new_file_and_defaults_not_added(self):
        gs.save(self.out, {"cod3_max_fps": 60, "fullscreen": True, "cod3_volume": 50})
        self.assertEqual(self.file.read_text(encoding="utf-8"), gs.HEADER + "cod3_volume = 0.500000\n")

    def test_vsync_writes_both_renderers_inverted(self):
        gs.save(self.out, {"vsync": True})
        text = self.file.read_text(encoding="utf-8")
        self.assertIn("vulkan_allow_present_mode_immediate = false\n", text)
        self.assertIn("d3d12_allow_variable_refresh_rate_and_tearing = false\n", text)
        self.assertIs(gs.load(self.out)["vsync"], True)
        gs.save(self.out, {"vsync": False})
        self.assertIn("vulkan_allow_present_mode_immediate = true\n", self.file.read_text(encoding="utf-8"))

    def test_comments_after_values_stay(self):
        self.file.write_text('cod3_glyph_style = "xbox"  # my pick\n', encoding="utf-8")
        gs.save(self.out, {"cod3_glyph_style": "playstation"})
        self.assertEqual(self.file.read_text(encoding="utf-8"), 'cod3_glyph_style = "playstation"  # my pick\n')

    def test_bad_values_write_nothing(self):
        self.file.write_text(SAMPLE, encoding="utf-8")
        with self.assertRaises(SetupError):
            gs.save(self.out, {"cod3_max_fps": 144, "swap_post_effect": "bogus"})
        self.assertEqual(self.file.read_text(encoding="utf-8"), SAMPLE)

    def test_refuses_while_the_game_runs(self):
        self.file.write_text(SAMPLE, encoding="utf-8")
        self.running.return_value = True
        with self.assertRaises(SetupError):
            gs.save(self.out, {"cod3_max_fps": 144})
        self.assertEqual(self.file.read_text(encoding="utf-8"), SAMPLE)

    def test_no_output_folder(self):
        with self.assertRaises(SetupError):
            gs.save(self.out / "missing", {"cod3_max_fps": 144})


class RunningTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.proc = Path(self._tmp.name) / "proc"
        self.proc.mkdir()
        self.pid = 1000

    def tearDown(self):
        self._tmp.cleanup()

    def process(self, comm: str, *args: str) -> None:
        self.pid += 1
        d = self.proc / str(self.pid)
        d.mkdir()
        (d / "comm").write_text(comm + "\n")
        (d / "cmdline").write_bytes(b"\0".join(a.encode() for a in args) + b"\0")
        (self.proc / "self").mkdir(exist_ok=True)   # not a process

    def test_nothing_running(self):
        self.process("bash", "/bin/bash")
        self.process("python3", "python3", "launcher/cod3_waw_setup.py", "--launcher")
        self.process("vim", "vim", "cod3")                 # editing a file called cod3
        self.assertFalse(gs.game_running(proc=self.proc))

    def test_linux_game(self):
        self.process("cod3", "/games/COD3/bin/cod3")
        self.assertTrue(gs.game_running(proc=self.proc))

    def test_appimage_name_cut_to_15_characters(self):
        self.process("CoD3-x86_64.App", "/games/COD3/CoD3-x86_64.AppImage")
        self.assertTrue(gs.game_running(proc=self.proc))

    def test_windows_game_under_wine(self):
        self.process("wine64-preload", "wine64", "Z:\\games\\COD3\\bin-windows\\cod3.exe")
        self.assertTrue(gs.game_running(proc=self.proc))

    def test_names_from_the_record(self):
        out = Path(self._tmp.name) / "out"
        out.mkdir()
        (out / "cod3_waw.json").write_text(json.dumps({"cod3_exe_linux": "bin/MyGame"}))
        self.process("MyGame", "./bin/MyGame")
        self.assertFalse(gs.game_running(proc=self.proc))
        self.assertTrue(gs.game_running(out, proc=self.proc))

    def test_windows_asks_tasklist(self):
        csv_out = '"System","4","Services","0","1,234 K"\r\n"cod3.exe","7","Console","1","900,000 K"\r\n'
        with mock.patch.object(gs.common, "IS_WINDOWS", True), mock.patch.object(gs.common, "UNDER_WINE", False), \
                mock.patch.object(gs.subprocess, "run") as run:
            run.return_value = mock.Mock(stdout=csv_out)
            self.assertTrue(gs.game_running())
            args = run.call_args[0][0]
            self.assertEqual(args[0], "tasklist")
            self.assertNotIn("shell", run.call_args[1])
            run.return_value = mock.Mock(stdout='"explorer.exe","1","Console","1","1 K"\r\n')
            self.assertFalse(gs.game_running())
            run.side_effect = OSError("no tasklist")
            self.assertFalse(gs.game_running())


class NoTkTests(unittest.TestCase):
    def test_backend_modules_dont_import_tkinter(self):
        code = ("import sys; sys.modules['tkinter'] = None\n"
                "import c3w.profiles, c3w.repair, c3w.game_settings, c3w.logtools, c3w.pipeline\n"
                "print('ok')")
        r = subprocess.run([sys.executable, "-c", code], cwd=str(TOOL), capture_output=True, text=True, timeout=60)
        self.assertEqual(r.stdout.strip(), "ok", r.stderr)


if __name__ == "__main__":
    unittest.main()

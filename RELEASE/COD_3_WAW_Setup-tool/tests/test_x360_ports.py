"""Tests for c3w/x360_ports.py: World at War, Call of Duty 2 and Call of Duty 4 recompiled from their Xbox 360
discs on Call of Duty 3's SDK [waw-decomp]. No disc, compiler or Tkinter needed: made-up programs stand in for the
discs' (their SHA-1s are swapped in for the test), and nothing is built.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import game_settings, x360_ports  # noqa: E402
from c3w.common import IS_LINUX, Log, SetupError  # noqa: E402
from c3w.pipeline import BuildOptions  # noqa: E402

CAMPAIGN = b"made-up campaign program"
MULTI = b"made-up multiplayer program"


def fake_port(base: x360_ports.Port) -> x360_ports.Port:
    """`base` with the made-up programs' SHA-1s."""
    progs = list(base.programs)
    progs[0] = replace(progs[0], sha1=hashlib.sha1(CAMPAIGN).hexdigest())
    if len(progs) > 1:
        progs[1] = replace(progs[1], sha1=hashlib.sha1(MULTI).hexdigest())
    return replace(base, programs=tuple(progs))


class TableTests(unittest.TestCase):
    def test_three_ports(self):
        self.assertEqual(x360_ports.PORT_ORDER, ("cod2", "waw", "cod4"))
        for pid in x360_ports.PORT_ORDER:
            p = x360_ports.port(pid)
            self.assertEqual(p.programs[0].xex, "default.xex")
            self.assertEqual(p.programs[0].label, "Campaign")
            for q in p.programs:
                self.assertRegex(q.sha1, r"^[0-9a-f]{40}$")
                self.assertEqual(q.codegen_target, q.name + "_codegen")
        with self.assertRaises(SetupError):
            x360_ports.port("halo")

    def test_projects(self):
        self.assertEqual(x360_ports.COD2.projects, [""])        # cod2mp is inside the port's own project
        self.assertEqual(x360_ports.COD4.projects, ["", "mp"])  # cod4mp is a project of its own
        self.assertEqual(x360_ports.WAW.projects, [""])
        self.assertEqual(x360_ports.COD4.program("cod4mp").asset_dir, "mp/assets")

    def test_the_ports_folders_are_next_to_the_tool(self):
        tool = Path(__file__).resolve().parent.parent
        for p in x360_ports.PORTS.values():
            self.assertTrue((tool / p.template / "CMakeLists.txt").is_file(), p.template)
            for q in p.programs:
                manifest = tool / p.template / (q.project or "") / f"{q.name}_manifest.toml"
                if q.name == "cod2mp":
                    manifest = tool / p.template / "mp" / "cod2mp_manifest.toml"
                self.assertTrue(manifest.is_file(), manifest)
                self.assertIn(q.sha1, manifest.read_text(encoding="utf-8") +
                              (tool / p.template / f"{p.programs[0].name}_manifest.toml").read_text(encoding="utf-8"))

    def test_launcher_names(self):
        self.assertEqual(x360_ports.launcher_name(x360_ports.COD2, x360_ports.COD2.programs[1]),
                         "Call of Duty 2 Multiplayer.sh")
        self.assertEqual(x360_ports.launcher_name(x360_ports.WAW, x360_ports.WAW.programs[0]), "Call of Duty.sh")
        self.assertEqual(x360_ports.launcher_name(x360_ports.COD4, x360_ports.COD4.programs[0]), "Call of Duty 4.sh")


class DiscTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.port = fake_port(x360_ports.COD2)

    def tearDown(self):
        self.tmp.cleanup()

    def disc(self, campaign=CAMPAIGN, multi=MULTI) -> Path:
        d = self.root / "disc"
        d.mkdir(exist_ok=True)
        if campaign is not None:
            (d / "default.xex").write_bytes(campaign)
        if multi is not None:
            (d / "default_mp.xex").write_bytes(multi)
        return d

    def test_known_disc(self):
        disc, line = x360_ports.check_disc(self.port, str(self.disc()))
        self.assertIsNotNone(disc)
        self.assertIn("the disc the port knows", line)
        self.assertEqual([q.name for q in x360_ports.buildable(self.port, disc)], ["cod2", "cod2mp"])

    def test_unknown_campaign_is_refused_unless_forced(self):
        d = self.disc(campaign=b"another release")
        disc, line = x360_ports.check_disc(self.port, str(d))
        self.assertIsNone(disc)
        self.assertIn("isn't the one the port was made for", line)
        disc, line = x360_ports.check_disc(self.port, str(d), force_unknown=True)
        self.assertIsNotNone(disc)
        self.assertIn("unknown", line)
        self.assertEqual([q.name for q in x360_ports.buildable(self.port, disc, True)], ["cod2", "cod2mp"])

    def test_missing_or_unknown_multiplayer_leaves_it_out(self):
        disc, line = x360_ports.check_disc(self.port, str(self.disc(multi=None)))
        self.assertIn("no default_mp.xex", line)
        self.assertEqual([q.name for q in x360_ports.buildable(self.port, disc)], ["cod2"])
        disc, line = x360_ports.check_disc(self.port, str(self.disc(multi=b"other")))
        self.assertIn("isn't the known one", line)
        self.assertEqual([q.name for q in x360_ports.buildable(self.port, disc)], ["cod2"])

    def test_not_a_disc(self):
        self.assertIsNone(x360_ports.check_disc(self.port, "")[0])
        self.assertIsNone(x360_ports.check_disc(self.port, str(self.root / "nope"))[0])
        empty = self.root / "empty"
        empty.mkdir()
        disc, line = x360_ports.check_disc(self.port, str(empty))
        self.assertIsNone(disc)
        self.assertIn("No default.xex", line)


class PlayTests(unittest.TestCase):
    def test_arguments(self):
        p = x360_ports.COD2
        o = x360_ports.PlayOptions(fullscreen=True, mute=True, skip_films=True, max_fps=144)
        args = o.arguments(p)
        self.assertIn("--fullscreen=true", args)
        self.assertIn("--audio_mute=true", args)
        self.assertIn("--cod2_skip_intro_movies=true", args)
        self.assertIn("--cod2_max_fps=144", args)
        # World at War has no skip setting yet: nothing is passed for it.
        self.assertFalse([a for a in x360_ports.PlayOptions(skip_films=True).arguments(x360_ports.WAW)
                          if "skip" in a])
        self.assertIn("--cod4_fov=90", x360_ports.PlayOptions(fov=90).arguments(x360_ports.COD4))
        # [f2-menus] An unticked Fullscreen box passes nothing, so the display mode set in the game (F2) stays.
        self.assertFalse([a for a in x360_ports.PlayOptions().arguments(p) if "fullscreen" in a])
        self.assertEqual(x360_ports.PlayOptions().arguments(x360_ports.WAW), [])
        # Call of Duty 2's field of view (whole degrees) and Call of Duty 4's frame limit have rows too.
        self.assertIn("--cod2_fov=90", x360_ports.PlayOptions(fov=90.4).arguments(p))
        self.assertIn("--cod4_fov=92.5", x360_ports.PlayOptions(fov=92.5).arguments(x360_ports.COD4))
        self.assertIn("--cod4_max_fps=120", x360_ports.PlayOptions(max_fps=120).arguments(x360_ports.COD4))
        # Each port's own top: cod4_max_fps stops at 250, the other two at 1000.
        self.assertTrue(x360_ports.PlayOptions(max_fps=300).problems(x360_ports.COD4))
        self.assertFalse(x360_ports.PlayOptions(max_fps=300).problems(x360_ports.WAW))
        self.assertFalse(x360_ports.PlayOptions(max_fps=250).problems(x360_ports.COD4))

    def test_problems(self):
        p = x360_ports.COD4
        self.assertEqual(x360_ports.PlayOptions().problems(p), [])
        self.assertTrue(x360_ports.PlayOptions(program="cod2").problems(p))
        self.assertTrue(x360_ports.PlayOptions(fov=10).problems(p))
        self.assertTrue(x360_ports.PlayOptions(max_fps=5000).problems(p))
        self.assertTrue(x360_ports.PlayOptions(graphics_api="glide").problems(p))

    def test_settings_round_trip(self):
        p = x360_ports.COD4
        o = x360_ports.PlayOptions(program="cod4mp", fullscreen=True, fov=95, max_fps=0, graphics_api="d3d12")
        s = json.loads(json.dumps(o.to_settings(p)))
        again = x360_ports.PlayOptions.from_settings(p, s)
        self.assertEqual((again.program, again.fullscreen, again.fov, again.max_fps, again.graphics_api),
                         ("cod4mp", True, 95.0, 0, "d3d12"))
        junk = x360_ports.PlayOptions.from_settings(p, {"port_cod4_fov": "wide", "port_cod4_max_fps": True,
                                                        "port_cod4_graphics_api": "glide"})
        self.assertEqual((junk.fov, junk.max_fps, junk.graphics_api), (0.0, -1, "auto"))

    def test_play_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            p = x360_ports.COD2
            cmd, why = x360_ports.play_command(p, out, x360_ports.PlayOptions())
            self.assertIsNone(cmd)
            self.assertIn("Build", why)
            system = "windows" if not IS_LINUX else "linux"
            exe = "bin/cod2.exe" if system == "windows" else "bin/cod2"
            (out / "cod2" / "bin").mkdir(parents=True)
            (out / "cod2" / exe).write_bytes(b"")
            (out / "cod2" / "cod2.json").write_text(json.dumps({"game": "cod2", f"exe_{system}": {"cod2": exe}}))
            cmd, why = x360_ports.play_command(p, out, x360_ports.PlayOptions(mute=True))
            self.assertEqual(why, "")
            self.assertEqual(Path(cmd[0]), out / "cod2" / exe)
            self.assertIn("--audio_mute=true", cmd)
            cmd, why = x360_ports.play_command(p, out, x360_ports.PlayOptions(program="cod2mp"))
            self.assertIsNone(cmd)
            self.assertIn("isn't built", why)


def _fake_install(out: Path, p) -> Path:
    """A made-up built game in out: its folder, record and (empty) programs. Returns the folder."""
    system = "windows" if not IS_LINUX else "linux"
    folder = out / p.subdir
    (folder / "bin").mkdir(parents=True)
    exes = {q.name: f"bin/{q.name}.exe" if system == "windows" else f"bin/{q.name}" for q in p.programs}
    for rel in exes.values():
        (folder / rel).write_bytes(b"")
    (folder / p.manifest).write_text(json.dumps({"game": p.id, f"exe_{system}": exes}), encoding="utf-8")
    return folder


class GameSettingsTests(unittest.TestCase):
    """[f2-menus] 2026-11-10 The Play page's Settings rows are the game's own <program>.toml (one copy, as Call of
    Duty 3's Game settings), not flags passed at each start."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = Path(self.tmp.name)
        running = mock.patch.object(game_settings, "game_running", return_value=False)
        self.running = running.start()
        self.addCleanup(running.stop)

    def test_each_port_has_its_own_names_ranges_and_defaults(self):
        by = {p.id: {s.key: s for s in x360_ports.game_settings_of(p)} for p in x360_ports.PORTS.values()}
        self.assertEqual(sorted(by["cod2"]), ["audio_mute", "cod2_fov", "cod2_graphics_api", "cod2_max_fps",
                                              "cod2_skip_intro_movies", "fullscreen"])
        self.assertNotIn("waw_skip_intro_movies", by["waw"])          # the port has no such setting
        self.assertEqual((by["cod2"]["cod2_max_fps"].default, by["cod2"]["cod2_max_fps"].max), (60, 1000))
        self.assertEqual((by["cod4"]["cod4_max_fps"].default, by["cod4"]["cod4_max_fps"].max), (0, 250))
        self.assertTrue(by["cod4"]["fullscreen"].default)              # the SDK's own default
        self.assertEqual(by["waw"]["waw_graphics_api"].platforms, ("windows",))
        self.assertEqual(x360_ports.settings_file_name(x360_ports.COD2), "cod2.toml")
        self.assertEqual(x360_ports.settings_file_name(x360_ports.COD2, "cod2mp"), "cod2mp.toml")

    def test_load_reads_the_file_and_says_what_it_has(self):
        p = x360_ports.COD4
        folder = _fake_install(self.out, p)
        values, have = x360_ports.load_game_settings(p, self.out)
        self.assertEqual((values["fullscreen"], values["cod4_fov"], values["cod4_max_fps"]), (True, 0, 0))
        self.assertEqual(have, set())
        (folder / "cod4.toml").write_text("# kept\nfullscreen = false\ncod4_fov = 90.000000\n[UI]\nscale = 2\n",
                                          encoding="utf-8")
        values, have = x360_ports.load_game_settings(p, self.out)
        self.assertEqual((values["fullscreen"], values["cod4_fov"], values["cod4_max_fps"]), (False, 90, 0))
        self.assertEqual(have, {"fullscreen", "cod4_fov"})
        self.assertEqual(x360_ports.load_game_settings(p, self.out, "cod4mp")[1], set())   # the other program's file

    def test_save_changes_only_its_lines(self):
        p = x360_ports.COD4
        folder = _fake_install(self.out, p)
        toml = folder / "cod4.toml"
        toml.write_text("# kept\nfullscreen = true\ncod4_fov = 90.000000\n[UI]\nscale = 2\n", encoding="utf-8")
        x360_ports.save_game_settings(p, self.out, {"fullscreen": False, "cod4_fov": 100, "cod4_max_fps": 120})
        text = toml.read_text(encoding="utf-8")
        for line in ("# kept\n", "fullscreen = false\n", "cod4_fov = 100.000000\n", "cod4_max_fps = 120\n",
                     "[UI]\nscale = 2\n"):
            self.assertIn(line, text)
        self.assertLess(text.index("cod4_max_fps"), text.index("[UI]"))   # new lines go before the first table
        self.assertEqual(sorted(self.running.call_args.kwargs["names"]),
                         ["cod4", "cod4.exe", "cod4mp", "cod4mp.exe"])   # whose programs must not be running
        with self.assertRaises(SetupError):
            x360_ports.save_game_settings(p, self.out, {"cod4_max_fps": 300})        # its own top is 250
        with self.assertRaises(SetupError):
            x360_ports.save_game_settings(p, self.out, {"cod2_fov": 90})             # another game's setting
        self.running.return_value = True
        with self.assertRaises(SetupError):
            x360_ports.save_game_settings(p, self.out, {"cod4_fov": 80})             # the game is running
        self.assertIn("cod4_fov = 100.000000\n", toml.read_text(encoding="utf-8"))

    def test_whole_degrees_and_the_multiplayer_file(self):
        p = x360_ports.COD2
        folder = _fake_install(self.out, p)
        x360_ports.save_game_settings(p, self.out, {"cod2_fov": 90, "fullscreen": False}, "cod2mp")
        self.assertFalse((folder / "cod2.toml").exists())
        text = (folder / "cod2mp.toml").read_text(encoding="utf-8")
        self.assertIn("cod2_fov = 90\n", text)                 # an integer setting: no decimals
        self.assertIn("fullscreen = false\n", text)
        x360_ports.save_game_settings(p, self.out, {"cod2_max_fps": 60}, "cod2mp")   # its default: no line needed
        self.assertNotIn("cod2_max_fps", (folder / "cod2mp.toml").read_text(encoding="utf-8"))

    def test_call_of_duty_3s_file_is_untouched_by_the_new_arguments(self):
        self.assertEqual(game_settings.settings_file(self.out).name, "cod3.toml")
        self.assertEqual(set(game_settings.load(self.out)), set(game_settings.BY_KEY))


class PlayPageInItsOwnProcessTests(unittest.TestCase):
    """[f2-menus] 2026-11-10 PlayPageTests (below) open a window, so they run in a Python of their own: this one
    must stay without tkinter (test_theme checks that), and whatever other tests did to the imported modules
    can't reach them. Skipped where no window can be opened."""

    def test_the_play_page(self):
        env = {**os.environ, "C3W_PLAY_PAGE_TESTS": "1", "PYTHONDONTWRITEBYTECODE": "1"}
        run = subprocess.run([sys.executable, "-B", "-m", "unittest", "-v", "test_x360_ports.PlayPageTests"],
                             cwd=str(Path(__file__).resolve().parent), env=env, capture_output=True, text=True,
                             timeout=180)
        said = run.stdout + run.stderr
        if "no window can be opened here" in said:
            self.skipTest("no window can be opened here")
        self.assertEqual(run.returncode, 0, said[-4000:])
        self.assertIn("Ran 3 tests", said)
        self.assertNotIn("skipped", said)


class PlayPageTests(unittest.TestCase):
    """[f2-menus] 2026-11-10 The Play page itself, on a made-up game folder: the rows fill from the game's file,
    Save writes them back, nothing of them goes on the command line. Run by PlayPageInItsOwnProcessTests."""

    def setUp(self):
        if os.environ.get("C3W_PLAY_PAGE_TESTS") != "1":
            self.skipTest("run by PlayPageInItsOwnProcessTests, in a Python of its own")
        try:
            import tkinter as tk
            from c3w import gui_ports
            self.root = tk.Tk()
        except Exception as e:   # no tkinter, or no display to open a window on
            self.skipTest(f"no window can be opened here: {e}")
        self.addCleanup(self.root.destroy)
        self.root.withdraw()
        self.gui_ports = gui_ports
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = Path(self.tmp.name)
        running = mock.patch.object(game_settings, "game_running", return_value=False)
        running.start()
        self.addCleanup(running.stop)

    def app(self, settings: dict):
        out, test = self.out, self

        class Stub(self.gui_ports.Ports):   # what the page uses of gui.App
            def __init__(self):
                self.root = test.root
                self.pages = {f"{g}_play": {} for g in x360_ports.PORT_ORDER}
                self.worker = None
                self.said = []
                self._init_port_settings(settings)

            def _grid_cols(self, frame):
                frame.columnconfigure(1, weight=1)

            def _update_actions(self):
                pass

            def _out_path(self):
                return out

            def _open(self, path):
                pass

            def _info(self, title, text, kind="info"):
                self.said.append((title, text))

        return Stub()

    def page(self, app, pid: str):
        from tkinter import ttk
        frame = ttk.Frame(self.root)
        frame.pack()
        app._page_port_play(frame, pid)
        self.root.update_idletasks()
        app.pages[f"{pid}_play"]["on_show"]()
        return app.port_vars[pid]

    def test_rows_fill_from_the_file_and_save_writes_them(self):
        p = x360_ports.COD4
        toml = _fake_install(self.out, p) / "cod4.toml"
        toml.write_text("# kept\nfullscreen = true\ncod4_fov = 90.000000\n[UI]\nscale = 2\n", encoding="utf-8")
        app = self.app({})
        v = self.page(app, "cod4")
        self.assertEqual((v["fullscreen"].get(), v["fov"].get(), v["max_fps"].get()), (True, "90", "0"))
        self.assertEqual(app._port_gs_changed("cod4"), {})
        v["fullscreen"].set(False)
        v["fov"].set("100")
        v["max_fps"].set("120")
        v["skip_films"].set(True)
        self.assertTrue(app._port_gs_save("cod4"))
        text = toml.read_text(encoding="utf-8")
        for line in ("# kept\n", "fullscreen = false\n", "cod4_fov = 100.000000\n", "cod4_max_fps = 120\n",
                     "cod4_skip_intro_movies = true\n", "[UI]\nscale = 2\n"):
            self.assertIn(line, text)
        self.assertIn("cod4.toml", v["gs_reason"].get())
        self.assertEqual(app._port_gs_changed("cod4"), {})
        cmd, why = x360_ports.play_command(p, self.out, app._port_play_options("cod4"))
        self.assertEqual((cmd[1:], why), ([], ""))           # nothing of the rows on the command line
        # the game's F2 changed the file: the rows follow when the game ends or the page is shown again
        toml.write_text(text.replace("cod4_fov = 100.000000", "cod4_fov = 0.000000"), encoding="utf-8")
        app._port_gs_load("cod4")
        self.assertEqual(v["fov"].get(), self.gui_ports.NO_FOV)
        # a row that is wrong isn't written, and the page says why
        v["max_fps"].set("300")
        self.assertFalse(app._port_gs_save("cod4", quiet=True))
        self.assertIn("250", v["gs_reason"].get())
        self.assertIn("cod4_max_fps = 120\n", toml.read_text(encoding="utf-8"))
        self.assertFalse(app._port_gs_save("cod4"))
        self.assertEqual(app.said[-1][0], "Not saved")

    def test_what_the_tool_had_saved_goes_into_the_file_once(self):
        # Until 2026-11-10 the rows were kept in settings.json and passed at each start. A row the game's file has
        # no line for keeps that value and is written at the first Save or Play, for each program's file.
        p = x360_ports.COD2
        folder = _fake_install(self.out, p)
        app = self.app({"port_cod2_fullscreen": False, "port_cod2_fov": 90})
        v = self.page(app, "cod2")
        self.assertEqual((v["fullscreen"].get(), v["fov"].get(), v["max_fps"].get()), (False, "90", "60"))
        self.assertEqual(app._port_gs_changed("cod2"), {"fullscreen": False, "cod2_fov": 90})
        app.pages["cod2_play"]["on_show"]()                    # shown again: rows not saved yet stay
        self.assertEqual(v["fov"].get(), "90")
        self.assertTrue(app._port_gs_save("cod2"))
        text = (folder / "cod2.toml").read_text(encoding="utf-8")
        self.assertIn("fullscreen = false\n", text)
        self.assertIn("cod2_fov = 90\n", text)
        self.assertNotIn("cod2_max_fps", text)                 # 60 is the port's own: no line
        v["program"].set("Multiplayer")                        # the other program has its own file
        self.assertEqual(app.port_gs["cod2"]["file"][1], "cod2mp")
        self.assertTrue(app._port_gs_save("cod2"))
        self.assertIn("cod2_fov = 90\n", (folder / "cod2mp.toml").read_text(encoding="utf-8"))
        self.assertEqual(app._port_play_options("cod2").program, "cod2mp")
        self.assertEqual((folder / "cod2.toml").read_text(encoding="utf-8"), text)

    def test_a_game_that_isnt_built_has_no_file_to_write(self):
        app = self.app({})
        v = self.page(app, "waw")
        v["fullscreen"].set(True)
        self.assertTrue(app._port_gs_save("waw"))
        self.assertFalse((self.out / "waw").exists())


class PipelineTests(unittest.TestCase):
    def pipe(self, tmp: Path, **kw) -> "x360_ports.PortPipeline":
        opts = BuildOptions(output_dir=str(tmp / "out"), work_dir=str(tmp / "work"), **kw)
        return x360_ports.PortPipeline(x360_ports.COD4, str(tmp / "disc"), opts, Log(), threading.Event())

    def test_steps(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            keys = [s.key for s in self.pipe(tmp, build_linux=True, build_windows=True).steps()]
            if IS_LINUX:
                self.assertEqual(keys, ["inputs", "tools", "assets", "sdk", "recompile", "windows", "finish",
                                        "cleanup"])
                keys = [s.key for s in self.pipe(tmp, build_linux=False, build_windows=True).steps()]
                self.assertNotIn("recompile", keys)
                self.assertIn("windows", keys)
            pipe = self.pipe(tmp)
            self.assertEqual(pipe.root, (tmp / "out" / "cod4").resolve())
            self.assertFalse(pipe._needs_rust)
            self.assertFalse(pipe.wsl_linux)
            self.assertEqual(pipe.protected, [(tmp / "disc").resolve(), (tmp / "out").resolve()])

    def test_projects_follow_the_disc(self):
        with tempfile.TemporaryDirectory() as tmp:
            pipe = self.pipe(Path(tmp))
            pipe.programs = list(x360_ports.COD4.programs)
            self.assertEqual([(proj, [q.name for q in progs]) for proj, progs in pipe._projects()],
                             [("", ["cod4"]), ("mp", ["cod4mp"])])
            pipe.programs = [x360_ports.COD4.programs[0]]
            self.assertEqual([proj for proj, _ in pipe._projects()], [""])
            self.assertTrue(any(a.startswith("-DCOD3_RECOMP_SRC=") for a in pipe._extra_args()))

    def test_no_disc_is_refused_before_anything_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            pipe = self.pipe(Path(tmp))
            with self.assertRaises(SetupError) as e:
                pipe.preflight()
            self.assertIn("disc image", str(e.exception))

    def test_cod3_options_are_left_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            opts = BuildOptions(cod3_source="x.iso", waw_steam="steam", appimage=True, upscale_textures=True,
                                output_dir=str(tmp / "out"), work_dir=str(tmp / "work"))
            pipe = x360_ports.PortPipeline(x360_ports.WAW, str(tmp / "disc"), opts, Log(), threading.Event())
            self.assertEqual((pipe.o.cod3_source, pipe.o.waw_steam, pipe.o.appimage, pipe.o.upscale_textures),
                             ("", "", False, False))
            self.assertIsNone(pipe.cod3_src)


if __name__ == "__main__":
    unittest.main()

"""Tests for the Big Red One tab's logic (c3w/bro.py, [acct2-bro]): the disc check (a folder, a disc image, the
default.xbe it holds), where things go, xboxrecomp's step commands, what the recompiled C depends on, the Play
page's choices and the environment they become, installing and finding the built game, and the project's own
tables. No Tkinter, no network, nothing built.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import hashlib
import json
import os
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import bro, profiles  # noqa: E402
from c3w.common import Log, SetupError  # noqa: E402

PROJECT = Path(__file__).resolve().parent.parent.parent


def fake_xbe(title: str = "Call of Duty 2: Big Red One", title_id: int = 0x41560017, version: int = 3) -> bytes:
    """An XBE's first page: the XBEH header, the base address, the certificate's address, the certificate."""
    base, cert_off = 0x10000, 0x180
    data = bytearray(0x1000)
    data[:4] = b"XBEH"
    struct.pack_into("<I", data, 0x104, base)
    struct.pack_into("<I", data, 0x118, base + cert_off)
    struct.pack_into("<I", data, cert_off + 0x08, title_id)
    name = title.encode("utf-16-le")[:80]
    data[cert_off + 0x0C:cert_off + 0x0C + len(name)] = name
    struct.pack_into("<I", data, cert_off + 0xAC, version)
    return bytes(data)


def fake_xiso(path: Path, files: dict) -> None:
    """A trimmed XISO (XDVDFS at offset 0) holding `files` ({name: bytes}) in its root folder."""
    sector = 2048
    names = sorted(files)
    entries = bytearray()
    first_file = 34                     # the volume descriptor is sector 32, the root table sector 33
    offsets, next_sector = [], first_file
    for i, name in enumerate(names):
        offsets.append(len(entries))
        size = len(files[name])
        right = 0
        entry = struct.pack("<HHIIBB", 0, right, next_sector, size, 0x20, len(name)) + name.encode("latin-1")
        entry += b"\0" * (-len(entry) % 4)
        entries += entry
        next_sector += -(-size // sector) or 1
    # Chain the entries through their right child (offsets in 4-byte units).
    for i in range(len(names) - 1):
        struct.pack_into("<H", entries, offsets[i] + 2, offsets[i + 1] // 4)
    image = bytearray(next_sector * sector)
    vd = 32 * sector
    image[vd:vd + 20] = b"MICROSOFT*XBOX*MEDIA"
    struct.pack_into("<II", image, vd + 20, 33, len(entries))
    image[vd + 0x7EC:vd + 0x7EC + 20] = b"MICROSOFT*XBOX*MEDIA"
    image[33 * sector:33 * sector + len(entries)] = entries
    s = first_file
    for name in names:
        image[s * sector:s * sector + len(files[name])] = files[name]
        s += -(-len(files[name]) // sector) or 1
    path.write_bytes(bytes(image))


class XbeTests(unittest.TestCase):
    def test_title_and_version(self):
        info = bro.xbe_info(fake_xbe())
        self.assertEqual((info.title, info.title_id, info.version), ("Call of Duty 2: Big Red One", 0x41560017, 3))

    def test_not_an_xbe(self):
        for data in (b"", b"MZ" + b"\0" * 0x400, b"XBEH" + b"\0" * 0x100):
            with self.assertRaises(SetupError):
                bro.xbe_info(data)
        broken = bytearray(fake_xbe())
        struct.pack_into("<I", broken, 0x118, 0x10000 + 0x10_0000)   # the certificate past the data
        with self.assertRaises(SetupError):
            bro.xbe_info(bytes(broken))


class DiscTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.xbe = fake_xbe()
        self.md5 = hashlib.md5(self.xbe).hexdigest()

    def tearDown(self):
        self.tmp.cleanup()

    def folder(self, xbe: bytes = None, name: str = "default.xbe") -> Path:
        d = self.base / "disc"
        (d / "sp").mkdir(parents=True)
        (d / name).write_bytes(self.xbe if xbe is None else xbe)
        (d / "sp" / "frontend.cod").write_bytes(b"x" * 100)
        return d

    def test_a_known_folder(self):
        d = self.folder()
        with mock.patch.dict(bro.KNOWN_XBE, {self.md5: "the test disc"}):
            disc, line = bro.check_disc(str(d))
            self.assertIsNotNone(disc)
            self.assertEqual((disc.kind, disc.files), ("folder", 2))
            self.assertIn("Found: the test disc (folder, 2 files", line)
            # default.xbe itself, quoted as Windows' "Copy as path" does, names its folder.
            disc, _ = bro.check_disc(f'"{d / "default.xbe"}"')
            self.assertEqual(disc.path, d)

    def test_the_name_case_doesnt_matter(self):
        d = self.folder(name="DEFAULT.XBE")
        with mock.patch.dict(bro.KNOWN_XBE, {self.md5: "the test disc"}):
            self.assertIsNotNone(bro.check_disc(str(d))[0])

    def test_another_version_is_refused_with_its_md5(self):
        disc, line = bro.check_disc(str(self.folder()))
        self.assertIsNone(disc)
        self.assertIn(self.md5, line)
        self.assertIn("isn't the version the port was made from", line)

    def test_another_game_is_named(self):
        disc, line = bro.check_disc(str(self.folder(fake_xbe("Burnout 3: Takedown"))))
        self.assertIsNone(disc)
        self.assertIn("This is Burnout 3: Takedown, not Call of Duty 2: Big Red One", line)

    def test_nothing_or_the_wrong_folder(self):
        self.assertIn("Choose your", bro.check_disc("  ")[1])
        self.assertIn("isn't there", bro.check_disc(str(self.base / "nope"))[1])
        empty = self.base / "empty"
        empty.mkdir()
        self.assertIn("has no default.xbe", bro.check_disc(str(empty))[1])
        junk = self.base / "junk.iso"
        junk.write_bytes(b"\0" * 300000)
        self.assertIn("not an Xbox", bro.check_disc(str(junk))[1])

    def test_a_disc_image(self):
        iso = self.base / "bro.iso"
        fake_xiso(iso, {"default.xbe": self.xbe, "readme.txt": b"hello"})
        with mock.patch.dict(bro.KNOWN_XBE, {self.md5: "the test disc"}):
            disc, line = bro.check_disc(str(iso))
        self.assertIsNotNone(disc, line)
        self.assertEqual((disc.kind, disc.files, disc.xbe_md5), ("iso", 2, self.md5))
        self.assertIn("disc image", line)
        other = self.base / "other.iso"
        fake_xiso(other, {"readme.txt": b"hello"})
        self.assertIn("without default.xbe", bro.check_disc(str(other))[1])

    def test_copying_the_files_goes_on_where_it_stopped(self):
        d = self.folder()
        lay = bro.layout(self.base / "out", self.base / "work")
        with mock.patch.dict(bro.KNOWN_XBE, {self.md5: "the test disc"}):
            disc = bro.check_disc(str(d))[0]
            log = Log()
            bro.copy_game(disc, lay, log, threading.Event())
            self.assertEqual((lay.game / "sp" / "frontend.cod").read_bytes(), b"x" * 100)
            self.assertEqual(lay.xbe.read_bytes(), self.xbe)
            (lay.game / "sp" / "frontend.cod").write_bytes(b"y" * 100)   # same size: kept
            bro.copy_game(disc, lay, log, threading.Event())
            self.assertEqual((lay.game / "sp" / "frontend.cod").read_bytes(), b"y" * 100)
            # The game's own folder as the disc: nothing to copy.
            disc2 = bro.check_disc(str(lay.game))[0]
            bro.copy_game(disc2, lay, log, threading.Event())
            # A disc folder inside the game folder (or around it) is refused.
            inner = bro.Disc("folder", lay.game / "sp", disc.xbe_md5, disc.info, 1, 1)
            with self.assertRaises(SetupError):
                bro.copy_game(inner, lay, log, threading.Event())


class LayoutTests(unittest.TestCase):
    def test_where_things_go(self):
        lay = bro.layout(Path("/o"), Path("/w"), repo=Path("/p"))
        self.assertEqual(lay.out, Path("/o/bro"))
        self.assertEqual(lay.game, Path("/o/bro/game"))
        self.assertEqual(lay.saves, Path("/o/bro/saves"))
        self.assertEqual(lay.exe.parent, Path("/o/bro/bin"))
        self.assertEqual(lay.work, Path("/w/bro"))
        self.assertEqual(lay.xboxrecomp_cache, Path(f"/w/tools/xboxrecomp-{bro.XBOXRECOMP_VERSION}"))
        self.assertEqual(lay.python_env, Path("/w/tools/bro-python"))
        self.assertEqual(lay.xbe, Path("/w/bro/game_files/default.xbe"))
        self.assertEqual(lay.seeds, Path("/p/bro/config/seed_functions.json"))

    def test_the_build_is_linux_only_for_now(self):
        ok, why = bro.supported()
        self.assertEqual(ok, sys.platform.startswith("linux"))
        self.assertEqual(why == "", ok)
        with mock.patch.object(bro, "IS_LINUX", False):
            ok, why = bro.supported()
            self.assertFalse(ok)
            self.assertIn("Linux for now", why)


class StepTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name) / "repo"
        (self.repo / "bro" / "config").mkdir(parents=True)
        (self.repo / "bro" / "patches").mkdir()
        (self.repo / "bro" / "config" / "stand_in_functions.tsv").write_text(
            "address\tname\tret\n0x00012340\tnglFoo\t0\n\n# a note\n0x00056780\tnglBar\t4\n", encoding="utf-8")
        (self.repo / "bro" / "config" / "seed_functions.json").write_text('[{"start": "0x00042C40"}]')
        (self.repo / "bro" / "patches" / "b.patch").write_text("b")
        (self.repo / "bro" / "patches" / "a.patch").write_text("a")
        self.lay = bro.layout(Path(self.tmp.name) / "out", Path(self.tmp.name) / "work", repo=self.repo)

    def tearDown(self):
        self.tmp.cleanup()

    def test_manual_functions_are_the_stand_ins_addresses(self):
        self.assertEqual(bro.manual_functions(self.lay.stand_ins), ["0x00012340", "0x00056780"])

    def test_patches_in_name_order(self):
        self.assertEqual([p.name for p in self.lay.patches], ["a.patch", "b.patch"])

    def test_step_commands(self):
        py = Path("/w/tools/bro-python/bin/python")
        disasm = bro.step_command("disasm", self.lay, py)
        self.assertEqual(disasm[1:3], ["-m", "tools.disasm"])
        self.assertEqual(disasm[disasm.index("--seed-functions") + 1], str(self.lay.work / "seed_functions.json"))
        recomp = bro.step_command("recomp", self.lay, py)
        for flag in ("--all", "--manual-functions", "--gen-dir"):
            self.assertIn(flag, recomp)
        self.assertEqual(recomp[recomp.index("--gen-dir") + 1], str(self.lay.gen))
        parser = bro.step_command("xbe_parser", self.lay, py)
        self.assertEqual(parser[parser.index("--json") + 1], str(self.lay.xbe.with_name("default_analysis.json")))
        self.assertEqual([k for k, _, _ in bro.STEPS], ["xbe_parser", "disasm", "func_id", "abi_analysis", "recomp"])
        self.assertAlmostEqual(sum(share for _, _, share in bro.STEPS), 1.0)
        with self.assertRaises(ValueError):
            bro.step_command("nope", self.lay, py)

    def test_what_the_c_depends_on(self):
        key = bro.recompile_key(self.lay, "abc")
        self.assertEqual(key, bro.recompile_key(self.lay, "abc"))
        self.assertNotEqual(key, bro.recompile_key(self.lay, "abd"))
        (self.repo / "bro" / "patches" / "a.patch").write_text("changed")
        self.assertNotEqual(key, bro.recompile_key(self.lay, "abc"))
        self.assertEqual(key["xboxrecomp"], bro.XBOXRECOMP_COMMIT)

    def test_cmake_commands(self):
        tools = bro.Tools(cmake=Path("/usr/bin/cmake"), ninja=Path("/usr/bin/ninja"))
        cfg = bro.configure_command(self.lay, tools)
        self.assertEqual(cfg[:3], [str(Path("/usr/bin/cmake")), "-G", "Ninja"])
        self.assertIn(f"-DXBOXRECOMP_DIR={self.lay.xboxrecomp}", cfg)
        self.assertIn(f"-DBRO_GEN_DIR={self.lay.gen}", cfg)
        self.assertEqual(cfg[cfg.index("-S") + 1], str(self.repo / "bro"))
        self.assertNotIn("-G", bro.configure_command(self.lay, bro.Tools(cmake=Path("cmake"))))
        self.assertEqual(bro.build_command(self.lay, tools, 0)[-2:], ["-j", "1"])
        # SDL3: Call of Duty 3's SDK's when it is in the work folder, else the system's (no flag)
        self.assertFalse([a for a in cfg if a.startswith("-DSDL3_DIR=")])
        tools.sdl3 = self.lay.sdk_sdl3
        self.assertIn(f"-DSDL3_DIR={self.lay.sdk_sdl3}", bro.configure_command(self.lay, tools))
        self.assertEqual(self.lay.sdk_sdl3.parts[-7:-4], ("rexglue-sdk", "out", "install"))
        self.assertEqual(self.lay.sdk_sdl3.parents[6], self.lay.tools.parent)

    def test_what_is_missing(self):
        t = bro.Tools(git=Path("git"), cmake=Path("cmake"), cmake_version=(3, 28, 3), cc=Path("cc"),
                      glslc=Path("glslc"), pkgconfig=Path("pkg-config"), system_sdl3=True)
        self.assertEqual(t.problems(), [])
        t.system_sdl3 = False
        self.assertIn("SDL3", " ".join(t.problems()))
        t.sdl3 = Path("sdk/lib/cmake/SDL3")
        self.assertEqual(t.problems(), [])
        t.cmake_version = (3, 16)
        t.missing_modules = ["sdl2"]
        t.venv = False
        problems = " ".join(t.problems())
        for word in ("cmake >= 3.20", "SDL2", "venv"):
            self.assertIn(word, problems)

    def test_package_script_holds_only_the_list(self):
        for pm, wanted in bro.LINUX_PACKAGES.items():
            script = bro.install_script(pm, wanted)
            self.assertIn(" ".join(wanted), script)
            self.assertTrue(script.startswith("#!/bin/sh"))
            item = bro.packages_install(pm, wanted)
            self.assertEqual(item.key, "bro-system-packages")
            self.assertIn(pm, item.where)

    def test_one_approval_list(self):
        class Quiet:
            @staticmethod
            def capture(cmd, **kw):
                return 1, ""   # nothing installed: no git answer, no Python with capstone
        ready = bro.Tools(git=Path("git"), cmake=Path("cmake"), cmake_version=(3, 28), cc=Path("cc"),
                          glslc=Path("glslc"), pkgconfig=Path("pkg-config"), system_sdl3=True)
        items = bro.downloads(self.lay, ready, Quiet(), True)
        self.assertEqual([i.key for i in items], ["xboxrecomp", "bro-capstone"])
        missing = bro.Tools()
        with mock.patch.object(bro.deps, "_linux_package_manager", return_value="apt"), \
                mock.patch.object(bro, "IS_LINUX", True):
            keys = [i.key for i in bro.downloads(self.lay, missing, Quiet(), True)]
            self.assertEqual(keys, ["bro-system-packages", "xboxrecomp", "bro-capstone"])
            keys = [i.key for i in bro.downloads(self.lay, missing, Quiet(), False)]
            self.assertEqual(keys, ["xboxrecomp", "bro-capstone"])   # installing off: no packages

    def test_downloads_say_where_they_go(self):
        x, c = bro.xboxrecomp_install(self.lay), bro.capstone_install(self.lay)
        self.assertEqual((x.source, x.where), ("github.com", str(self.lay.xboxrecomp_cache)))
        self.assertEqual((c.source, c.where), ("pypi.org", str(self.lay.python_env)))
        self.assertRegex(bro.XBOXRECOMP_COMMIT, r"^[0-9a-f]{40}$")
        self.assertRegex(bro.CAPSTONE, r"^capstone==\d+\.\d+\.\d+$")


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name) / "out"
        self.lay = bro.layout(self.out, Path(self.tmp.name) / "work")
        self.built = Path(self.tmp.name) / "bro"
        self.built.write_bytes(b"\x7fELF program")
        self.disc = bro.Disc("folder", Path(self.tmp.name), "abc", bro.XbeInfo(1, "t", 1), 1, 1)

    def tearDown(self):
        self.tmp.cleanup()

    def test_install_then_find_and_start(self):
        self.assertIsNone(bro.installed(self.out))
        self.assertIsNone(bro.play_command(self.out)[0])
        bro.install(self.lay, self.built, self.disc, "9.9.9")
        exe = bro.installed(self.out)
        self.assertEqual(exe, self.lay.exe)
        self.assertEqual(exe.read_bytes(), b"\x7fELF program")
        m = json.loads(self.lay.manifest.read_text())
        self.assertEqual((m["tool_version"], m["xbe_md5"], m["xboxrecomp"]), ("9.9.9", "abc", bro.XBOXRECOMP_COMMIT))
        cmd, why = bro.play_command(self.out)
        self.assertEqual((cmd, why), ([str(exe), str(self.lay.game), str(self.lay.saves)], ""))
        self.assertTrue(self.lay.saves.is_dir())
        if os.name != "nt":
            self.assertTrue(os.access(exe, os.X_OK))
            text = self.lay.launcher.read_text()
            self.assertIn('exec "$here/bin/bro" "$here/game" "$here/saves"', text)
            self.assertIn("BRO_VK=${BRO_VK:-1}", text)

    def test_a_manifest_cant_point_outside(self):
        self.lay.out.mkdir(parents=True)
        self.lay.manifest.write_text(json.dumps({"exe_linux": "../../etc/passwd", "exe_windows": "../x.exe"}))
        self.assertIsNone(bro.installed(self.out))
        self.lay.manifest.write_text("not json")
        self.assertIsNone(bro.installed(self.out))


class PlayTests(unittest.TestCase):
    def test_defaults_and_round_trip(self):
        o = bro.PlayOptions()
        self.assertEqual(o.problems(), [])
        o2 = bro.PlayOptions(level="gela", fullscreen=True, vsync=False, size="1920x1080", mouse_look=False,
                             mouse_speed=2400, mute=True)
        self.assertEqual(bro.PlayOptions.from_settings(o2.to_settings()), o2)

    def test_bad_saved_values_fall_back(self):
        o = bro.PlayOptions.from_settings({"bro_level": "nope", "bro_size": "big", "bro_mouse_speed": 99999,
                                           "bro_fullscreen": 1, "bro_vsync": "no", "bro_mute": True})
        self.assertEqual((o.level, o.size, o.mouse_speed), ("", "", bro.DEFAULT_MOUSE_SPEED))
        self.assertEqual((o.fullscreen, o.vsync, o.mute), (False, True, True))   # 1 isn't a yes, "no" isn't a bool
        self.assertEqual(bro.PlayOptions.from_settings({"bro_mouse_speed": True}).mouse_speed,
                         bro.DEFAULT_MOUSE_SPEED)

    def test_problems(self):
        self.assertTrue(bro.PlayOptions(level="mars").problems())
        self.assertTrue(bro.PlayOptions(size="1920*1080").problems())
        self.assertTrue(bro.PlayOptions(mouse_speed=5).problems())

    def test_the_environment(self):
        base = {"PATH": "/bin", "BRO_DEVMAP": "oran", "BRO_FULLSCREEN": "1"}
        env = bro.play_env(bro.PlayOptions(), base)
        self.assertEqual(env["BRO_VK"], "1")                    # drawn with Vulkan
        for gone in ("BRO_DEVMAP", "BRO_FULLSCREEN", "BRO_VSYNC", "BRO_MOUSE_LOOK", "BRO_SOUND_MUTE"):
            self.assertNotIn(gone, env)                         # nothing left over from the caller's
        self.assertEqual(env["PATH"], "/bin")
        env = bro.play_env(bro.PlayOptions(level="omaha_beach", fullscreen=True, vsync=False, size="1600x1200",
                                           mouse_look=False, mouse_speed=800, mute=True), base)
        self.assertEqual({k: env[k] for k in env if k.startswith("BRO_")},
                         {"BRO_VK": "1", "BRO_DEVMAP": "omaha_beach", "BRO_FULLSCREEN": "1", "BRO_VSYNC": "0",
                          "BRO_VK_SIZE": "1600x1200", "BRO_MOUSE_LOOK": "0", "BRO_MOUSE_SPEED": "800",
                          "BRO_SOUND_MUTE": "1"})

    def test_levels(self):
        keys = [k for k, _ in bro.LEVELS]
        self.assertEqual(keys[0], "")                           # the menus first
        self.assertEqual(len(keys), len(set(keys)))
        self.assertIn("omaha_beach", keys)

    def test_profiles_keep_the_choices(self):
        notes = []
        clean = profiles.clean_settings({"bro_source": "/discs/bro.iso", "bro_level": "gela", "bro_size": "1920x1080",
                                         "bro_mouse_speed": 800, "bro_mute": True}, notes)
        self.assertEqual(clean["bro_level"], "gela")
        self.assertEqual(notes, [])
        bad = profiles.clean_settings({"bro_level": "mars", "bro_size": "7x7", "bro_mouse_speed": 1}, notes)
        self.assertEqual(bad, {})
        self.assertIn("bro_source", profiles.PATH_KEYS)


class UnfinishedBuildTests(unittest.TestCase):
    """A build that stopped (the owner's of 2026-11-06, closed during the disassembly): Play says where it stopped,
    and the long steps keep telling the user they are working."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name) / "out"
        self.lay = bro.layout(self.out, Path(self.tmp.name) / "work")

    def tearDown(self):
        self.tmp.cleanup()

    def _log(self, *lines):
        self.lay.logs.mkdir(parents=True)
        (self.lay.logs / "build.log").write_text("\n".join(lines) + "\n", encoding="utf-8")

    def test_no_build_yet(self):
        self.assertEqual(bro.last_build(self.out), "")
        self.assertIn("choose your disc", bro.play_command(self.out)[1])

    def test_closed_during_the_disassembly(self):
        self._log("12:50:19 [step] Recompiling default.xbe", "12:50:19 [step] Disassembling (the long step: about an hour)",
                  "12:51:05 [out] Phase 5: Detecting functions...")
        cmd, why = bro.play_command(self.out)
        self.assertIsNone(cmd)
        self.assertIn('stopped at "Disassembling (the long step: about an hour)"', why)
        self.assertIn("without an error", why)
        self.assertIn("Press Build again", why)

    def test_stopped_by_an_error(self):
        self._log("10:00:00 [step] Building", "10:05:00 [error] The build found no SDL3")
        why = bro.play_command(self.out)[1]
        self.assertIn('stopped at "Building"', why)
        self.assertIn("The build found no SDL3", why)

    def test_the_clock_moves_the_bar_and_says_it_is_working(self):
        seen = []
        log = Log(sinks=[lambda kind, text, frac: seen.append((kind, text, frac))])
        with bro._StepClock(log, "Disassembling", 0.01, 0.80, every=0.05) as clock:
            handle = bro._progress_lines(log, "Disassembling", clock.where)
            deadline = time.monotonic() + 5
            while not seen and time.monotonic() < deadline:
                time.sleep(0.02)
            handle("Phase 5: Detecting functions...")
        ticks = [s for s in seen if "still working" in s[1]]
        self.assertTrue(ticks)
        self.assertTrue(all(s[0] == "progress" and 0.01 < s[2] <= 0.01 + 0.80 * 0.95 for s in ticks))
        lines = [s for s in seen if "Phase 5" in s[1]]
        self.assertGreater(lines[0][2], 0.01)   # the bar where the clock moved it, not the step's start

    def test_a_plain_number_still_works(self):
        seen = []
        log = Log(sinks=[lambda kind, text, frac: seen.append(frac)])
        bro._progress_lines(log, "Compiling", 0.0)("[1/10] Building C object")
        self.assertEqual(seen, [0.0])


@unittest.skipUnless((PROJECT / "bro" / "CMakeLists.txt").is_file(), "the port's sources aren't in this copy")
class ProjectTests(unittest.TestCase):
    def test_the_port_is_next_to_the_tool(self):
        bro.check_project(bro.layout(Path("/o"), Path("/w"), repo=PROJECT))

    def test_seeds_and_stand_ins(self):
        lay = bro.layout(Path("/o"), Path("/w"), repo=PROJECT)
        seeds = json.loads(lay.seeds.read_text(encoding="utf-8"))
        self.assertTrue(seeds)
        for s in seeds:
            self.assertRegex(s["start"], r"^0x[0-9A-Fa-f]{8}$")
        addresses = bro.manual_functions(lay.stand_ins)
        self.assertTrue(addresses)
        for a in addresses:
            self.assertRegex(a, r"^(0x)?[0-9A-Fa-f]{8}$")
        self.assertEqual(len(addresses), len(set(addresses)))

    def test_patches_are_patches(self):
        lay = bro.layout(Path("/o"), Path("/w"), repo=PROJECT)
        self.assertTrue(lay.patches)
        for p in lay.patches:
            self.assertTrue(p.read_text(encoding="utf-8").startswith("diff --git a/src/"), p.name)


if __name__ == "__main__":
    unittest.main()

"""Tests for checking and repairing a finished build (c3w/repair.py) and the
pipeline's support for it (BuildOptions.only_steps, the rip layout record,
the game files listed in cod3_waw.json). A made-up output folder stands in
for a real build; nothing is built, ripped or run.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import dataclasses
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import binaries, pipeline, repair, rippers, upscale  # noqa: E402
from c3w.common import Log, exe_name  # noqa: E402
from c3w.pipeline import LEVELS, RECOMP_FILES, TOOL_VERSION, BuildOptions, Pipeline  # noqa: E402

C3W = Path(repair.__file__).resolve().parent
KEYS_HEADER = "# key\twidth\theight\txenos_format\tname\tfile\tkind\n"
INDEX_HEADER = "# key\twidth\theight\txenos_format\tfile\n"


def write(path: Path, data=b"x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        path.write_text(data, encoding="utf-8")
    else:
        path.write_bytes(data)
    return path


def make_build(out: Path) -> None:
    """A complete output folder: both versions, the disc's files, a rip of
    the current layout, an HD pack with its pictures, the ripper and the
    launchers."""
    appimage = write(out / "CoD3-x86_64.AppImage", b"\x7fELF")
    with open(appimage, "r+b") as f:
        f.truncate(2 * 1024 * 1024)
    win = ["bin-windows/cod3.exe", "bin-windows/librexgpu-xenos.dll", "bin-windows/libc++.dll"] + \
        [f"bin-windows/cod3_{lvl}.dll" for lvl in LEVELS]
    for rel in win:
        write(out / rel, b"MZ" + b"\0" * 64)
    for rel in RECOMP_FILES:
        write(out / "assets" / rel, b"data")
    data = out / "data" / "cod3"
    write(data / "textures" / "wall.png")
    write(data / "textures" / upscale.KEYS_FILE, KEYS_HEADER + "00000000000000aa\t64\t64\t18\twall\twall\tdiffuse\n")
    rippers.record_layout(data)
    write(data / upscale.OUTPUT_NAME / "wall.png")
    write(data / upscale.PACK_NAME / upscale.PACK_INDEX, INDEX_HEADER + "00000000000000aa\t64\t64\t18\twall.dds\n")
    write(data / upscale.PACK_NAME / "wall.dds", b"DDS " + b"\0" * 200)
    write(out / "tools" / exe_name("rip"), b"\x7fELF rip")
    write(out / "tools" / "rip", b"\x7fELF rip")
    write(out / "launcher" / "cod3_waw_setup.py", "# launcher\n")
    for p in C3W.glob("*.py"):
        write(out / "launcher" / "c3w" / p.name, "# copy\n")
    write(out / "launcher" / "c3w" / "pipeline.py", f'TOOL_VERSION = "{TOOL_VERSION}"\n')
    write(out / "Launcher.sh", "#!/bin/sh\n")
    write(out / "Launcher.bat", "@echo off\r\n")
    write(out / "cod3_waw.json", json.dumps({
        "tool_version": TOOL_VERSION, "platform": "linux-amd64",
        "cod3_exe": "CoD3-x86_64.AppImage", "cod3_exe_linux": "CoD3-x86_64.AppImage",
        "cod3_exe_windows": "bin-windows/cod3.exe",
        "cod3_files_linux": ["CoD3-x86_64.AppImage"], "cod3_files_windows": win,
        "cod3_data": "data/cod3", "cod3_textures_upscaled": f"data/cod3/{upscale.OUTPUT_NAME}",
        "cod3_hd_textures": f"data/cod3/{upscale.PACK_NAME}", "cod3_user_textures": None,
        "waw_source": {"kind": "none", "path": None}, "waw_data": None, "ripper": "tools/rip",
    }))


class RepairCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.out = Path(self._tmp.name) / "out"
        make_build(self.out)

    def tearDown(self):
        self._tmp.cleanup()

    def manifest(self, **changes) -> None:
        path = self.out / "cod3_waw.json"
        m = json.loads(path.read_text(encoding="utf-8"))
        m.update(changes)
        path.write_text(json.dumps(m), encoding="utf-8")

    def issues(self):
        return repair.check_install(self.out).issues

    def only(self, fix: str, area: str):
        issues = self.issues()
        self.assertTrue(issues, "no issue found")
        self.assertEqual({(i.fix, i.area) for i in issues}, {(fix, area)}, issues)
        return issues


class CheckTests(RepairCase):
    def test_a_complete_build_is_fine(self):
        report = repair.check_install(self.out)
        self.assertEqual(report.issues, [])
        self.assertTrue(report.ok)
        self.assertEqual(report.summary(), "No problems found.")
        self.assertEqual(repair.repair_overrides(report), {})

    def test_no_build(self):
        (self.out / "cod3_waw.json").unlink()
        report = repair.check_install(self.out)
        self.assertFalse(report.ok)
        self.assertIsNone(report.issues[0].fix)
        self.assertEqual(repair.repair_overrides(report), {})
        self.assertFalse(repair.check_install(self.out / "nothing").ok)
        (self.out / "cod3_waw.json").write_text("[1, 2]", encoding="utf-8")
        self.assertFalse(repair.check_install(self.out).ok)

    def test_missing_hd_texture(self):
        (self.out / "data" / "cod3" / upscale.PACK_NAME / "wall.dds").unlink()
        issue = self.only("repack_hd", "hd")[0]
        self.assertEqual(issue.severity, "error")
        self.assertIn("wall.dds", issue.detail)

    def test_hd_texture_that_isnt_a_dds(self):
        write(self.out / "data" / "cod3" / upscale.PACK_NAME / "wall.dds", b"\0" * 300)
        self.only("repack_hd", "hd")

    def test_hd_pack_without_its_pictures_cant_be_repacked(self):
        (self.out / "data" / "cod3" / upscale.PACK_NAME / upscale.PACK_INDEX).unlink()
        (self.out / "data" / "cod3" / upscale.OUTPUT_NAME / "wall.png").unlink()
        fixes = {(i.fix, i.area) for i in self.issues()}
        self.assertEqual(fixes, {(None, "hd")})

    def test_hd_index_naming_files_elsewhere(self):
        write(self.out / "data" / "cod3" / upscale.PACK_NAME / upscale.PACK_INDEX,
              INDEX_HEADER + "00000000000000aa\t64\t64\t18\t../../../etc/passwd\n")
        issues = self.issues()
        self.assertTrue(any("outside" in i.title for i in issues), issues)

    def test_windows_file_missing(self):
        (self.out / "bin-windows" / "cod3_saint_lo.dll").unlink()
        issue = self.only("rebuild_windows", "windows")[0]
        self.assertIn("cod3_saint_lo.dll", issue.detail)

    def test_windows_exe_damaged(self):
        write(self.out / "bin-windows" / "cod3.exe", b"")
        self.only("rebuild_windows", "windows")

    def test_appimage_cut_off(self):
        write(self.out / "CoD3-x86_64.AppImage", b"\x7fELF" * 10)
        self.only("rebuild_game", "linux")

    def test_windows_without_a_file_list_reads_the_imports(self):
        self.manifest(cod3_files_windows=None)
        imports = {"cod3.exe": ["KERNEL32.dll", "libc++.dll", "cod3_blkbrn.dll", "SDL3.dll"]}
        with mock.patch.object(binaries, "needed", lambda p: imports.get(Path(p).name, [])):
            issues = self.issues()
        self.assertEqual([(i.fix, i.area) for i in issues], [("rebuild_windows", "windows")])
        self.assertIn("SDL3.dll", issues[0].detail)
        self.assertNotIn("KERNEL32", issues[0].detail)

    def test_windows_without_a_file_list_checks_levels_and_plugin(self):
        self.manifest(cod3_files_windows=None)
        (self.out / "bin-windows" / "cod3_island.dll").unlink()
        (self.out / "bin-windows" / "librexgpu-xenos.dll").unlink()
        with mock.patch.object(binaries, "needed", lambda p: []):
            titles = [i.title for i in self.issues()]
        self.assertEqual(len(titles), 2, titles)

    def test_disc_files_missing(self):
        (self.out / "assets" / "default.xex").unlink()
        self.only("rerip", "assets")
        report = repair.check_install(self.out)
        overrides = repair.repair_overrides(report)
        self.assertNotIn("cod3_source", overrides)   # the disc is needed
        self.assertIn("assets", overrides["only_steps"])

    def test_extraction_that_didnt_finish(self):
        write(self.out / "assets" / "sp" / "island" / "island.cod.part")
        self.only("rerip", "assets")

    def test_rip_from_an_older_ripper(self):
        rippers.write_json(self.out / "data" / "cod3" / rippers.LAYOUT_FILE, {"output_layout": 5})
        issue = self.only("rerip", "data")[0]
        self.assertEqual(issue.severity, "warning")

    def test_rip_layout_guessed_without_the_record(self):
        data = self.out / "data" / "cod3"
        (data / rippers.LAYOUT_FILE).unlink()
        self.assertEqual(self.issues(), [])                 # 7 columns: layout 6
        write(data / "textures" / upscale.KEYS_FILE, "# key\n00000000000000aa\t64\t64\t18\twall\twall\n")
        if rippers.RIP_OUTPUT_VERSION > 4:
            self.only("rerip", "data")
        (data / "textures" / upscale.KEYS_FILE).unlink()
        self.only("rerip", "data")
        self.assertEqual(repair.guess_layout(data / "nothing"), None)

    def test_ripped_files_missing(self):
        import shutil
        shutil.rmtree(self.out / "data" / "cod3" / "textures")
        self.assertIn(("rerip", "data"), {(i.fix, i.area) for i in self.issues()})

    def test_ripper_missing(self):
        (self.out / "tools" / "rip").unlink()
        if (self.out / "tools" / exe_name("rip")).exists():
            (self.out / "tools" / exe_name("rip")).unlink()
        self.only("rerip", "ripper")
        overrides = repair.repair_overrides(repair.check_install(self.out))
        self.assertEqual(overrides["only_steps"], ["ripper"])
        self.assertFalse(overrides["build_recomp"])

    def test_world_at_war_missing(self):
        self.manifest(waw_data="data/waw")
        self.only("rerip", "waw")
        write(self.out / "data" / "waw" / "mp_airfield" / "x.txt")
        self.assertEqual(self.issues(), [])

    def test_launchers(self):
        (self.out / "Launcher.sh").unlink()
        (self.out / "Launcher.bat").unlink()
        issues = self.only("relaunchers", "launcher")
        self.assertEqual(sorted(i.severity for i in issues), ["error", "warning"])
        overrides = repair.repair_overrides(repair.check_install(self.out))
        self.assertEqual(overrides["only_steps"], ["finish"])

    def test_launcher_from_another_version(self):
        write(self.out / "launcher" / "c3w" / "pipeline.py", 'TOOL_VERSION = "0.1"\n')
        self.assertEqual(self.only("relaunchers", "launcher")[0].severity, "warning")
        (self.out / "launcher" / "c3w" / "repair.py").unlink()
        self.only("relaunchers", "launcher")
        import shutil
        shutil.rmtree(self.out / "launcher")
        self.assertEqual(self.only("relaunchers", "launcher")[0].severity, "error")

    def test_record_naming_places_outside_is_never_followed(self):
        for bad in ("../../etc/passwd", "/etc/passwd", "C:\\Windows\\notepad.exe", "..\\x", 5):
            self.manifest(cod3_exe_linux=bad, cod3_files_linux=None)
            issues = self.issues()
            self.assertTrue(any(i.area == "record" and i.fix is None for i in issues), (bad, issues))
            self.assertFalse(any(i.area == "linux" for i in issues), (bad, issues))

    def test_record_naming_a_missing_file(self):
        self.manifest(cod3_exe_linux="bin/cod3", cod3_files_linux=["bin/cod3"])
        self.only("rebuild_game", "linux")


class OverrideTests(RepairCase):
    def test_hd_repack_runs_only_the_pack(self):
        (self.out / "data" / "cod3" / upscale.PACK_NAME / "wall.dds").unlink()
        o = repair.repair_overrides(repair.check_install(self.out))
        self.assertEqual(o["only_steps"], ["hd_pack"])
        self.assertEqual(o["output_dir"], str(self.out.resolve()))
        self.assertFalse(o["build_recomp"])
        self.assertNotIn("cod3_source", o)                   # no disc needed

    def test_game_rebuild_uses_the_files_in_the_output(self):
        (self.out / "bin-windows" / "cod3_saint_lo.dll").unlink()
        o = repair.repair_overrides(repair.check_install(self.out))
        self.assertTrue(o["build_recomp"])
        self.assertTrue(o["build_windows"])
        self.assertFalse(o["build_linux"])
        self.assertIn("windows", o["only_steps"])
        self.assertEqual(Path(o["cod3_source"]), self.out.resolve() / "assets")

    def test_a_new_rip_also_packs_again(self):
        rippers.write_json(self.out / "data" / "cod3" / rippers.LAYOUT_FILE, {"output_layout": 1})
        o = repair.repair_overrides(repair.check_install(self.out))
        self.assertEqual(o["only_steps"], ["hd_pack", "rip_cod3", "user_pack"])

    def test_overrides_fit_build_options(self):
        (self.out / "Launcher.sh").unlink()
        (self.out / "bin-windows" / "cod3_saint_lo.dll").unlink()
        o = repair.repair_overrides(repair.check_install(self.out))
        opts = dataclasses.replace(BuildOptions(), **o)
        self.assertEqual(opts.only_steps, o["only_steps"])
        self.assertEqual(repair.check_install(self.out).fixes, ["rebuild_windows", "relaunchers"])


def pipeline_for(tmp: Path, host: str = "linux", **opts) -> Pipeline:
    o = BuildOptions(output_dir=str(tmp / "out"), work_dir=str(tmp / "work"), **opts)
    with mock.patch.object(pipeline, "IS_LINUX", host == "linux"), \
            mock.patch.object(pipeline, "IS_WINDOWS", host == "windows"):
        return Pipeline(o, Log(sinks=[]), threading.Event())


def titles(host: str = "linux", **opts) -> list:
    with tempfile.TemporaryDirectory() as tmp, \
            mock.patch.object(pipeline, "IS_LINUX", host == "linux"), \
            mock.patch.object(pipeline, "IS_WINDOWS", host == "windows"):
        return [s.title for s in pipeline_for(Path(tmp), host, **opts).steps()]


class OnlyStepsTests(unittest.TestCase):
    def test_a_normal_build_runs_every_step(self):
        t = titles(build_linux=True, build_windows=True, upscale_textures=True)
        self.assertEqual(t, ["Checking inputs", "Tools", "Call of Duty 3 game files", "Ripper",
                             "Ripping Call of Duty 3", "Upscaling textures (waifu2x)", "HD texture pack",
                             "User texture pack", "World at War", "ReXGlue SDK", "Recompiling Call of Duty 3",
                             "Packaging", "Windows version (cod3.exe)", "Finishing", "Cleaning up"])

    def test_every_step_has_a_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = pipeline_for(Path(tmp), build_linux=True, build_windows=True, upscale_textures=True)
            keys = [s.key for s in p.steps()]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertTrue(all(keys))

    def test_hd_repack_without_upscaling(self):
        t = titles(only_steps=["hd_pack"], build_recomp=False)
        self.assertEqual(t, ["Checking inputs", "Tools", "Ripper", "HD texture pack", "Finishing", "Cleaning up"])

    def test_launchers_only_need_no_tools(self):
        self.assertEqual(titles(only_steps=["finish"], build_recomp=False),
                         ["Checking inputs", "Finishing", "Cleaning up"])

    def test_windows_rebuild_on_linux(self):
        t = titles(only_steps=["sdk", "recompile", "package", "windows"], build_linux=False, build_windows=True)
        self.assertEqual(t, ["Checking inputs", "Tools", "Call of Duty 3 game files", "ReXGlue SDK",
                             "Translating Call of Duty 3's code", "Windows version (cod3.exe)", "Finishing",
                             "Cleaning up"])

    def test_linux_rebuild_on_windows_uses_wsl(self):
        t = titles("windows", only_steps=["sdk", "recompile", "package", "linux_wsl"], build_linux=True,
                   build_windows=False)
        self.assertEqual(t, ["Checking inputs", "Tools", "Call of Duty 3 game files",
                             "Linux version (AppImage, built in WSL)", "Finishing", "Cleaning up"])

    def test_preflight_of_a_repair_needs_no_disc(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = pipeline_for(Path(tmp), only_steps=["finish"], build_recomp=False)
            p.preflight()                                    # no ISO chosen, nothing to rip: fine
            normal = pipeline_for(Path(tmp), build_recomp=False)
            with self.assertRaises(pipeline.SetupError):
                normal.preflight()

    def test_a_repair_reuses_the_ripper_in_the_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = pipeline_for(Path(tmp), only_steps=["hd_pack"], build_recomp=False)
            rip = write(Path(tmp) / "out" / "tools" / exe_name("rip"), b"\x7fELF")
            with mock.patch.object(rippers, "works", return_value=True) as works:
                self.assertFalse(p._needs_rust)
                p.ripper()                                   # no toolchain needed
                self.assertEqual(p.rip, rip)
                self.assertEqual(works.call_count, 1)        # asked once
            normal = pipeline_for(Path(tmp), build_recomp=False)
            self.assertIsNone(normal._reusable_rip())
            self.assertTrue(normal._needs_rust)

    def test_finish_keeps_what_a_repair_didnt_redo(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            write(out / "cod3_waw.json", json.dumps({
                "waw_source": {"kind": "steam", "path": "S"}, "cod3_exe_windows": "bin-windows/cod3.exe",
                "cod3_files_windows": ["bin-windows/cod3.exe"], "cod3_exe_linux": "CoD3-x86_64.AppImage",
                "cod3_files_linux": ["CoD3-x86_64.AppImage"]}))
            p = pipeline_for(Path(tmp), only_steps=["finish"], build_recomp=False)
            p.finish()
            m = json.loads((out / "cod3_waw.json").read_text(encoding="utf-8"))
            self.assertEqual(m["waw_source"], {"kind": "steam", "path": "S"})
            self.assertEqual(m["cod3_files_windows"], ["bin-windows/cod3.exe"])
            self.assertEqual(m["cod3_files_linux"], ["CoD3-x86_64.AppImage"])
            self.assertTrue((out / "launcher" / "c3w" / "repair.py").is_file())
            # A full build records its own source and the files it packed.
            p = pipeline_for(Path(tmp), build_recomp=False)
            p.packed["windows"] = ["bin-windows/cod3.exe", "bin-windows/x.dll"]
            p.linux_exe_rel = "CoD3-x86_64.AppImage"          # rebuilt inside WSL, without a list
            p.finish()
            m = json.loads((out / "cod3_waw.json").read_text(encoding="utf-8"))
            self.assertEqual(m["waw_source"]["kind"], "none")
            self.assertEqual(m["cod3_files_windows"], ["bin-windows/cod3.exe", "bin-windows/x.dll"])
            self.assertIsNone(m["cod3_files_linux"])

    def test_rip_layout_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.assertIsNone(rippers.recorded_layout(d))
            rippers.record_layout(d)
            self.assertEqual(rippers.recorded_layout(d), rippers.RIP_OUTPUT_VERSION)
            (d / rippers.LAYOUT_FILE).write_text('{"output_layout": true}', encoding="utf-8")
            self.assertIsNone(rippers.recorded_layout(d))
            (d / rippers.LAYOUT_FILE).write_text("[6]", encoding="utf-8")
            self.assertIsNone(rippers.recorded_layout(d))

    def test_a_rip_writes_the_layout_record(self):
        class Runner:
            def run(self, cmd, **kw):
                return 0
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "data"
            rippers.rip_cod3(Path("rip"), Path(tmp) / "sp", out, 2, Runner(), Log(sinks=[]))
            self.assertEqual(rippers.recorded_layout(out), rippers.RIP_OUTPUT_VERSION)


if __name__ == "__main__":
    unittest.main()

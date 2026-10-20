"""Tests for settings profiles (c3w/profiles.py): names that make safe file
names, only known settings with the right types, rename and delete, and
export / import without personal paths. Everything happens in temporary
folders.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import profiles  # noqa: E402
from c3w.common import SetupError  # noqa: E402


def settings(project: Path) -> dict:
    """A settings dict like gui.App._save writes."""
    return {
        "cod3_source": str(project / "input" / "cod3.iso"), "waw_steam": "", "waw_xbox360": "",
        "waw_pc_disc": "", "waw_alternative": "xbox360", "waw_map_packs": [str(project / "input" / "patch.exe")],
        "use_waw": False, "build_linux": True, "build_windows": False,
        "output_dir": str(project / "output"), "work_dir": str(project / "work"), "jobs": 4,
        "build_recomp": True, "appimage": True, "install_deps": True, "fidelityfx_sdk": False, "cleanup": True,
        "keep_sdk": False, "upscale_textures": True, "upscale_model": "photo", "upscale_noise": 1,
        "upscale_scale": 2, "cod3_windowed": False, "cod3_args": "--fullscreen=false",
    }


class ProfileTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.project = root / "project"
        self.config = self.project / "config"
        self.config.mkdir(parents=True)
        self.elsewhere = root / "elsewhere"
        patcher = mock.patch.object(profiles.common, "project_dir", return_value=self.project)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        self._tmp.cleanup()


class NameTests(ProfileTestCase):
    def test_plain_names_are_kept(self):
        for name in ("Fast build", "Linux + Windows", "4K (HD textures)", "Ünïcode name"):
            self.assertEqual(profiles.check_name(f"  {name} "), name)

    def test_unsafe_names_are_refused(self):
        bad = ["", "   ", "a/b", "a\\b", "..", ".hidden", "name.", "a:b", 'a"b', "a*b", "a?b", "a<b", "a|b",
               "tab\there", "x" * (profiles.MAX_NAME + 1), "CON", "con.txt", "Nul", "COM1", "lpt9.json", "aux "]
        for name in bad:
            with self.assertRaises(SetupError, msg=repr(name)):
                profiles.check_name(name)
        with self.assertRaises(SetupError):
            profiles.check_name(None)

    def test_a_name_never_leaves_the_profiles_folder(self):
        for name in ("../escape", "..\\escape", "/abs", "C:\\abs"):
            with self.assertRaises(SetupError):
                profiles.save_profile(self.config, name, {"jobs": 2})
        self.assertEqual(list(self.config.parent.glob("*.json")), [])


class SaveLoadTests(ProfileTestCase):
    def test_round_trip(self):
        s = settings(self.project)
        profiles.save_profile(self.config, "Main", s)
        self.assertTrue((self.config / "profiles" / "Main.json").is_file())
        self.assertEqual(profiles.list_profiles(self.config), ["Main"])
        self.assertEqual(profiles.load_profile(self.config, "Main"), s)
        self.assertEqual(profiles.load_profile(self.config, "main"), s)   # any capitals

    def test_only_known_settings_with_the_right_types(self):
        notes = []
        profiles.save_profile(self.config, "P", {
            "jobs": "4",                 # text, not a number
            "use_waw": 1,                # a number, not true/false
            "cleanup": True,
            "build_recomp": None,
            "upscale_scale": 3,          # not one of 1, 2, 4
            "upscale_noise": True,       # true/false isn't a number here
            "waw_alternative": "floppy",
            "waw_map_packs": ["ok.exe", 5],
            "cod3_args": "line\nbreak",
            "evil": "rm -rf /",
        }, notes=notes)
        self.assertEqual(profiles.load_profile(self.config, "P"), {"cleanup": True})
        self.assertTrue(any("evil" in n for n in notes))
        self.assertTrue(any("jobs" in n for n in notes))

    def test_jobs_range(self):
        clean = profiles.clean_settings({"jobs": 0})
        self.assertEqual(clean, {})
        self.assertEqual(profiles.clean_settings({"jobs": 16}), {"jobs": 16})

    def test_list_is_sorted_and_ignores_other_files(self):
        for n in ("beta", "Alpha", "gamma"):
            profiles.save_profile(self.config, n, {"jobs": 2})
        (self.config / "profiles" / "notes.txt").write_text("x")
        (self.config / "profiles" / "CON.json").write_text("{}")
        self.assertEqual(profiles.list_profiles(self.config), ["Alpha", "beta", "gamma"])
        self.assertEqual(profiles.list_profiles(self.config / "missing"), [])

    def test_saving_with_other_capitals_replaces(self):
        profiles.save_profile(self.config, "fast", {"jobs": 2})
        profiles.save_profile(self.config, "Fast", {"jobs": 8})
        self.assertEqual(profiles.list_profiles(self.config), ["Fast"])
        self.assertEqual(profiles.load_profile(self.config, "fast"), {"jobs": 8})

    def test_missing_and_damaged_profiles(self):
        with self.assertRaises(SetupError):
            profiles.load_profile(self.config, "nope")
        (self.config / "profiles").mkdir()
        (self.config / "profiles" / "bad.json").write_text("{not json", encoding="utf-8")
        with self.assertRaises(SetupError):
            profiles.load_profile(self.config, "bad")
        (self.config / "profiles" / "list.json").write_text("[1, 2]", encoding="utf-8")
        with self.assertRaises(SetupError):
            profiles.load_profile(self.config, "list")


class RenameDeleteTests(ProfileTestCase):
    def test_rename(self):
        profiles.save_profile(self.config, "old", {"jobs": 3})
        profiles.rename_profile(self.config, "old", "new")
        self.assertEqual(profiles.list_profiles(self.config), ["new"])
        self.assertEqual(profiles.load_profile(self.config, "new"), {"jobs": 3})
        data = json.loads((self.config / "profiles" / "new.json").read_text(encoding="utf-8"))
        self.assertEqual(data["name"], "new")

    def test_rename_only_capitals(self):
        profiles.save_profile(self.config, "fast", {"jobs": 3})
        profiles.rename_profile(self.config, "fast", "Fast")
        self.assertEqual(profiles.list_profiles(self.config), ["Fast"])

    def test_rename_refuses_existing_and_missing(self):
        profiles.save_profile(self.config, "a", {"jobs": 1})
        profiles.save_profile(self.config, "b", {"jobs": 2})
        with self.assertRaises(SetupError):
            profiles.rename_profile(self.config, "a", "B")
        with self.assertRaises(SetupError):
            profiles.rename_profile(self.config, "zzz", "c")
        with self.assertRaises(SetupError):
            profiles.rename_profile(self.config, "a", "../c")
        self.assertEqual(profiles.load_profile(self.config, "a"), {"jobs": 1})

    def test_delete(self):
        profiles.save_profile(self.config, "gone", {"jobs": 1})
        profiles.delete_profile(self.config, "GONE")
        self.assertEqual(profiles.list_profiles(self.config), [])
        with self.assertRaises(SetupError):
            profiles.delete_profile(self.config, "gone")


class ExportImportTests(ProfileTestCase):
    def test_export_leaves_out_paths_outside_the_project(self):
        s = settings(self.project)
        s["waw_steam"] = str(self.elsewhere / "Steam" / "World at War")
        s["waw_map_packs"].append(str(self.elsewhere / "patch2.exe"))
        profiles.save_profile(self.config, "Share", s)
        dest = self.elsewhere / "share.json"
        notes = profiles.export_profile(self.config, "Share", dest)
        text = dest.read_text(encoding="utf-8")
        self.assertNotIn(str(self.elsewhere), text)
        self.assertNotIn(str(self.project), text)
        data = json.loads(text)
        self.assertEqual(data["format"], profiles.FORMAT)
        exported = data["settings"]
        self.assertEqual(exported["cod3_source"], "input/cod3.iso")
        self.assertEqual(exported["output_dir"], "output")
        self.assertEqual(exported["waw_map_packs"], ["input/patch.exe"])
        self.assertNotIn("waw_steam", exported)
        self.assertEqual(exported["waw_xbox360"], "")
        self.assertTrue(any("waw_steam" in n for n in notes))

    def test_import_brings_paths_back_into_this_project(self):
        profiles.save_profile(self.config, "Share", settings(self.project))
        dest = self.elsewhere / "share.json"
        profiles.export_profile(self.config, "Share", dest)
        name = profiles.import_profile(self.config, dest)
        self.assertEqual(name, "Share (2)")     # the name was taken
        loaded = profiles.load_profile(self.config, name)
        self.assertEqual(Path(loaded["cod3_source"]), self.project / "input" / "cod3.iso")
        self.assertEqual(Path(loaded["output_dir"]), self.project / "output")
        self.assertEqual(loaded["jobs"], 4)

    def test_import_with_a_name_replaces_it(self):
        profiles.save_profile(self.config, "Mine", {"jobs": 1})
        src = self.elsewhere / "x.json"
        src.parent.mkdir(parents=True)
        src.write_text(json.dumps({"format": profiles.FORMAT, "settings": {"jobs": 6}}), encoding="utf-8")
        self.assertEqual(profiles.import_profile(self.config, src, "Mine"), "Mine")
        self.assertEqual(profiles.load_profile(self.config, "Mine"), {"jobs": 6})

    def test_import_a_plain_settings_file(self):
        src = self.elsewhere / "settings.json"
        src.parent.mkdir(parents=True)
        src.write_text(json.dumps({"jobs": 2, "theme": "dark", "cleanup": False}), encoding="utf-8")
        notes = []
        name = profiles.import_profile(self.config, src, notes=notes)
        self.assertEqual(name, "settings")
        self.assertEqual(profiles.load_profile(self.config, name), {"jobs": 2, "cleanup": False})
        self.assertTrue(any("theme" in n for n in notes))

    def test_import_refuses_relative_paths_that_leave_the_project(self):
        src = self.elsewhere / "p.json"
        src.parent.mkdir(parents=True)
        src.write_text(json.dumps({"settings": {}, "output_dir": "../../outside", "work_dir": "C:work",
                                   "cod3_source": "input/../../x.iso", "jobs": 2}), encoding="utf-8")
        name = profiles.import_profile(self.config, src)
        self.assertEqual(profiles.load_profile(self.config, name), {"jobs": 2})

    def test_import_refuses_bad_files(self):
        self.elsewhere.mkdir()
        cases = {"big.json": "{" + " " * (profiles.MAX_FILE + 10) + "}", "bad.json": "not json",
                 "list.json": "[]", "empty.json": "{}", "unknown.json": '{"x": 1}',
                 "deep.json": "[" * 100000 + "]" * 100000}
        for file_name, text in cases.items():
            (self.elsewhere / file_name).write_text(text, encoding="utf-8")
            with self.assertRaises(SetupError, msg=file_name):
                profiles.import_profile(self.config, self.elsewhere / file_name)
        (self.elsewhere / "binary.json").write_bytes(b"\xff\xfe\x00junk")
        with self.assertRaises(SetupError):
            profiles.import_profile(self.config, self.elsewhere / "binary.json")
        with self.assertRaises(SetupError):
            profiles.import_profile(self.config, self.elsewhere / "missing.json")
        self.assertEqual(profiles.list_profiles(self.config), [])

    def test_import_name_from_a_bad_file_name(self):
        self.elsewhere.mkdir()
        src = self.elsewhere / "CON.json"
        src.write_text(json.dumps({"jobs": 2}), encoding="utf-8")
        self.assertEqual(profiles.import_profile(self.config, src), "Imported")


if __name__ == "__main__":
    unittest.main()

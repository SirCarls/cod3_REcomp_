"""Tests for the log tools (c3w/logtools.py): reading the game's and the
setup tool's logs, filtering, finding them in an output folder, the game's
last session, and the bug report with personal details replaced. Made-up
logs in temporary folders; this PC's home, user and host names are made up
too.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import logtools  # noqa: E402

# Made-up people's folders, built from pieces so the repository's personal-path check
# (tools/check_repo.py) never mistakes them for real ones.
HOME_DIR = "/" + "home/"
MEDIA_DIR = "/" + "media/"
USERS_DIR = "Us" + "ers"

GAME = [
    "[2026-10-12 18:47:34.807] [info] [core] [t100] cod3: ---- session ---- build: x; Linux; 24 CPU threads",
    "[2026-10-12 18:47:35.730] [warning] [krnl] [t101] [NtCreateFile] FAILED: path='d:\\_english\\'",
    "[2026-10-12 18:47:41.534] [info] [core] [t100] cod3: window closed; exiting",
    "[2026-10-12 18:48:38.596] [info] [gpu] [t376] DXGI adapter: Some GPU",
    "[2026-10-12 18:48:39.071] [info] [gpu] [t376] Direct3D 12 device and OS features:",
    "* Tiled resources: tier 4",
    "* Resource binding: tier 3",
    "[2026-10-12 18:48:39.199] [info] [core] [t376] cod3: ---- session ---- build: y; Windows; 24 CPU threads",
    "[2026-10-12 18:49:54.416] [error] [core] [t548] cod3: something broke",
    "[2026-10-12 18:49:55.000] [critical] [core] [t548] cod3: very broken",
]
SETUP = [
    "18:10:43 [step] [1/9] Checking inputs",
    "18:10:44 [info] Game: Call of Duty 3 (USA, Europe)",
    "18:10:45 [error] Still missing:",
    "  - Rust",
    "18:10:46 [out]     Compiling lzo v0.1.0",
    "18:10:47 [progress] SDK 3/840",
    "18:10:48 [warn] careful",
]


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class ParseTests(unittest.TestCase):
    def test_game_lines(self):
        e = logtools.parse_lines(GAME)
        self.assertEqual(len(e), 8)                         # the "*" lines continue an entry
        self.assertEqual(e[0].time, "2026-10-12 18:47:34.807")
        self.assertEqual((e[1].level, e[1].source), ("warn", "krnl"))
        self.assertTrue(e[1].text.startswith("[NtCreateFile]"))
        self.assertEqual(e[4].text, "Direct3D 12 device and OS features:\n* Tiled resources: tier 4\n"
                                    "* Resource binding: tier 3")
        self.assertIn("* Resource binding", e[4].raw)
        self.assertEqual(e[7].level, "critical")

    def test_setup_lines(self):
        e = logtools.parse_lines(SETUP)
        self.assertEqual([x.level for x in e], ["step", "info", "error", "out", "progress", "warn"])
        self.assertEqual(e[2].text, "Still missing:\n  - Rust")
        self.assertEqual(e[0].source, "")
        self.assertEqual(e[0].time, "18:10:43")

    def test_lines_before_any_entry(self):
        e = logtools.parse_lines(["no prefix here", "", "[x] odd"])
        self.assertEqual([x.text for x in e], ["no prefix here\n[x] odd"])

    def test_read_only_the_end_of_a_big_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            lines = [f"18:00:{i % 60:02d} [info] line {i}" for i in range(50000)]
            path = write(Path(tmp) / "big.log", "\n".join(lines) + "\n")
            e = logtools.read_entries(path, max_lines=100)
            self.assertEqual(len(e), 100)
            self.assertEqual(e[-1].text, "line 49999")
            self.assertEqual(e[0].text, "line 49900")
            self.assertEqual(logtools.read_entries(Path(tmp) / "missing.log"), [])
            crlf = write(Path(tmp) / "crlf.log", "")
            crlf.write_bytes(b"18:00:00 [info] a\r\n18:00:01 [warn] b\r\n")
            self.assertEqual([x.text for x in logtools.read_entries(crlf)], ["a", "b"])

    def test_filter(self):
        e = logtools.parse_lines(GAME + SETUP)
        self.assertEqual(len(logtools.filter_entries(e, set(), "")), len(e))
        errors = logtools.filter_entries(e, {"error"}, "")
        self.assertEqual([x.level for x in errors], ["error", "critical", "error"])
        self.assertEqual(len(logtools.filter_entries(e, {"warning"}, "")), 2)
        found = logtools.filter_entries(e, set(), "SESSION")
        self.assertEqual(len(found), 2)
        self.assertEqual(logtools.filter_entries(e, {"info"}, "tiled")[0].source, "gpu")


class OutputFolderTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.out = Path(self._tmp.name) / "out"
        logs = self.out / "userdata" / "logs"
        write(logs / "cod3.log", "\n".join(GAME[7:]) + "\n")
        write(logs / "cod3.1.log", "\n".join(GAME[:7]) + "\n")
        write(logs / "cod3.2.log", "old\n")
        write(logs / "report-20261012-184559.txt", "report new\n")
        write(logs / "report-20261011-120108.txt", "report old\n")
        write(self.out / "logs" / "setup-20261012-180045.log", "\n".join(SETUP) + "\n")
        write(self.out / "logs" / "setup-20261010-030902.log", "18:00:00 [info] older\n")

    def tearDown(self):
        self._tmp.cleanup()

    def test_known_logs(self):
        labels = [label for label, _ in logtools.known_logs(self.out)]
        self.assertEqual(labels, ["Game log", "Game report 2026-10-12 18:45", "Game report 2026-10-11 12:01",
                                  "Setup log 2026-10-12 18:00", "Setup log 2026-10-10 03:09",
                                  "Game log, older part 1", "Game log, older part 2"])
        self.assertEqual(logtools.known_logs(Path(self._tmp.name) / "nothing"), [])

    def test_last_session_starts_with_its_start_up_lines(self):
        session = logtools.last_session(self.out)
        self.assertEqual(session[0], GAME[3])               # after the last run's "exiting"
        self.assertEqual(session[-1], GAME[-1])
        self.assertIn("* Tiled resources: tier 4", session)
        self.assertNotIn(GAME[2], session)

    def test_last_session_without_start_up_lines_or_header(self):
        write(self.out / "userdata" / "logs" / "cod3.log", "\n".join(GAME[:3]) + "\n")
        self.assertEqual(logtools.last_session(self.out), GAME[:3])
        write(self.out / "userdata" / "logs" / "cod3.log", "no session here\n")
        for p in self.out.glob("userdata/logs/cod3.*.log"):
            p.unlink()
        self.assertEqual(logtools.last_session(self.out), ["no session here"])

    def test_cap_keeps_start_and_end(self):
        text = "".join(f"line {i}\n" for i in range(100000))
        capped = logtools._cap(text, 20000)
        self.assertLess(len(capped), 21000)
        self.assertTrue(capped.startswith("line 0\n"))
        self.assertTrue(capped.endswith("line 99999\n"))
        self.assertIn("lines left out", capped)


class ScrubTests(unittest.TestCase):
    def test_known_and_unknown_folders_and_names(self):
        s = logtools.Scrubber([("/mnt/Big Disk/COD3", "<OUTPUT>"), (HOME_DIR + "alice", "<HOME>")], ["alice"], ["alicepc"])
        cases = {
            "out /mnt/Big Disk/COD3/userdata/logs": "out <OUTPUT>/userdata/logs",
            "wine Z:\\mnt\\Big Disk\\COD3\\cod3.toml": "wine <OUTPUT>\\cod3.toml",
            "json \"Z:\\\\mnt\\\\Big Disk\\\\COD3\"": "json \"<OUTPUT>\"",
            "Z:/mnt/Big Disk/COD3/data/cod3/hd_textures": "<OUTPUT>/data/cod3/hd_textures",
            HOME_DIR + "alice/.cache/x": "<HOME>/.cache/x",
            HOME_DIR + "alicette/x": "<HOME>/x",                  # someone else's home folder
            "user Alice on ALICEPC": "user <USER> on <HOST>",
            "C:\\" + USERS_DIR + "\\Bob Smith\\AppData then bob smith": "<HOME>\\AppData then <USER>",
            MEDIA_DIR + "carol/Disk/x": "/media/<USER>/Disk/x",
            "/other/place/COD3/cod3_waw.json and /other/place/COD3 alone": "<OUTPUT>/cod3_waw.json and <OUTPUT> alone",
            "/srv/x/COD_3_WAW_Setup-tool/c3w": "<PROJECT>/COD_3_WAW_Setup-tool/c3w",
            "SDK 0.10.0.0-dev and 1.2.3.4-rc": "SDK 0.10.0.0-dev and 1.2.3.4-rc",
            "peer 192.168.1.20. and 10.0.0.5:28960, local 127.0.0.1": "peer <IP>. and <IP>:28960, local 127.0.0.1",
            "/alicex/ stays, info stays": "/alicex/ stays, info stays",
        }
        for text, expected in cases.items():
            self.assertEqual(s(text), expected, text)

    def test_names_learned_from_paths_hide_cut_off_paths(self):
        s = logtools.Scrubber()
        s.learn("Work folder " + HOME_DIR + "dave/work")
        self.assertEqual(s("progress  ome/dave/.cache/x"), "progress  ome/<USER>/.cache/x")

    def test_short_and_common_names_are_left(self):
        s = logtools.Scrubber([], ["al", "root", "user"], ["localhost"])
        self.assertEqual(s("al root user localhost"), "al root user localhost")


class BugReportTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.home = root / "home" / "zelda"
        self.out = self.home / "Games" / "COD3"
        self.project = root / "proj" / "cod3"
        iso = self.home / "Downloads" / "Call of Duty 3.iso"
        waw = root / "steam" / "World at War"
        win_out = "Z:" + str(self.out).replace("/", "\\")
        logs = self.out / "userdata" / "logs"
        write(logs / "cod3.log", "\n".join([
            "[2026-10-12 18:48:39.199] [info] [core] [t1] cod3: ---- session ---- build: y",
            f"[2026-10-12 18:48:39.300] [info] [core] [t1] cod3: HD textures on: the pack in {win_out}\\data\\cod3",
            f"[2026-10-12 18:48:39.301] [info] [core] [t1] cod3: user zelda on zelda-desktop at 192.168.0.12",
            f"[2026-10-12 18:48:39.302] [info] [core] [t1] cod3: config {self.home}/.config/x",
        ]) + "\n")
        write(logs / "report-20261012-184559.txt", f"report from {self.out}\n")
        write(self.out / "logs" / "setup-20261012-180045.log",
              f"18:00:45 [info] Extracting {iso} into {self.out}/assets\n"
              f"18:00:46 [info] World at War source: {waw}\n"
              f"18:00:47 [info] Project {self.project}/COD_3_WAW_Setup-tool\n")
        write(self.out / "cod3.toml", f'log_file = "{win_out.replace(chr(92), chr(92) * 2)}\\\\userdata"\n')
        write(self.out / "cod3_waw.json", json.dumps({"waw_source": {"kind": "steam", "path": str(waw)},
                                                      "cod3_exe_linux": "CoD3-x86_64.AppImage"}))
        write(self.out / "assets" / "default.xex", "GAME FILE")
        write(self.out / "data" / "cod3" / "hd_textures" / "x.dds", "GAME FILE")
        write(self.out / "userdata" / "save.dat", "SAVE")
        self.settings = write(root / "config" / "settings.json", json.dumps({
            "cod3_source": str(iso), "waw_steam": str(waw), "output_dir": str(self.out),
            "work_dir": str(self.project / "work"), "waw_map_packs": [str(self.home / "patch.exe")]}))
        self.secrets = ["zelda", str(self.home), str(self.out), win_out, str(iso), str(waw), str(self.project),
                        "192.168.0.12", "Zelda-Desktop"]
        patches = [mock.patch.object(logtools, "_identity",
                                     return_value=([str(self.home)], ["zelda"], ["zelda-desktop"])),
                   mock.patch.object(logtools.common, "project_dir", return_value=self.project)]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self._tmp.cleanup()

    def test_report_has_the_logs_without_personal_details(self):
        dest = Path(self._tmp.name) / "report" / "bug.zip"
        path, names = logtools.make_bug_report(self.out, dest, settings_file=self.settings)
        self.assertEqual(path, dest)
        self.assertEqual(names, ["README.txt", "setup.log", "game-last-session.log", "game-report.txt", "cod3.toml",
                                 "cod3_waw.json", "setup-settings.json", "check.txt"])
        with zipfile.ZipFile(dest) as z:
            self.assertEqual(z.namelist(), names)
            texts = {n: z.read(n).decode("utf-8") for n in names}
        everything = "\n".join(texts.values())
        for secret in self.secrets:
            self.assertNotIn(secret.lower(), everything.lower(), secret)
        self.assertNotIn("GAME FILE", everything)
        self.assertNotIn("SAVE", everything)
        for placeholder in ("<HOME>", "<USER>", "<HOST>", "<OUTPUT>", "<ISO>", "<WAW>", "<PROJECT>", "<IP>"):
            self.assertIn(placeholder, everything, placeholder)
        self.assertIn("<OUTPUT>\\data\\cod3", texts["game-last-session.log"])
        self.assertIn("---- session ----", texts["game-last-session.log"])
        json.loads(texts["setup-settings.json"])             # still valid JSON
        json.loads(texts["cod3_waw.json"])

    def test_report_of_an_empty_folder(self):
        empty = Path(self._tmp.name) / "empty"
        empty.mkdir()
        path, names = logtools.make_bug_report(empty, Path(self._tmp.name) / "e.zip",
                                               settings_file=Path(self._tmp.name) / "none.json")
        self.assertEqual(names, ["README.txt", "check.txt"])
        self.assertTrue(zipfile.is_zipfile(path))


if __name__ == "__main__":
    unittest.main()

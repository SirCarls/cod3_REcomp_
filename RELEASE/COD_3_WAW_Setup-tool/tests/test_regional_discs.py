"""Tests for the regional Call of Duty 3 discs (docs/X360_FAMILY.md): the
German and the French/Spanish/Italian discs carry the USA/Europe campaign
program byte for byte, so the setup tool takes them, says which one it is,
and tells the French/Spanish/Italian disc's owner to pick the console
language (Game settings > Console language, the SDK's user_language).

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOL = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOL))

from c3w import game_settings as gs  # noqa: E402
from c3w import inputs, pipeline  # noqa: E402
from c3w.common import SetupError  # noqa: E402

USA = "681724c40e250e584e6585dd903e05afa00a8634"
GERMANY = "52447cb4189bc4168e71324255e9e7acf5495eb8"
FR_ES_IT = "6f9247bb74c4006632a8556dcd1d41332139ff83"


class _Log:
    def __init__(self):
        self.lines = []

    def info(self, msg):
        self.lines.append(("info", msg))

    def warn(self, msg):
        self.lines.append(("warn", msg))


def _check(sha, force=False):
    """Pipeline._check_cod3_source on a disc whose default.xex has this SHA-1."""
    p = pipeline.Pipeline.__new__(pipeline.Pipeline)
    p.log = _Log()
    p.o = mock.Mock(force_unknown_iso=force, build_recomp=False)
    with mock.patch.object(pipeline.Pipeline, "_cod3_xex_sha1", return_value=sha):
        p._check_cod3_source()
    return p.log.lines


class RegionalDiscTests(unittest.TestCase):
    def test_the_three_discs_are_known(self):
        self.assertEqual(set(pipeline.KNOWN_XEX), {USA, GERMANY, FR_ES_IT})
        self.assertTrue(pipeline.XEX_PICKS_LANGUAGE <= set(pipeline.KNOWN_XEX))
        self.assertEqual(pipeline.XEX_PICKS_LANGUAGE, {FR_ES_IT})
        for sha, name in pipeline.KNOWN_XEX.items():
            self.assertRegex(sha, r"^[0-9a-f]{40}$")
            self.assertTrue(name.startswith("Call of Duty 3 ("), name)

    def test_each_disc_is_named_in_the_log(self):
        for sha, name in pipeline.KNOWN_XEX.items():
            lines = _check(sha)
            self.assertIn(("info", f"Game: {name} (default.xex {sha[:12]}...)"), lines)

    def test_only_the_fr_es_it_disc_asks_for_the_console_language(self):
        for sha in (USA, GERMANY):
            self.assertFalse(any("Console language" in m for _, m in _check(sha)), sha)
        self.assertTrue(any("Console language" in m for _, m in _check(FR_ES_IT)))

    def test_an_unknown_disc_lists_the_known_ones(self):
        with self.assertRaises(SetupError) as e:
            _check("0" * 40)
        for name in pipeline.KNOWN_XEX.values():
            self.assertIn(name, str(e.exception))
        lines = _check("0" * 40, force=True)
        self.assertEqual(lines[0][0], "warn")

    def test_the_multiplayer_program_stays_usa_only(self):
        # The regional multiplayer programs are rebuilds with other addresses.
        self.assertEqual(len(pipeline.KNOWN_MP_XEX), 1)

    def test_the_input_folder_finds_a_regional_disc(self):
        with tempfile.TemporaryDirectory() as tmp:
            iso = Path(tmp) / "cod3_de.iso"
            iso.write_bytes(b"")
            with mock.patch.object(inputs, "_disc_kind",
                                   side_effect=lambda p, known: "cod3" if GERMANY in set(known) else None):
                found = inputs.scan(Path(tmp), pipeline.KNOWN_XEX)
            self.assertEqual(found.cod3_iso, iso)


class ConsoleLanguageSettingTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.out = Path(self._tmp.name)
        patcher = mock.patch.object(gs, "game_running", return_value=False)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        self._tmp.cleanup()

    def test_the_setting(self):
        st = gs.BY_KEY["user_language"]
        self.assertEqual(st.default, 1)   # the SDK's default: English
        # Xbox 360 language numbers (XGetLanguage): 1 en, 3 de, 4 fr, 5 es, 6 it.
        self.assertEqual([v for v, _ in st.choices], [1, 3, 4, 5, 6])
        gs.check({"user_language": 4})
        for bad in (2, 0, 7, "4", True, "fr"):
            with self.assertRaises(SetupError, msg=repr(bad)):
                gs.check({"user_language": bad})

    def test_saved_as_a_plain_number_and_read_back(self):
        gs.save(self.out, {"user_language": 5})
        self.assertIn("user_language = 5\n", (self.out / gs.FILE_NAME).read_text(encoding="utf-8"))
        self.assertEqual(gs.load(self.out)["user_language"], 5)
        gs.save(self.out, {"user_language": 1})
        self.assertEqual(gs.load(self.out)["user_language"], 1)

    def test_the_sdk_defines_it(self):
        """The name and default are the SDK's (src/kernel/xam/xam_user.cpp), when a checkout is there."""
        for sdk in (TOOL.parent / "work" / "rexglue-sdk",):   # the setup tool's own checkout
            src = sdk / "src" / "kernel" / "xam" / "xam_user.cpp"
            if src.is_file():
                self.assertIn('REXCVAR_DEFINE_UINT32(user_language, 1,', src.read_text(encoding="utf-8"))
                return
        self.skipTest("no SDK checkout next to the tool")


if __name__ == "__main__":
    unittest.main()

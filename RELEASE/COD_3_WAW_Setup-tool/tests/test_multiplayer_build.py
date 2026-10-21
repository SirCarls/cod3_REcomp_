"""Tests for building the multiplayer program (cod3mp) next to the campaign:
which codmp_xenonf.xex goes into the project, and how the packaged game
starts one program or the other. Nothing is built; the files are made up.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import hashlib
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import pipeline  # noqa: E402
from c3w.common import Log  # noqa: E402
from c3w.pipeline import BuildOptions, Pipeline  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def drop_mp_conditionals(text: str) -> str:
    """Lines inside `#if defined(COD3_MP)` / `#ifdef COD3_MP` / `#if !defined(COD3_MP)`
    blocks (both branches) blanked: they are written for one program on
    purpose (the same rule as tools/mp_aliases.py's, which needs Python 3.11)."""
    out, depth, skip_at = [], 0, None
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#if"):
            depth += 1
            if skip_at is None and "COD3_MP" in s:
                skip_at = depth
                out.append("")
                continue
        elif s.startswith("#endif"):
            if skip_at == depth:
                skip_at = None
                depth -= 1
                out.append("")
                continue
            depth -= 1
        out.append("" if skip_at is not None else line)
    return "\n".join(out)


def make_pipeline(tmp: Path) -> Pipeline:
    o = BuildOptions(output_dir=str(tmp / "out"), work_dir=str(tmp / "work"))
    return Pipeline(o, Log(sinks=[]), threading.Event())


class CopyMpXexTests(unittest.TestCase):
    def test_known_build_goes_into_the_project(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = make_pipeline(Path(tmp))
            data = b"multiplayer executable"
            (p.assets).mkdir(parents=True)
            (p.assets / pipeline.MP_XEX).write_bytes(data)
            known = {hashlib.sha1(data).hexdigest(): "test build"}
            with mock.patch.object(pipeline, "KNOWN_MP_XEX", known):
                self.assertTrue(p._copy_mp_xex())
            self.assertEqual((p.proj / "assets" / pipeline.MP_XEX).read_bytes(), data)

    def test_other_build_is_left_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = make_pipeline(Path(tmp))
            p.assets.mkdir(parents=True)
            (p.assets / pipeline.MP_XEX).write_bytes(b"some other build")
            # A copy from an earlier build must go too, or CMake would still add mp/.
            stale = p.proj / "assets" / pipeline.MP_XEX
            stale.parent.mkdir(parents=True)
            stale.write_bytes(b"old")
            self.assertFalse(p._copy_mp_xex())
            self.assertFalse(stale.exists())

    def test_no_multiplayer_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = make_pipeline(Path(tmp))
            p.assets.mkdir(parents=True)
            self.assertFalse(p._copy_mp_xex())

    def test_the_known_hash_is_the_one_the_hooks_are_for(self):
        # recomp/mp/config/codmp.toml names the build its addresses are from.
        config = (ROOT / "recomp" / "mp" / "config" / "codmp.toml").read_text(encoding="utf-8")
        for sha in pipeline.KNOWN_MP_XEX:
            self.assertIn(sha, config)


class ProjectLayoutTests(unittest.TestCase):
    def test_mp_folder_is_added_only_with_its_executable(self):
        top = (ROOT / "recomp" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/codmp_xenonf.xex"', top)
        self.assertIn("add_subdirectory(mp)", top)
        # Last: mp/'s generated glue redefines the SDK's macros.
        self.assertGreater(top.index("add_subdirectory(mp)"), top.index("rexglue_setup_target(cod3"))

    def test_mp_manifest_reads_the_projects_assets(self):
        manifest = (ROOT / "recomp" / "mp" / "cod3mp_manifest.toml").read_text(encoding="utf-8")
        self.assertIn('file_path = "../assets/codmp_xenonf.xex"', manifest)
        self.assertIn('name = "cod3mp"', manifest)
        self.assertTrue((ROOT / "recomp" / "mp" / "generated" / "rexglue.cmake").is_file())

    def test_shared_sources_hook_no_campaign_address(self):
        # A campaign function name in a file compiled into cod3mp would hook
        # whatever function has that name or address in codmp_xenonf.xex: each
        # one goes through tools/mp_aliases.py (CampaignInMultiplayerTests).
        import re
        cmake = (ROOT / "recomp" / "mp" / "CMakeLists.txt").read_text(encoding="utf-8")
        sources = re.findall(r"\$\{COD3_SRC\}/(\S+\.cpp)", cmake)
        self.assertIn("game/script_threads.cpp", sources)
        self.assertIn("-include${COD3_SRC}/game/mp/mp_aliases.h", cmake)
        # Also without the tool: every sub_ hook in a cod3mp file is one the
        # alias header renames to its MP function.
        aliases = (ROOT / "recomp" / "src" / "game" / "mp" / "mp_aliases.h").read_text(encoding="utf-8")
        renamed = set(re.findall(r"^#define (sub_[0-9A-F]{8}) ", aliases, re.M))
        # [mp-menus] cod3mp's own files hook its own functions by their own
        # names (the same list as tools/mp_aliases.py's MP_OWN).
        mp_own = re.compile(r"^(main_mp\.cpp|cod3mp_app\.(h|cpp)|game/mp_hooks\.cpp|game/mp/.*)$")
        for rel in sources:
            if mp_own.match(rel):
                continue
            # [mp-menus] A file shared by both programs keeps each one's own
            # hooks in #if [!]defined(COD3_MP) blocks (game/pc_menu.cpp,
            # game/button_prompts.cpp): those aren't campaign names in cod3mp.
            text = drop_mp_conditionals((ROOT / "recomp" / "src" / rel).read_text(encoding="utf-8"))
            for name in re.findall(r"REX_(?:HOOK_RAW|EXTERN)\((?:__imp__)?(sub_[0-9A-F]{8})\)", text):
                self.assertIn(name, renamed, f"{rel} hooks {name}, which mp_aliases.h doesn't map")


def load_mp_aliases():
    """tools/mp_aliases.py (next to this setup tool), or None (also on a
    Python older than 3.11, which has no tomllib)."""
    import importlib.util
    path = ROOT.parent / "tools" / "mp_aliases.py"
    if not path.is_file() or importlib.util.find_spec("tomllib") is None:
        return None
    spec = importlib.util.spec_from_file_location("mp_aliases", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CampaignInMultiplayerTests(unittest.TestCase):
    """The campaign's port code compiled into cod3mp (tools/mp_aliases.py,
    recomp/mp/config/campaign_map.tsv). No game is built here."""

    @classmethod
    def setUpClass(cls):
        cls.tool = load_mp_aliases()
        if cls.tool is None:
            raise unittest.SkipTest("tools/mp_aliases.py isn't next to the setup tool (or Python < 3.11)")
        cls.recomp = ROOT / "recomp"
        cls.problems = cls.tool.Problems()
        cls.out = cls.tool.generate(cls.recomp, cls.problems)
        _, cls.rows = cls.tool.read_table(cls.recomp / cls.tool.TABLE)

    def problems_like(self, *words):
        return [p for p in self.problems if any(w in p for w in words)]

    def test_aliases_are_current(self):
        # The four generated files are what the table and the sources give.
        for rel, text in self.out.items():
            path = self.recomp / rel
            self.assertTrue(path.is_file(), f"{rel} is missing: run tools/mp_aliases.py --write")
            self.assertEqual(path.read_text(encoding="utf-8"), text,
                             f"{rel} is out of date: run tools/mp_aliases.py --write")
        # No alias chain, no two campaign names on one MP function, no todo row.
        self.assertEqual(self.problems_like("alias chain", "both become", "not reviewed"), [])

    def test_every_campaign_hook_in_cod3mp_is_mapped_or_marked(self):
        # A new campaign hook in a shared file without a reviewed row fails
        # here, so it can't hook the wrong MP function in silence.
        self.assertEqual(self.problems_like("no row", "unknown status", "needs its reason",
                                            "has its own code", "renames in cod3mp"), [])

    def test_same_names_bind_to_the_mapped_function(self):
        mp_names = self.tool.read_names(self.recomp / "mp" / "config" / "names_mp.toml")
        by_name = {n: a for a, n in mp_names.items()}
        for r in self.rows:
            if r.status != "same" or ":" in r.symbol or not r.mp or r.mp == "=":
                continue
            mp = self.tool.hexval(r.mp)
            if r.symbol in by_name:
                self.assertEqual(by_name[r.symbol], mp, f"{r.symbol}: names_mp.toml has it elsewhere")
        self.assertEqual(self.problems_like("but names_mp.toml", "names_mp.toml has it"), [])

    def test_midasm_hooks_follow(self):
        # Every campaign mid-asm hook a cod3mp file defines has an MP site (or
        # a reason), and the generated table has each MP site once.
        self.assertEqual(self.problems_like("mid-asm hook", "give its MP address", "isn't defined"), [])
        import tomllib
        generated = tomllib.loads(self.out[self.tool.CAMPAIGN_TOML]).get("midasm_hook", [])
        addresses = [h["address"] for h in generated]
        self.assertEqual(len(addresses), len(set(addresses)))
        sites = [r for r in self.rows if r.symbol.startswith("midasm:") and r.status == "mp"]
        self.assertEqual(len(generated), len(sites))
        own = tomllib.loads((self.recomp / "mp" / "config" / "codmp.toml").read_text(encoding="utf-8"))
        own_addresses = {h["address"] for h in own.get("midasm_hook", [])}
        self.assertFalse(own_addresses & set(addresses), "a site is in codmp.toml and codmp_campaign.toml")

    def test_no_raw_campaign_addresses(self):
        # Addresses in shared code are addresses.h constants or COD3_ADDR(sp, mp).
        self.assertEqual(self.problems_like("raw campaign address", "COD3_ADDR("), [])

    def test_force_include_only_on_cod3mp(self):
        import re
        mp_cmake = (self.recomp / "mp" / "CMakeLists.txt").read_text(encoding="utf-8")
        uses = re.findall(r"target_compile_options\((\w+)[^)]*mp_aliases\.h", mp_cmake)
        self.assertTrue(uses)
        self.assertEqual(set(uses), {"cod3mp"})
        top = (self.recomp / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertNotIn("mp_aliases", top)
        self.assertNotIn("game/mp/", top)  # the campaign build has none of cod3mp's own files
        addresses = (self.recomp / "src" / "game" / "addresses.h").read_text(encoding="utf-8")
        self.assertIn('#if defined(COD3_MP)  // [mp]', addresses)
        before, _, after = addresses.partition('#include "game/mp/addresses_mp.h"')
        self.assertTrue(before.rstrip().endswith("(tools/mp_aliases.py)"))
        self.assertTrue(after.lstrip().startswith("#else"))

    def test_the_manifest_includes_the_generated_hooks(self):
        manifest = (self.recomp / "mp" / "cod3mp_manifest.toml").read_text(encoding="utf-8")
        self.assertIn('"config/codmp_campaign.toml"', manifest)


class AppRunTests(unittest.TestCase):
    def test_apprun_starts_the_program_asked_for(self):
        src = (ROOT / "c3w" / "pipeline.py").read_text(encoding="utf-8")
        self.assertIn('"$COD3_PROGRAM" = cod3mp', src)
        relaunch = (ROOT / "recomp" / "src" / "game" / "relaunch.cpp").read_text(encoding="utf-8")
        self.assertIn('"COD3_PROGRAM"', relaunch)


if __name__ == "__main__":
    unittest.main()

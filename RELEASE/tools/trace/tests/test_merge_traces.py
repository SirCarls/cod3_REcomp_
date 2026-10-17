"""Tests of tools/trace/merge_traces.py on small made-up trace sessions.

    python3 -m unittest discover -s tools/trace/tests
"""
import ast
import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("merge_traces", HERE.parent / "merge_traces.py")
merge_traces = importlib.util.module_from_spec(spec)
spec.loader.exec_module(merge_traces)

FUNCTIONS_HEADER = "module\taddress\tname\tfirst_seen_s\tlevel\tstate\tmarker\tcalls\tcallers"
CALLS_HEADER = "callee_module\tcallee_address\tcallee_name\tcaller_module\tcaller_address\tcaller_name\tcall_site"
EVIDENCE_HEADER = ("kind\tmodule\tfunction_address\tfunction_name\tvalue\tcount\tfirst_seen_s\tlevel\tstate"
                   "\tcall_site\tvia")

SYMBOLS = '''# test map
[[function]]
address = 0x82100000
name = "CL_Init"
confidence = "high"
evidence = "adds the client commands"

[[function]]
address = 0x82100800
name = "Taken_Name"
confidence = "high"
evidence = "already named"

[[function]]
address = 0x82100900
name = "Broc_Something"
confidence = "high"
evidence = "level slot 0x1AC (anglestoforward) tail-calls it"
'''


def write_session(root, name, functions, calls=(), evidence=(), markers=(), modules=None):
    folder = Path(root) / name
    folder.mkdir(parents=True)
    (folder / "functions.tsv").write_text("\n".join([FUNCTIONS_HEADER, *functions]) + "\n")
    (folder / "calls.tsv").write_text("\n".join([CALLS_HEADER, *calls]) + "\n")
    (folder / "evidence.tsv").write_text("\n".join([EVIDENCE_HEADER, *evidence]) + "\n")
    (folder / "markers.tsv").write_text(
        "\n".join(["number\tpressed_s\ttime_s\tlevel\tstate\ttext\tfunctions", *markers]) + "\n")
    rows = modules or ["default\t1000\t0\t0\t0", "saint_lo\t500\t0\t0\t0"]
    (folder / "modules.tsv").write_text("\n".join(["module\tfunctions\tseen\tseen_named\tseen_unnamed", *rows]) + "\n")
    (folder / "trace.tsv").write_text("key\tvalue\nformat\t1\nsession\t" + name + "\n")
    return folder


def read(path):
    return merge_traces.read_tsv(Path(path))


class Unescape(unittest.TestCase):
    def test_writer_escapes_undone(self):
        self.assertEqual(merge_traces.unescape("a\\tb\\nc\\\\d\\r"), "a\tb\nc\\d\r")
        self.assertEqual(merge_traces.unescape("plain"), "plain")

    def test_unknown_escape_kept(self):
        self.assertEqual(merge_traces.unescape("x\\qy"), "x\\qy")


class Merging(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        traces = self.root / "decomp_trace"
        write_session(traces, "20261015-100000", [
            "default\t0x82100000\tCL_Init\t1.0\tfront end\tfront end\t\t1\t",
            "default\t0x82100400\tsub_82100400\t2.5\tfront end\tmenu\t1: title screen\t40\tCL_Init+0x10",
            "default\t0x82100500\tsub_82100500\t3.0\tsaint_lo\tloading\t\t7\tCL_Init+0x20",
            "default\t0x82100600\tsub_82100600\t4.0\tsaint_lo\tplaying\t\t3\tsaint_lo!sub_89031000+0x44",
            "default\t0x82100700\tsub_82100700\t5.0\tsaint_lo\tplaying\t\t2\t",
            "default\t0x82100800\tsub_82100800\t5.0\tsaint_lo\tplaying\t\t2\t",
            "saint_lo\t0x89031000\tsub_89031000\t6.0\tsaint_lo\tplaying\t\t9\t",
        ], calls=[
            "default\t0x82100400\tsub_82100400\tdefault\t0x82100000\tCL_Init\t0x82100010",
            "default\t0x82100600\tsub_82100600\tsaint_lo\t0x89031000\tsub_89031000\t0x89031044",
        ], evidence=[
            "command_handler\tdefault\t0x82100400\tsub_82100400\tvid_restart\t1\t1.0\tfront end\tfront end"
            "\t0x82100010\tCL_Init+0x10",
            "print\tdefault\t0x82100500\tsub_82100500\tR_LoadWorld: %s not found\\n\t2\t3.0\tsaint_lo\tloading"
            "\t0x82100520\t",
            "dvar\tdefault\t0x82100500\tsub_82100500\tr_mode\t5\t3.0\tsaint_lo\tloading\t0x82100530\t",
            "level_slot\tdefault\t0x82100600\tsub_82100600\tslot 0x1AC\t1\t4.0\tsaint_lo\tplaying\t\t",
            "print\tdefault\t0x82100800\tsub_82100800\tTaken_Name: oops\t1\t5.0\tsaint_lo\tplaying\t0x82100810\t",
        ], markers=["1\t2.0\t4.0\tfront end\tmenu\ttitle screen\t1"])
        write_session(traces / "older", "20261014-090000", [
            "default\t0x82100400\tsub_82100400\t9.0\tfront end\tmenu\t\t60\tCL_Init+0x10",
            "default\t0x82100700\tsub_82100700\t5.0\tfalaise\tplaying\t\t1\t",
        ])
        (self.root / "symbols.toml").write_text(SYMBOLS)
        # Two pairing tables that agree on one function, one that only one names.
        (self.root / "waw_pairs.tsv").write_text(
            "cod3_address\twaw_name\n0x82100700\tG_RunThink\n0x82100500\tR_Other\n")
        (self.root / "cod2_pairs.tsv").write_text("address\tcod2_name\tnote\n82100700\tG_RunThink\tx\n")
        self.out = self.root / "merged"
        code = merge_traces.main([str(traces), "-o", str(self.out), "--symbols", str(self.root / "symbols.toml"),
                                  "--pairs", str(self.root / "waw_pairs.tsv"),
                                  "--pairs", str(self.root / "cod2_pairs.tsv"), "-q"])
        self.assertEqual(code, 0)

    def tearDown(self):
        self.tmp.cleanup()

    def candidate(self, address):
        rows = [r for r in read(self.out / "candidates.tsv") if r["address"] == address]
        self.assertEqual(len(rows), 1, address)
        return rows[0]

    def test_finds_nested_sessions_and_merges_them(self):
        functions = {r["address"]: r for r in read(self.out / "functions.tsv")}
        self.assertEqual(functions["0x82100400"]["sessions"], "2")
        self.assertEqual(functions["0x82100400"]["calls"], "100")
        self.assertIn("title screen", functions["0x82100400"]["markers"])
        self.assertEqual(functions["0x82100000"]["symbol_name"], "CL_Init")

    def test_coverage_per_module_and_level(self):
        rows = {(r["module"], r["first_seen_in"]): r for r in read(self.out / "coverage.tsv")}
        all_default = rows[("default", "(all)")]
        self.assertEqual(all_default["functions"], "1000")
        self.assertEqual(all_default["seen"], "6")
        self.assertEqual(all_default["named"], "2")  # CL_Init, and 0x82100800 from the symbol map
        self.assertEqual(rows[("default", "saint_lo")]["seen"], "4")
        self.assertEqual(rows[("default", "falaise")]["seen"], "1")
        self.assertEqual(rows[("saint_lo", "(all)")]["seen"], "1")

    def test_command_handler_named_after_registering_function(self):
        c = self.candidate("0x82100400")
        self.assertEqual(c["suggested_name"], "CL_Vid_Restart_f")
        self.assertEqual(c["confidence"], "high")

    def test_self_naming_print(self):
        c = self.candidate("0x82100500")
        self.assertEqual(c["suggested_name"], "R_LoadWorld")
        self.assertIn("r_mode", c["evidence"])
        self.assertIn("R_Other", c["pairs"])  # a lone pairing is shown, not used

    def test_level_slot_with_known_builtin(self):
        c = self.candidate("0x82100600")
        self.assertEqual(c["suggested_name"], "Broc_Anglestoforward")
        self.assertIn("saint_lo!sub_89031000", c["callers"])

    def test_two_agreeing_pairings(self):
        c = self.candidate("0x82100700")
        self.assertEqual(c["suggested_name"], "G_RunThink")
        self.assertEqual(c["confidence"], "medium")
        self.assertIn("falaise", c["levels"])

    def test_named_functions_are_no_candidates(self):
        addresses = {r["address"] for r in read(self.out / "candidates.tsv")}
        self.assertNotIn("0x82100000", addresses)
        self.assertNotIn("0x82100800", addresses)  # named in the symbol map though the build said sub_

    def test_curated_lines_are_python_tuples(self):
        lines = [l for l in (self.out / "curated_suggestions.txt").read_text().splitlines()
                 if l.strip().startswith("(")]
        entries = [ast.literal_eval(l.strip().rstrip(",")) for l in lines]
        names = {e[1] for e in entries}
        self.assertEqual(names, {"CL_Vid_Restart_f", "R_LoadWorld", "Broc_Anglestoforward", "G_RunThink"})
        for address, name, confidence, evidence in entries:
            self.assertIsInstance(address, int)
            self.assertIn(confidence, ("high", "medium"))
            self.assertTrue(evidence.startswith("trace ("))

    def test_summary_written(self):
        text = (self.out / "summary.txt").read_text()
        self.assertIn("Sessions: 2", text)
        self.assertIn("Markers: 1", text)


class Conflicts(unittest.TestCase):
    def test_name_taken_or_shared_is_not_suggested(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_session(root / "t", "s1", [
                "default\t0x82100A00\tsub_82100A00\t1.0\tx\ty\t\t1\t",
                "default\t0x82100B00\tsub_82100B00\t1.0\tx\ty\t\t1\t",
                "default\t0x82100C00\tsub_82100C00\t1.0\tx\ty\t\t1\t",
            ], evidence=[
                "print\tdefault\t0x82100A00\tsub_82100A00\tTaken_Name: x\t1\t1.0\tx\ty\t0x82100A10\t",
                "print\tdefault\t0x82100B00\tsub_82100B00\tSame_Name: x\t1\t1.0\tx\ty\t0x82100B10\t",
                "print\tdefault\t0x82100C00\tsub_82100C00\tSame_Name: y\t1\t1.0\tx\ty\t0x82100C10\t",
            ])
            (root / "symbols.toml").write_text(SYMBOLS)
            out = root / "out"
            merge_traces.main([str(root / "t"), "-o", str(out), "--symbols", str(root / "symbols.toml"), "-q"])
            rows = {r["address"]: r for r in read(out / "candidates.tsv")}
            self.assertEqual(rows["0x82100A00"]["suggested_name"], "")
            self.assertIn("already 0x82100800", rows["0x82100A00"]["reason"])
            self.assertEqual(rows["0x82100B00"]["suggested_name"], "")
            self.assertIn("2 functions would get Same_Name", rows["0x82100B00"]["reason"])


class Inputs(unittest.TestCase):
    def test_simple_symbol_parser_matches(self):
        parsed = merge_traces.parse_symbols_simple(SYMBOLS)
        self.assertEqual(parsed[0x82100000][0], "CL_Init")
        self.assertEqual(merge_traces.slot_names_from_symbols(parsed), {0x1AC: "anglestoforward"})

    def test_pairs_with_explicit_columns(self):
        with tempfile.TemporaryDirectory() as tmp:
            table = Path(tmp) / "pairs.tsv"
            table.write_text("x\ty\tz\n0x82100700\tfoo\tBar_Name\n")
            source, pairs = merge_traces.read_pairs(f"{table}:x:z")
            self.assertEqual(pairs, {0x82100700: "Bar_Name"})
            self.assertIn("z", source)

    def test_no_sessions(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(merge_traces.main([tmp, "-o", str(Path(tmp) / "o")]), 1)

    def test_handler_names(self):
        self.assertEqual(merge_traces.command_handler_name("+attack", "CL_InitInput+0x20"), "CL_Plus_Attack_f")
        self.assertEqual(merge_traces.command_handler_name("give", ""), "Cmd_Give_f")
        self.assertEqual(merge_traces.command_handler_name("map", "SV_AddOperatorCommands+0x8"), "SV_Map_f")

    def test_callers_shown_with_symbol_names(self):
        symbols = {0x82100000: ("CL_Init", "high", "")}
        self.assertEqual(merge_traces.pretty_caller("sub_82100000+0x3C", symbols), "CL_Init+0x3C")
        self.assertEqual(merge_traces.pretty_caller("sub_82100004+0x3C", symbols), "sub_82100004+0x3C")
        # Level code keeps its module and name; it isn't in the executable's map.
        self.assertEqual(merge_traces.pretty_caller("saint_lo!sub_89031000+0x44", symbols),
                         "saint_lo!sub_89031000+0x44")

    def test_functions_seen_while_starting_up(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_session(tmp, "20261016-000000",
                          ["default\t0x82100100\tsub_82100100\t1.0\t\tstarting\t\t5\t",
                           "default\t0x82100200\tsub_82100200\t90.0\tfront end\tmenu\t\t2\tsub_82100000+0x10"])
            (Path(tmp) / "symbols.toml").write_text(SYMBOLS)
            out = Path(tmp) / "merged"
            self.assertEqual(merge_traces.main([tmp, "-o", str(out), "--symbols", str(Path(tmp) / "symbols.toml"),
                                                "-q"]), 0)
            rows = {(r["module"], r["first_seen_in"]): r for r in read(out / "coverage.tsv")}
            self.assertEqual(rows[("default", merge_traces.STARTUP)]["seen"], "1")
            self.assertEqual(rows[("default", "front end")]["seen"], "1")
            functions = {r["address"]: r for r in read(out / "functions.tsv")}
            self.assertEqual(functions["0x82100100"]["first_seen"], f"{merge_traces.STARTUP}:starting")
            self.assertEqual(functions["0x82100200"]["callers"], "CL_Init+0x10")


if __name__ == "__main__":
    unittest.main()

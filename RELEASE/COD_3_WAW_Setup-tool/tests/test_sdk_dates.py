"""[waw-decomp] 2026-11-16 The SDK checkout keeps the dates of files whose bytes did not change (c3w/sdk_dates.py),
and the codegen's key is the codegen tool (c3w/codegen_key.py). No git, no build: files written by hand stand in
for a checkout, a build folder's .ninja_log and an SDK install.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import codegen_key, sdk_dates  # noqa: E402

PATCH = """From 1 Mon Sep 17 00:00:00 2001
Subject: [PATCH] test

diff --git a/src/a.cpp b/src/a.cpp
--- a/src/a.cpp
+++ b/src/a.cpp
@@ -1 +1 @@
-a
+a2
diff --git a/include/b.h b/include/b.h
new file mode 100644
--- /dev/null
+++ b/include/b.h
@@ -0,0 +1 @@
+b
diff --git a/old.h b/new.h
similarity index 90%
rename from old.h
rename to new.h
diff --git "a/odd name.h" "b/odd name.h"
--- "a/odd name.h"
+++ "b/odd name.h"
"""
OLD = 1_700_000_000_000_000_000   # a date long before the test runs, in ns


class PatchNameTests(unittest.TestCase):
    def test_names_from_a_patch(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "0001.patch"
            p.write_bytes(PATCH.encode("utf-8"))
            self.assertEqual(sdk_dates.patch_names([p]), {"src/a.cpp", "include/b.h", "old.h", "new.h"})


class SdkDatesTests(unittest.TestCase):
    """Two flavors on one checkout, the way the Linux and the Windows SDK share theirs."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.src = self.tmp / "sdk"
        self.work = self.tmp / "work"
        self.patches = self.tmp / "patches"
        for d in (self.src / "src", self.src / "include", self.work, self.patches):
            d.mkdir(parents=True)
        (self.patches / "0001.patch").write_bytes(PATCH.encode("utf-8"))
        self.builds = {f: self.tmp / "out" / f for f in ("linux-amd64", "mingw")}
        for b in self.builds.values():
            b.mkdir(parents=True)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def dates(self, flavor):
        return sdk_dates.SdkDates(self.src, self.work, flavor, self.builds[flavor], [self.patches / "0001.patch"])

    def checkout(self, text_a):
        """What a checkout does: the files written anew (today's date), whatever their bytes."""
        (self.src / "src" / "a.cpp").write_text(text_a, encoding="utf-8")
        (self.src / "include" / "b.h").write_text("b\n", encoding="utf-8")

    def build(self, flavor):
        log = self.builds[flavor] / ".ninja_log"
        with open(log, "a", encoding="utf-8") as f:
            f.write(f"{time.time_ns()}\n")

    def mtime(self, rel):
        return (self.src / rel).stat().st_mtime_ns

    def age(self, rel):
        os.utime(self.src / rel, ns=(OLD, OLD))

    def test_same_bytes_keep_the_date_of_the_last_build(self):
        self.checkout("a2\n")
        self.age("src/a.cpp")
        self.age("include/b.h")
        d = self.dates("linux-amd64")
        d.settle()                      # no record yet: everything dated now
        self.assertGreater(self.mtime("src/a.cpp"), OLD)
        self.build("linux-amd64")
        d.remember()
        built = self.mtime("src/a.cpp")
        time.sleep(0.01)
        self.checkout("a2\n")          # the next update: same bytes, new dates
        self.assertNotEqual(self.mtime("src/a.cpp"), built)
        kept, renewed = self.dates("linux-amd64").settle()
        self.assertEqual((kept, renewed), (2, 0))
        self.assertEqual(self.mtime("src/a.cpp"), built)

    def test_other_bytes_are_dated_now(self):
        self.checkout("a2\n")
        d = self.dates("linux-amd64")
        d.settle()
        self.build("linux-amd64")
        d.remember()
        built = self.mtime("src/a.cpp")
        time.sleep(0.01)
        self.checkout("a3\n")
        kept, renewed = self.dates("linux-amd64").settle()
        self.assertEqual((kept, renewed), (1, 1))
        self.assertGreater(self.mtime("src/a.cpp"), built)

    def test_a_build_since_the_record_makes_it_useless(self):
        self.checkout("a2\n")
        d = self.dates("linux-amd64")
        d.settle()
        self.build("linux-amd64")
        d.remember()
        self.build("linux-amd64")      # built by hand since: the record no longer describes that folder
        time.sleep(0.01)
        self.checkout("a2\n")
        self.assertEqual(self.dates("linux-amd64").settle(), (0, 2))

    def test_the_record_is_gone_until_the_build_succeeds(self):
        self.checkout("a2\n")
        d = self.dates("linux-amd64")
        d.settle()
        self.build("linux-amd64")
        d.remember()
        self.dates("linux-amd64").settle()
        self.assertFalse(sdk_dates.record_path(self.work, "linux-amd64").exists())

    def test_a_date_the_other_flavor_put_back_is_renewed(self):
        # Linux builds a.cpp as "a2", Windows as "w". Back on Linux, its record puts the Linux build's (old) date
        # back on "a2". Windows then finds "a2" with that old date while its build last saw "w": it must date the
        # file now, or the Windows build would keep objects made from "w".
        self.checkout("a2\n")
        lin = self.dates("linux-amd64")
        lin.settle()
        self.build("linux-amd64")
        lin.remember()
        time.sleep(0.01)
        self.checkout("w\n")
        win = self.dates("mingw")
        win.settle()
        self.build("mingw")
        win.remember()
        time.sleep(0.01)
        self.checkout("a2\n")          # back to Linux: its record gives the Linux build's date back
        self.assertEqual(self.dates("linux-amd64").settle(), (2, 0))
        linux_date = self.mtime("src/a.cpp")
        # A Windows checkout that leaves a.cpp as it is ("a2", the Linux build's old date): b.h keeps the Windows
        # build's date, a.cpp is dated now.
        self.assertEqual(self.dates("mingw").settle(), (1, 1))
        self.assertGreater(self.mtime("src/a.cpp"), linux_date)

    def test_snapshot_and_restore_unchanged(self):
        f, g = self.work / "crtversion.h", self.work / "other.h"
        f.write_text("same\n", encoding="utf-8")
        g.write_text("old\n", encoding="utf-8")
        os.utime(f, ns=(OLD, OLD))
        os.utime(g, ns=(OLD, OLD))
        before = sdk_dates.snapshot([f, g, self.work / "missing.h"])
        f.write_text("same\n", encoding="utf-8")
        g.write_text("new\n", encoding="utf-8")
        self.assertEqual(sdk_dates.restore_unchanged(before), 1)
        self.assertEqual(f.stat().st_mtime_ns, OLD)
        self.assertGreater(g.stat().st_mtime_ns, OLD)


class CodegenKeyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.install = self.tmp / "install"
        for d in ("bin", "lib", "include/rex"):
            (self.install / d).mkdir(parents=True)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def make(self, stamp, runtime_code, gpu_code):
        (self.install / "include" / "rex" / "version.h").write_text(
            f'#define REXGLUE_BUILD_TIMESTAMP "{stamp}"\n', encoding="utf-8")
        # Not ELF files: the libraries named in the program count.
        (self.install / "bin" / "rexglue").write_bytes(b"program librexruntime.so code")
        note = codegen_key.BUILD_ID_NOTE + os.urandom(20)
        (self.install / "lib" / "librexruntime.so").write_bytes(
            b"runtime " + runtime_code + b" build: x@" + stamp.encode() + b" " + note)
        (self.install / "lib" / "librexgpu-xenos.so").write_bytes(gpu_code)
        return codegen_key.tool_key(self.install, "rexglue")

    def test_timestamp_and_build_id_do_not_count(self):
        a = self.make("20261116_1500", b"same", b"gpu 1")
        b = self.make("20261116_1612", b"same", b"gpu 2")   # another configure time and another GPU plugin
        self.assertEqual(a, b)
        self.assertEqual(sorted(a["files"]), ["bin/rexglue", "lib/librexruntime.so"])

    def test_a_runtime_change_counts(self):
        self.assertNotEqual(self.make("20261116_1500", b"same", b"gpu"), self.make("20261116_1500", b"new", b"gpu"))

    def test_no_rexglue_no_key(self):
        self.assertIsNone(codegen_key.tool_key(self.install, "rexglue"))

    def test_elf_needed_of_a_non_elf_file(self):
        self.assertIsNone(codegen_key.elf_needed(b"MZ not an ELF file"))

    def test_the_sdk_app_sources_count(self):
        a = self.make("20261116_1500", b"same", b"gpu")
        (self.install / "share" / "rexglue").mkdir(parents=True)
        (self.install / "share" / "rexglue" / "rex_app.cpp").write_text("app\n", encoding="utf-8")
        b = codegen_key.tool_key(self.install, "rexglue")
        self.assertIn("share/rexglue/rex_app.cpp", b["files"])
        self.assertNotEqual(a, b)

    def test_project_inputs(self):
        proj = self.tmp / "cod3" / "mp"
        (proj / "config").mkdir(parents=True)
        (self.tmp / "cod3" / "assets").mkdir()
        (proj / "x_manifest.toml").write_text('file_path = "../assets/x.xex"\nincludes = ["config/a.toml"]\n',
                                             encoding="utf-8")
        (proj / "config" / "a.toml").write_text("a = 1\n", encoding="utf-8")
        # In config/ but named nowhere: counts too.
        (proj / "config" / "b_campaign.toml").write_text("b = 1\n", encoding="utf-8")
        (self.tmp / "cod3" / "assets" / "x.xex").write_bytes(b"XEX2 program")
        first = codegen_key.project_inputs(proj)
        self.assertEqual(sorted(first), ["../assets/x.xex", "config/a.toml", "config/b_campaign.toml",
                                         "x_manifest.toml"])
        self.assertEqual(len(first["../assets/x.xex"]), 40)            # SHA-1 for program images
        (proj / "config" / "b_campaign.toml").write_text("b = 2\n", encoding="utf-8")
        self.assertNotEqual(codegen_key.project_inputs(proj), first)

    def test_generated_manifest(self):
        proj = self.tmp / "proj"
        module = proj / "generated" / "default"
        module.mkdir(parents=True)
        (module / "a.cpp").write_text("int a;\n", encoding="utf-8")
        (module / "a.h").write_text("extern int a;\n", encoding="utf-8")
        (module / "codegen.stamp").write_text("1\n", encoding="utf-8")
        self.assertFalse(codegen_key.generated_files_match(proj))       # no manifest yet
        codegen_key.write_generated_manifest(proj)
        self.assertTrue(codegen_key.generated_files_match(proj))
        (module / "codegen.stamp").write_text("2\n", encoding="utf-8")       # the codegen's own bookkeeping
        self.assertTrue(codegen_key.generated_files_match(proj))
        (module / "a.cpp").write_text("int a = 1;\n", encoding="utf-8")      # changed by hand
        self.assertFalse(codegen_key.generated_files_match(proj))
        codegen_key.write_generated_manifest(proj)
        (module / "b.cpp").write_text("int b;\n", encoding="utf-8")          # a file the codegen didn't write
        self.assertFalse(codegen_key.generated_files_match(proj))


if __name__ == "__main__":
    unittest.main()

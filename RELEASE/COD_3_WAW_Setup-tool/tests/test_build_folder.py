"""[acct2-bro] 2026-11-22 A build folder is started over whenever CMake would act on (or silently keep) something of
another configure (c3w/build_folder.py; the user reports of 2026-11-23, setup tool 2.47.0 on Windows). Caches are
written by hand; nothing is configured.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import build_folder as bf  # noqa: E402

# The game's build folder as 2.46.0 left it on the reporting PC (the work folder through C:\c3w-work), placeholders
# for the user's own folders.
OLD = """# This is the CMakeCache file.
CMAKE_HOME_DIRECTORY:INTERNAL=C:/c3w-work/cod3
CMAKE_C_COMPILER:FILEPATH=C:/c3w-work/tools/llvm-win/bin/clang.EXE
CMAKE_CXX_COMPILER:FILEPATH=C:/c3w-work/tools/llvm-win/bin/clang++.EXE
CMAKE_MAKE_PROGRAM:FILEPATH=C:/c3w-work/tools/ninja-win/ninja.EXE
CMAKE_GENERATOR:INTERNAL=Ninja
rexglue_DIR:PATH=C:/c3w-work/rexglue-sdk/out/install/win-amd64/lib/cmake/rexglue
COD3_ENGINE_DIR:UNINITIALIZED=C:/c3w-project-c969a5fa/engine
"""
NEW = r"C:\c3w-work-2d87231e"


def now(windows=True, **over):
    args = dict(source=NEW + r"\cod3",
                compilers={"CMAKE_C_COMPILER": NEW + r"\tools\llvm-win\bin\clang.EXE",
                           "CMAKE_CXX_COMPILER": NEW + r"\tools\llvm-win\bin\clang++.EXE"},
                make_program=NEW + r"\tools\ninja-win\ninja.EXE", generator="Ninja",
                sdk_prefix=NEW + r"\rexglue-sdk\out\install\win-amd64", windows=windows)
    args.update(over)
    return args


class StaleReasons(unittest.TestCase):
    def test_renamed_work_link_starts_over(self):
        why = bf.stale_reasons(OLD, **now())
        text = "\n".join(why)
        self.assertIn("configured from C:/c3w-work/cod3", text)
        self.assertIn("CMAKE_CXX_COMPILER was C:/c3w-work/tools/llvm-win/bin/clang++.EXE", text)
        self.assertIn("CMAKE_C_COMPILER", text)
        self.assertIn("CMAKE_MAKE_PROGRAM", text)
        self.assertIn("made with the SDK at C:/c3w-work/rexglue-sdk", text)

    def test_same_configure_is_kept(self):
        cache = OLD.replace("C:/c3w-work/", "C:/c3w-work-2d87231e/")
        self.assertEqual(bf.stale_reasons(cache, **now()), [])

    def test_slashes_and_drive_letter_case_are_not_a_change(self):
        cache = OLD.replace("C:/c3w-work/", "c:/c3w-work-2d87231e//")
        self.assertEqual(bf.stale_reasons(cache, **now()), [])

    def test_another_generator(self):
        cache = OLD.replace("C:/c3w-work/", "C:/c3w-work-2d87231e/").replace("=Ninja", "=Unix Makefiles")
        self.assertEqual(bf.stale_reasons(cache, **now()), ["generator Unix Makefiles, now Ninja"])

    def test_sdk_elsewhere_only(self):
        cache = OLD.replace("C:/c3w-work/", "C:/c3w-work-2d87231e/").replace(
            "C:/c3w-work-2d87231e/rexglue-sdk", "D:/old/rexglue-sdk")
        self.assertEqual(bf.stale_reasons(cache, **now()),
                         ["made with the SDK at D:/old/rexglue-sdk/out/install/win-amd64/lib/cmake/rexglue, now "
                          "C:/c3w-work-2d87231e/rexglue-sdk/out/install/win-amd64"])

    def test_sdk_not_found_yet_is_no_reason(self):
        cache = OLD.replace("C:/c3w-work/", "C:/c3w-work-2d87231e/").replace(
            "rexglue_DIR:PATH=C:/c3w-work-2d87231e/rexglue-sdk/out/install/win-amd64/lib/cmake/rexglue",
            "rexglue_DIR:PATH=rexglue_DIR-NOTFOUND")
        self.assertEqual(bf.stale_reasons(cache, **now()), [])

    def test_linux_paths_are_case_sensitive(self):
        cache = ("CMAKE_HOME_DIRECTORY:INTERNAL=/work/cod3\nCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++\n")
        self.assertEqual(bf.stale_reasons(cache, source="/work/cod3/", compilers={"CMAKE_CXX_COMPILER":
                                          "/usr/bin/clang++"}, windows=False), [])
        self.assertEqual(len(bf.stale_reasons(cache, source="/Work/cod3", windows=False)), 1)

    def test_empty_cache(self):
        self.assertEqual(bf.stale_reasons("", **now()), [])

    def test_sdk_in_use(self):
        self.assertIsNone(bf.sdk_in_use_ok(OLD, r"C:\c3w-work\rexglue-sdk\out\install\win-amd64", windows=True))
        self.assertIn("C:/c3w-work/rexglue-sdk", bf.sdk_in_use_ok(OLD, NEW + r"\rexglue-sdk\out\install\win-amd64",
                                                                   windows=True))
        self.assertEqual(bf.sdk_in_use_ok("", "/x", windows=False), "(none)")


class Paths(unittest.TestCase):
    def test_cmake_path(self):
        self.assertEqual(bf.cmake_path(r"C:\a\\b\\"), "C:/a/b")
        self.assertEqual(bf.cmake_path("C:/"), "C:/")
        self.assertEqual(bf.cmake_path(r"\\server\share\x"), "//server/share/x")

    def test_inside(self):
        self.assertTrue(bf.inside("C:/A/b/c", r"c:\a\B", windows=True))
        self.assertFalse(bf.inside("C:/a/bc", "C:/a/b", windows=True))
        self.assertFalse(bf.inside("/a/B/c", "/a/b", windows=False))



class FailureWords(unittest.TestCase):
    """[acct2-bro] 2026-11-22 c3w/build_failure.py: a failed compile or link says whose it is."""

    def setUp(self):
        from c3w import build_failure
        self.hint = build_failure.ours_hint

    def test_the_reported_link_error_is_ours(self):
        lines = ["[2/4] Linking CXX executable cod3mp.exe", "FAILED: cod3mp.exe",
                 "lld-link: error: undefined symbol: public: static char *ImGuiTextBuffer::EmptyString",
                 ">>> referenced by CMakeFiles/cod3.dir/src/ui/f2_tour.cpp.obj",
                 "clang++: error: linker command failed with exit code 1 (use -v to see invocation)",
                 "ninja: build stopped: subcommand failed."]
        text = self.hint(lines, "2.47.0")
        self.assertIn("not a problem with your PC", text)
        self.assertIn("setup tool 2.47.0", text)
        self.assertIn("Make bug report", text)

    def test_a_compile_error_is_ours(self):
        lines = ["FAILED: CMakeFiles/cod3.dir/src/game/ngl_side.cpp.obj",
                 "C:/c3w-work/cod3/src/game/ngl_side.cpp:4945:27: error: unknown type name 'ngl_vk_mesh_draw'"]
        self.assertIn("not a problem with your PC", self.hint(lines))

    def test_the_pc_side_wins(self):
        lines = ["FAILED: CMakeFiles/cod3.dir/src/game/x.cpp.obj", "fatal error: No space left on device"]
        self.assertIn("the disk is full", self.hint(lines))
        lines = ["FAILED: a.obj", "clang++: error: unable to execute command: Killed"]
        self.assertIn("ran out of memory", self.hint(lines))

    def test_names_in_a_link_error_are_not_what_happened(self):
        # [acct2-bro] 2026-11-23 the test build's link error (MinGW): the listing names std::bad_alloc's typeinfo
        lines = ["[85/994] Linking CXX shared library libcod3_blkbrn.dll",
                 "FAILED: libcod3_blkbrn.dll libcod3_blkbrn.dll.a",
                 "ld.lld: error: undefined symbol: vtable for __cxxabiv1::__si_class_type_info",
                 ">>> referenced by CMakeFiles/cod3_blkbrn.dir/generated/blkbrn/cod3_recomp.0.cpp.obj:(typeinfo for "
                 "std::bad_alloc)",
                 "clang: error: linker command failed with exit code 1 (use -v to see invocation)",
                 "ninja: build stopped: subcommand failed."]
        text = self.hint(lines, "2.48.0")
        self.assertIn("not a problem with your PC", text)
        self.assertNotIn("memory", text)
        self.assertIn("ran out of memory", self.hint(["FAILED: a.obj", "terminate called after throwing an instance "
                                                      "of 'std::bad_alloc'"]))

    def test_other_failures_say_nothing(self):
        self.assertEqual(self.hint(["error: could not download https://example/x.zip"]), "")
        self.assertEqual(self.hint([]), "")


class CrossBuild(unittest.TestCase):
    """[acct2-bro] 2026-11-22 the Windows game built on Linux: its compilers come from the toolchain file."""

    def test_moved_toolchain_file(self):
        cache = ("CMAKE_HOME_DIRECTORY:INTERNAL=/old/work/cod3\n"
                 "CMAKE_TOOLCHAIN_FILE:FILEPATH=/old/work/mingw/toolchain.cmake\n")
        why = bf.stale_reasons(cache, source="/new/work/cod3", toolchain_file="/new/work/mingw/toolchain.cmake",
                               windows=False)
        self.assertEqual(len(why), 2)
        self.assertEqual(bf.stale_reasons(cache.replace("/old/", "/new/"), source="/new/work/cod3",
                                          toolchain_file="/new/work/mingw/toolchain.cmake", windows=False), [])


if __name__ == "__main__":
    unittest.main()

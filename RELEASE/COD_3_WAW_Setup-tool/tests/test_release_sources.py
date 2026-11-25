"""[win-builds] 2026-11-24 (owner 11-24 17:55 "users are still having issues", "please check for more missing
things"): a 2.55.1 build from the release stopped at its first configure, "file failed to open for reading:
.../cod3/assets/menus/cod3/pause_options.toml": the menus' file sat in recomp/assets/, which tools/make_release.py
(SKIP_PATHS) and .gitignore leave out of the release as disc data. Every file a port's CMake files read at configure
time (file(READ), configure_file, include) must be in the tool's own tree and in a folder the release keeps.
Nothing is built or run here.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import fnmatch
import re
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent.parent
ROOT = TOOL.parent

# tools/make_release.py's SKIP_PATHS for the tool's folders (made on each machine, never released)
NOT_RELEASED = ["COD_3_WAW_Setup-tool/*/generated/*", "COD_3_WAW_Setup-tool/*/assets/*",
                "COD_3_WAW_Setup-tool/*/*/assets/*", "COD_3_WAW_Setup-tool/*/out/*"]
KEPT = re.compile(r"/generated/rexglue\.cmake$")

READS = re.compile(r'(?:file\(\s*READ|configure_file\(|include\()\s*"?([^"\s)]+)')
SETS = re.compile(r'(?:\bset\(|cmake_path\(\s*SET)\s*(\w+)\s+(?:NORMALIZE\s+)?"?([^"\s)]+)')


def _resolve(cmake_dir, path, variables):
    for _ in range(4):
        m = re.search(r"\$\{(\w+)\}", path)
        if not m:
            break
        name = m.group(1)
        if name in ("CMAKE_CURRENT_SOURCE_DIR", "CMAKE_CURRENT_LIST_DIR"):
            value = str(cmake_dir)
        elif name in variables:
            value = variables[name]
        else:
            return None   # a build-time or cache value (binary dir, SDK, generated): not the tool's own file
        path = path.replace(m.group(0), value)
    if "$" in path or "<" in path:
        return None
    p = Path(path)
    return Path(*(p if p.is_absolute() else cmake_dir / p).parts).resolve()


class ReleasedConfigureInputs(unittest.TestCase):
    def test_every_configure_time_read_is_released(self):
        problems = []
        lists = sorted(TOOL.glob("recomp*/CMakeLists.txt")) + sorted(TOOL.glob("recomp*/mp/CMakeLists.txt"))
        self.assertTrue(lists)
        for cmake in lists:
            text = re.sub(r"#[^\n]*", "", cmake.read_text(encoding="utf-8", errors="replace"))
            variables = {}
            for name, value in SETS.findall(text):
                resolved = _resolve(cmake.parent, value, variables)
                if resolved is not None:
                    variables[name] = str(resolved)
            for raw in READS.findall(text):
                if "BINARY_DIR" in raw:
                    continue
                target = _resolve(cmake.parent, raw, variables)
                if target is None or not str(target).startswith(str(TOOL.resolve())):
                    continue
                if target.suffix == "" and not target.exists():
                    continue   # include(<module>) by name
                rel = target.relative_to(ROOT.resolve()).as_posix()
                if any(fnmatch.fnmatch(rel, pat) for pat in NOT_RELEASED) and not KEPT.search(rel):
                    problems.append(f"{cmake.relative_to(ROOT).as_posix()}: reads {rel}, which the release leaves out")
                elif not target.exists():
                    problems.append(f"{cmake.relative_to(ROOT).as_posix()}: reads {rel}, which is missing")
        self.assertEqual(problems, [])

    def test_menus_file_is_in_a_released_folder(self):
        menus = TOOL / "recomp" / "menus" / "cod3" / "pause_options.toml"
        self.assertTrue(menus.is_file())
        self.assertIn('id = "pause_options"', menus.read_text(encoding="utf-8"))
        for cmake in (TOOL / "recomp" / "CMakeLists.txt", TOOL / "recomp" / "mp" / "CMakeLists.txt"):
            code = re.sub(r"#[^\n]*", "", cmake.read_text(encoding="utf-8"))
            self.assertNotIn("assets/menus", code, cmake)
            self.assertIn("menus/cod3/pause_options.toml", code, cmake)


if __name__ == "__main__":
    unittest.main()

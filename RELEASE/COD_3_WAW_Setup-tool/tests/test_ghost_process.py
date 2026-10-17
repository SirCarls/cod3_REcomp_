"""A game that has ended must not look as if it were still running (owner,
2026-10-16: "I'm getting "ghost application" again").

A program started from the setup tool or the launcher stays in the system's
process list after it ends ("zombie", state Z) until the tool collects it.
Its name is still there, so the Play page took it for a running game: the
game settings stayed locked and Play said "Already running".

- The running check leaves ended processes out.
- What the tools start is collected when it ends.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOL))

from c3w import common, game_settings as gs  # noqa: E402


def _fake_process(proc: Path, pid: int, comm: str, state: str) -> None:
    d = proc / str(pid)
    d.mkdir()
    (d / "comm").write_text(comm + "\n", encoding="utf-8")
    (d / "stat").write_text(f"{pid} ({comm}) {state} 1 {pid} {pid} 0 -1 4194560\n", encoding="utf-8")
    (d / "cmdline").write_bytes(b"")


class EndedProcessTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.proc = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_a_zombie_game_is_not_running(self):
        _fake_process(self.proc, 4242, "cod3", "Z")
        self.assertFalse(gs.game_running(proc=self.proc))

    def test_a_running_game_is_running(self):
        _fake_process(self.proc, 4242, "cod3", "Z")
        _fake_process(self.proc, 4243, "cod3", "S")
        self.assertTrue(gs.game_running(proc=self.proc))

    def test_names_with_spaces_and_parentheses(self):
        # The state is the first field after the last ")".
        _fake_process(self.proc, 4244, "x) Z (cod3", "S")
        self.assertFalse(gs._ended(self.proc / "4244"))
        _fake_process(self.proc, 4245, "a (b) c", "X")
        self.assertTrue(gs._ended(self.proc / "4245"))

    def test_no_stat_file_counts_as_running(self):
        # Best effort: without a readable state the process is kept.
        d = self.proc / "4246"
        d.mkdir()
        (d / "comm").write_text("cod3\n", encoding="utf-8")
        self.assertTrue(gs.game_running(proc=self.proc))


@unittest.skipUnless(sys.platform.startswith("linux"), "reads /proc")
class CollectTests(unittest.TestCase):
    def test_a_started_program_is_collected_when_it_ends(self):
        done = threading.Event()
        codes = []

        def ended(code):
            codes.append(code)
            done.set()

        proc = common.start_and_collect([sys.executable, "-c", "pass"], on_exit=ended)
        self.assertTrue(done.wait(20))
        self.assertEqual(codes, [0])
        # Collected: no zombie left behind under this process.
        deadline = time.monotonic() + 5
        while Path(f"/proc/{proc.pid}").exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertFalse(Path(f"/proc/{proc.pid}").exists())


if __name__ == "__main__":
    unittest.main()

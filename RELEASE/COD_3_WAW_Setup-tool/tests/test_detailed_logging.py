"""[acct2-ocr] 2026-11-22 The detailed log (owner 00:00: "Please make the setup tool logging better and more
verbose so I would be able to figure out what's wrong"): times to the millisecond, the details (debug) in every
log file and on screen only when asked, a session log for every start with its header and crash hooks, tracebacks,
what each command and download did, and the Log page and bug report reading the new files."""
from __future__ import annotations

import hashlib
import io
import os
import re
import sys
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import common, logtools  # noqa: E402

STAMP = r"\d{2}:\d{2}:\d{2}\.\d{3}"


class _Tmp(unittest.TestCase):
    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.tmp = Path(self._dir.name)
        common.stop_session_log()
        self._verbose = common.verbose()

    def tearDown(self) -> None:
        common.stop_session_log()
        common.set_verbose(self._verbose)
        self._dir.cleanup()

    def read(self, path: Path) -> str:
        return path.read_text(encoding="utf-8")


class LineFormatTests(_Tmp):
    def test_lines_have_milliseconds_and_parse(self):
        f = self.tmp / "a.log"
        log = common.Log(file=f)
        log.info("hello")
        log.debug("a detail")
        lines = self.read(f).splitlines()
        self.assertRegex(lines[0], rf"^{STAMP} \[info\] hello$")
        self.assertRegex(lines[1], rf"^{STAMP} \[debug\] a detail$")
        e = logtools.parse_line(lines[0])
        self.assertEqual((e.level, e.text, e.time[-4:-3]), ("info", "hello", "."))
        old = logtools.parse_line("18:10:43 [warn] an old line")   # logs from before 2026-11-22
        self.assertEqual((old.level, old.text, old.time), ("warn", "an old line", "18:10:43"))
        self.assertEqual(logtools.parse_line(lines[1]).level, "debug")

    def test_details_on_the_console_only_when_asked(self):
        for on, want in ((False, ""), (True, "a detail")):
            common.set_verbose(on)
            out = io.StringIO()
            with redirect_stdout(out):
                common.console_sink("debug", "a detail", None)
            self.assertIn(want, out.getvalue())
            if not on:
                self.assertEqual(out.getvalue(), "")


class SessionLogTests(_Tmp):
    def test_header_and_every_log_into_it(self):
        path = common.start_session_log("a test", argv=["tool", "build", "--iso", "x y.iso"],
                                        header=["setup tool 9.9.9"], folder=self.tmp)
        self.assertIsNotNone(path)
        self.assertEqual(common.start_session_log("again", folder=self.tmp), path)   # once per run
        common.Log(sinks=[]).info("from one log")
        other = self.tmp / "build.log"
        common.Log(file=other).warn("from a build's log")
        common.session_info("only here")
        common.end_session_log(0)
        text = self.read(path)
        for want in ("---- session ---- a test", "command line: tool build --iso \"x y.iso\"", "python: ",
                     "setup tool 9.9.9", "from one log", "[warn] from a build's log", "only here",
                     "---- session ended (exit code 0)"):
            self.assertIn(want, text)
        self.assertIn("from a build's log", self.read(other))   # the build's own log keeps its lines too
        self.assertEqual(logtools.session_logs(self.tmp), [path])

    def test_secrets_in_the_environment_are_hidden(self):
        os.environ["C3W_TEST_TOKEN"] = "s3cret-value"
        try:
            self.assertEqual(common._masked("C3W_TEST_TOKEN", "s3cret-value"), "<hidden>")
            self.assertEqual(common._masked("PATH", "/usr/bin"), "/usr/bin")
        finally:
            del os.environ["C3W_TEST_TOKEN"]

    def test_old_sessions_are_pruned(self):
        for i in range(common.KEEP_SESSION_LOGS + 5):
            (self.tmp / f"{common.SESSION_PREFIX}20200101-0000{i:02d}-1.log").write_text("old", encoding="utf-8")
        common.start_session_log("a test", folder=self.tmp)
        self.assertEqual(len(list(self.tmp.glob(f"{common.SESSION_PREFIX}*.log"))), common.KEEP_SESSION_LOGS)

    def test_an_uncaught_error_in_a_thread_is_logged(self):
        before = threading.excepthook
        threading.excepthook = lambda args: None   # what the session's hook hands on to: quiet here
        try:
            path = common.start_session_log("a test", folder=self.tmp)

            def boom():
                raise RuntimeError("worker broke")
            t = threading.Thread(target=boom, name="job-1")
            t.start()
            t.join()
            common.stop_session_log()
        finally:
            threading.excepthook = before
        text = self.read(path)
        self.assertIn("uncaught in thread job-1", text)
        self.assertIn("RuntimeError: worker broke", text)
        self.assertIn("Traceback", text)

    def test_stop_puts_the_hooks_back(self):
        before = (sys.excepthook, threading.excepthook)
        common.start_session_log("a test", folder=self.tmp)
        self.assertIsNot(sys.excepthook, before[0])
        common.stop_session_log()
        self.assertIs(sys.excepthook, before[0])
        self.assertIs(threading.excepthook, before[1])
        self.assertIsNone(common.session_log())


class ExceptionTests(_Tmp):
    def test_unexpected_errors_keep_their_traceback(self):
        f = self.tmp / "a.log"
        log = common.Log(file=f)
        try:
            {}["missing"]
        except KeyError as e:
            log.exception(e, "reading the disc")
        text = self.read(f)
        self.assertRegex(text, rf"{STAMP} \[error\] reading the disc: unexpected KeyError: 'missing'")
        self.assertIn("Traceback (most recent call last)", text)
        self.assertIn("test_detailed_logging.py", text)

    def test_the_tools_own_errors_get_theirs_as_details(self):
        f = self.tmp / "a.log"
        log = common.Log(file=f)
        try:
            raise common.SetupError("Choose an output folder.")
        except common.SetupError as e:
            log.exception(e, "Build")
        text = self.read(f)
        self.assertIn("[debug] Build: SetupError raised here:", text)
        self.assertNotIn("[error]", text)


class RunnerTests(_Tmp):
    def runner(self):
        f = self.tmp / "run.log"
        return common.Runner(common.Log(file=f), threading.Event()), f

    def test_folder_environment_time_and_exit_code(self):
        r, f = self.runner()
        env = dict(os.environ, C3W_TEST_TOKEN="s3cret-value", C3W_TEST_X="1")
        rc = r.run([sys.executable, "-c", "print('hi'); print('there')"], cwd=self.tmp, env=env)
        self.assertEqual(rc, 0)
        text = self.read(f)
        self.assertIn(f"[debug] in {self.tmp}", text)
        self.assertIn("environment set: C3W_TEST_TOKEN=<hidden>; C3W_TEST_X=1", text)
        self.assertNotIn("s3cret-value", text)
        self.assertRegex(text, r"\[debug\] exit code 0 after \d+\.\d s, 2 lines of output")
        self.assertIn("[out] hi", text)

    def test_a_quiet_command_that_fails_shows_its_end(self):
        r, f = self.runner()
        with self.assertRaises(common.SetupError):
            r.run([sys.executable, "-c", "print('the reason'); raise SystemExit(3)"], quiet=True)
        text = self.read(f)
        self.assertRegex(text, r"\[warn\] \S+ ended with exit code 3; its last 1 lines:\nthe reason")
        self.assertIn("[debug] the whole command:", text)

    def test_a_probe_that_may_fail_stays_a_detail(self):
        r, f = self.runner()
        rc = r.run([sys.executable, "-c", "print('not here'); raise SystemExit(1)"], quiet=True, check=False)
        self.assertEqual(rc, 1)
        text = self.read(f)
        self.assertNotIn("[warn]", text)
        self.assertRegex(text, r"\[debug\] \S+ ended with exit code 1; its last 1 lines:\nnot here")

    def test_capture_is_a_detail(self):
        r, f = self.runner()
        rc, out = r.capture([sys.executable, "-c", "print('version 1.2.3')"])
        self.assertEqual(rc, 0)
        self.assertIn("version 1.2.3", out)
        text = self.read(f)
        self.assertRegex(text, r"\[debug\] \$ .*: exit code 0 after \d+\.\d s\n  \| version 1\.2\.3")

    def test_a_missing_program(self):
        r, f = self.runner()
        with self.assertRaises(common.SetupError):
            r.run([str(self.tmp / "no-such-program")])
        self.assertIn("not started:", self.read(f))

    def test_exit_codes_that_mean_something(self):
        self.assertIn("access violation", common._rc_meaning(0xC0000005))
        self.assertIn("access violation", common._rc_meaning(-1073741819))   # the same, as a signed number
        self.assertIn("DLL", common._rc_meaning(0xC0000135))
        self.assertEqual(common._rc_meaning(0), "")
        self.assertEqual(common._rc_meaning(1), "")
        if not common.IS_WINDOWS:
            self.assertIn("SIGKILL", common._rc_meaning(-9))


class DownloadTests(_Tmp):
    def setUp(self) -> None:
        super().setUp()
        self._ask = common.APPROVAL.ask
        common.APPROVAL.reset()
        common.APPROVAL.ask = lambda items: True

    def tearDown(self) -> None:
        common.APPROVAL.ask = self._ask
        common.APPROVAL.reset()
        super().tearDown()

    def test_what_a_download_did(self):
        src = self.tmp / "src.bin"
        data = os.urandom(3000)
        src.write_bytes(data)
        sha = hashlib.sha256(data).hexdigest()
        f = self.tmp / "dl.log"
        log = common.Log(file=f)
        dest = self.tmp / "out" / "file.bin"
        common.download(src.as_uri(), dest, log, threading.Event(), sha256=sha)
        self.assertEqual(dest.read_bytes(), data)
        text = self.read(f)
        self.assertIn(f"[debug] to {dest}; expected sha256 {sha}", text)
        self.assertRegex(text, rf"\[debug\] downloaded .* in \d+\.\d s \(.*/s\); sha256 {sha} \(matches\)")
        common.download(src.as_uri(), dest, log, threading.Event(), sha256=sha)
        self.assertIn("file.bin: already downloaded", self.read(f))

    def test_a_wrong_checksum_says_both(self):
        src = self.tmp / "src.bin"
        src.write_bytes(b"abc")
        f = self.tmp / "dl.log"
        with self.assertRaises(common.SetupError):
            common.download(src.as_uri(), self.tmp / "x.bin", common.Log(file=f), threading.Event(),
                            sha256="0" * 64)
        self.assertIn(f"checksum: got sha256 {hashlib.sha256(b'abc').hexdigest()}, expected {'0' * 64}",
                      self.read(f))


class LogPageAndReportTests(_Tmp):
    def test_newest_session_first(self):
        a = self.tmp / f"{common.SESSION_PREFIX}20261121-100000-1.log"
        b = self.tmp / f"{common.SESSION_PREFIX}20261122-090000-2.log"
        for p in (a, b):
            p.write_text("x", encoding="utf-8")
        self.assertEqual(logtools.session_logs(self.tmp), [b, a])
        # the Log page's list: beside the output folder's logs, not among them (known_logs is the folder's own)
        self.assertEqual([p for _, p in logtools.session_log_entries(self.tmp)], [b, a])
        self.assertTrue(all(label.startswith("Setup tool session ") for label, _ in
                            logtools.session_log_entries(self.tmp)))

    def test_a_big_session_log_keeps_its_header(self):
        p = self.tmp / "big.log"
        with open(p, "w", encoding="utf-8") as f:
            f.write("---- session ---- the header\n")
            for i in range(50000):
                f.write(f"00:00:00.000 [out] line {i}\n")
        text = logtools._read_text(p, 100000)
        kept = logtools._with_head(p, text, 100000)
        self.assertTrue(kept.startswith("---- session ---- the header"))
        self.assertIn("left out", kept)
        self.assertTrue(kept.rstrip().endswith("line 49999"))

    def test_details_are_their_own_box_on_the_log_page(self):
        try:
            from c3w import gui_log
        except ImportError as e:   # no Tkinter here
            self.skipTest(str(e))
        self.assertEqual(gui_log._group("debug"), "detail")
        self.assertEqual(gui_log._group("trace"), "detail")
        self.assertEqual(gui_log._group("out"), "info")
        self.assertEqual(gui_log._group("error"), "error")


if __name__ == "__main__":
    unittest.main()

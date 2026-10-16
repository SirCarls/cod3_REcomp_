"""The setup tool's Log page: this session's build log as it happens, or
another log (the game's, earlier setup logs, reports) read from disk; a
search box and a level filter; Copy, Open folder and "Make bug report"
(logtools.py scrubs personal paths).

A mixin for gui.App.
"""
from __future__ import annotations

import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk
from typing import Dict, List, Optional, Set, Tuple

from . import logtools, theme, widgets
from .common import SetupError
from .widgets import px, tip

LIVE = "This session's build log"
MAX_LINES = 20000          # lines kept on screen (and read from a file)
# The level boxes and the log levels each one shows (logtools.LEVELS).
LEVEL_GROUPS: Dict[str, Set[str]] = {
    "error": {"error", "critical", "fatal"},
    "warn": {"warn", "warning"},
    "info": {"info", "debug", "trace", "step", "out", "progress", ""},
}
PREFIX = {"step": "\n== ", "warn": "WARNING: ", "error": "ERROR: "}


def _group(level: str) -> str:
    level = (level or "").lower()
    return next((g for g, levels in LEVEL_GROUPS.items() if level in levels), "info")


class LogPage:
    def _page_log(self, t) -> None:
        self.log_lines: List[Tuple[str, str]] = []    # this session: (kind, text)
        self._log_sources: List[Tuple[str, Optional[Path]]] = [(LIVE, None)]
        self._log_entries: list = []                 # the file shown: logtools entries
        self.v_log_source = tk.StringVar(value=LIVE)
        self.v_log_query = tk.StringVar()
        self.v_log_count = tk.StringVar()
        self.v_level = {g: tk.BooleanVar(value=True) for g in LEVEL_GROUPS}
        t.columnconfigure(0, weight=1)
        t.rowconfigure(2, weight=1)

        widgets.page_header(t, "Log", "What the tool and the game wrote. Errors in red, warnings in amber.",
                            columns=1)
        bar = ttk.Frame(t)
        bar.grid(row=1, column=0, sticky="ew", padx=px(24), pady=(px(8), px(6)))
        bar.columnconfigure(1, weight=1)
        ttk.Label(bar, text="Show").grid(row=0, column=0, sticky="w", padx=(0, px(8)))
        self.cb_log_source = ttk.Combobox(bar, textvariable=self.v_log_source, state="readonly", width=40,
                                          values=[LIVE])
        self.cb_log_source.grid(row=0, column=1, sticky="w")
        self.cb_log_source.bind("<<ComboboxSelected>>", lambda e: self._log_reload())
        tip(self.cb_log_source, "The live build log, the game's log, earlier setup logs and reports.")
        ttk.Label(bar, text="Search").grid(row=1, column=0, sticky="w", padx=(0, px(8)), pady=(px(6), 0))
        row = ttk.Frame(bar)
        row.grid(row=1, column=1, sticky="ew", pady=(px(6), 0))
        self.e_log_query = ttk.Entry(row, textvariable=self.v_log_query, width=26)
        self.e_log_query.pack(side="left", padx=(0, px(14)))
        tip(self.e_log_query, "Only lines with this text (Ctrl+F). Esc clears it.")
        self.e_log_query.bind("<Escape>", lambda e: self.v_log_query.set(""))
        for g, label in (("error", "Errors"), ("warn", "Warnings"), ("info", "Info")):
            ttk.Checkbutton(row, text=label, variable=self.v_level[g], command=self._log_render).pack(
                side="left", padx=(0, px(8)))
        ttk.Label(row, textvariable=self.v_log_count, style="Hint.TLabel").pack(side="right")
        self._log_query_after: Optional[str] = None
        self.v_log_query.trace_add("write", lambda *a: self._log_query_changed())

        box = ttk.Frame(t)
        box.grid(row=2, column=0, sticky="nsew", padx=px(24))
        box.rowconfigure(0, weight=1)
        box.columnconfigure(0, weight=1)
        self.txt = tk.Text(box, wrap="word", height=14, width=60, font="C3WMono", borderwidth=0, highlightthickness=1,
                           padx=px(10), pady=px(8), undo=False)
        self.txt.grid(row=0, column=0, sticky="nsew")
        sb = ttk.Scrollbar(box, orient="vertical", command=self.txt.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.txt.configure(yscrollcommand=sb.set, state="disabled")
        theme.recolor(self.txt)
        # Read-only, but text can still be selected and copied.
        self.txt.bind("<Control-c>", lambda e: self._log_copy(selection=True))

        bf = ttk.Frame(t)
        bf.grid(row=3, column=0, sticky="ew", padx=px(24), pady=(px(8), px(14)))
        widgets.button_row(bf, [
            ("Copy", self._log_copy, "Copy the lines shown (the selection, if any)."),
            ("Open folder", self._log_open_folder, "The folder of the log shown."),
            ("Make bug report...", self._make_bug_report,
             "Zip the newest logs and settings to attach to a bug report; personal folder names are removed.")])
        self.btn_log_clear = ttk.Button(bf, text="Clear", command=self._clear_log)
        self.btn_log_clear.pack(side="left", padx=(px(6), 0))
        tip(self.btn_log_clear, "Empty this session's log on screen (the log files stay).")
        self.root.bind("<Control-f>", lambda e: self._log_focus_search())
        self.pages["log"]["on_show"] = self._log_refresh_sources
        self._theme_log(theme.current)
        self._log_count()

    # ------------------------------------------------------------------ colors
    def _theme_log(self, c: Dict[str, str]) -> None:
        if not hasattr(self, "txt"):
            return
        theme.recolor(self.txt, c)
        self.txt.tag_configure("step", font="C3WMonoBold", foreground=c["accent_text"])
        self.txt.tag_configure("warn", foreground=c["warn"])
        self.txt.tag_configure("error", foreground=c["bad"])
        self.txt.tag_configure("out", foreground=c["text_dim"])
        self.txt.tag_configure("info", foreground=c["text"])
        self.txt.tag_configure("match", background=c["accent"], foreground=c["accent_fg"])
        self.txt.tag_raise("match")
        self.txt.tag_raise("sel")

    # ------------------------------------------------------------------ sources
    def _log_refresh_sources(self) -> None:
        """The logs there are to pick from (they change with the output
        folder and after a build or a game run)."""
        if not hasattr(self, "cb_log_source"):
            return
        out = self._out_path()
        found: List[Tuple[str, Path]] = []
        if out is not None:
            try:
                found = list(logtools.known_logs(out))
            except (SetupError, OSError):
                found = []
        self._log_sources = [(LIVE, None)] + [(label, Path(p)) for label, p in found]
        self.cb_log_source.configure(values=[label for label, _ in self._log_sources])
        if self.v_log_source.get() not in [label for label, _ in self._log_sources]:
            self.v_log_source.set(LIVE)
            self._log_reload()

    def _log_path(self) -> Optional[Path]:
        label = self.v_log_source.get()
        return next((p for lb, p in self._log_sources if lb == label), None)

    def _log_show_live(self) -> None:
        if hasattr(self, "v_log_source") and self.v_log_source.get() != LIVE:
            self.v_log_source.set(LIVE)
            self._log_reload()

    def _log_reload(self) -> None:
        """Shows the picked log: the live one at once, a file read off the
        UI thread."""
        if not hasattr(self, "txt"):
            return
        path = self._log_path()
        self.btn_log_clear.configure(state="normal" if path is None else "disabled")
        if path is None:
            self._log_render()
            return
        self.v_log_count.set("Reading...")

        def work():
            try:
                entries = logtools.read_entries(path, MAX_LINES)
                err = ""
            except (SetupError, OSError, ValueError) as e:
                entries, err = [], str(e)

            def done():
                if self._log_path() != path:
                    return   # another log was picked meanwhile
                self._log_entries = entries
                self._log_render()
                if err:
                    self.v_log_count.set(f"Couldn't read it: {err}")
            self.q.put(("call", done))
        threading.Thread(target=work, daemon=True).start()

    # ------------------------------------------------------------------ filter and search
    def _levels(self) -> Set[str]:
        return {g for g, v in self.v_level.items() if v.get()}

    def _log_query_changed(self) -> None:
        # Filter once typing pauses, not on every key.
        if self._log_query_after:
            self.root.after_cancel(self._log_query_after)
        self._log_query_after = self.root.after(250, self._log_render)

    def _live_matches(self, kind: str, text: str, groups: Set[str], query: str) -> bool:
        if kind != "step" and _group(kind) not in groups:
            return False
        return not query or query in text.lower()

    def _log_render(self) -> None:
        """Fills the text box with what the filter and the search let
        through, coloring by level and marking the search text."""
        self._log_query_after = None
        groups = self._levels()
        query = self.v_log_query.get().strip()
        t = self.txt
        t.configure(state="normal")
        t.delete("1.0", "end")
        shown = total = 0
        if self._log_path() is None:
            total = len(self.log_lines)
            q = query.lower()
            for kind, text in self.log_lines:
                if self._live_matches(kind, text, groups, q):
                    t.insert("end", PREFIX.get(kind, "") + text + "\n", kind)
                    shown += 1
        else:
            entries = self._log_entries
            total = len(entries)
            if groups:
                levels: Set[str] = set() if groups == set(LEVEL_GROUPS) else \
                    set().union(*(LEVEL_GROUPS[g] for g in groups))
                if levels:
                    # A level the groups don't name (a game's own) counts as info.
                    known = set().union(*LEVEL_GROUPS.values())
                    levels |= {e.level for e in entries if "info" in groups and e.level not in known}
                picked = logtools.filter_entries(entries, levels, query)
            else:
                picked = []
            for e in picked:
                g = _group(e.level)
                t.insert("end", e.raw.rstrip("\n") + "\n", "error" if g == "error" else "warn" if g == "warn"
                         else "out" if e.level in ("debug", "trace", "out") else "info")
            shown = len(picked)
        self._log_mark(query)
        t.configure(state="disabled")
        t.see("end")
        self._log_count(shown, total)

    def _log_mark(self, query: str) -> None:
        if not query:
            return
        t, start, n = self.txt, "1.0", 0
        while n < 2000:
            pos = t.search(query, start, stopindex="end", nocase=True)
            if not pos:
                break
            end = f"{pos}+{len(query)}c"
            t.tag_add("match", pos, end)
            start, n = end, n + 1

    def _log_count(self, shown: Optional[int] = None, total: Optional[int] = None) -> None:
        if shown is None:
            shown = total = len(getattr(self, "log_lines", []))
        self.v_log_count.set(f"{shown} lines" if shown == total else f"{shown} of {total} lines")

    def _log_focus_search(self) -> None:
        self.show_page("log")
        self.e_log_query.focus_set()
        self.e_log_query.select_range(0, "end")

    # ------------------------------------------------------------------ the live log
    def _append(self, kind: str, text: str) -> None:
        self.log_lines.append((kind, text))
        if len(self.log_lines) > MAX_LINES:   # keep the window responsive on very long builds
            del self.log_lines[:5000]
            if self._log_path() is None:
                self._log_render()
                return
        if self._log_path() is not None:
            return
        query = self.v_log_query.get().strip().lower()
        if not self._live_matches(kind, text, self._levels(), query):
            return
        t = self.txt
        at_end = t.yview()[1] >= 0.999
        t.configure(state="normal")
        start = t.index("end-1c")
        t.insert("end", PREFIX.get(kind, "") + text + "\n", kind)
        if query:
            pos = t.search(query, start, stopindex="end", nocase=True)
            while pos:
                end = f"{pos}+{len(query)}c"
                t.tag_add("match", pos, end)
                pos = t.search(query, end, stopindex="end", nocase=True)
        t.configure(state="disabled")
        if at_end:   # follow the end, unless the user scrolled up to read
            t.see("end")
        if len(self.log_lines) % 50 == 0 or kind in ("step", "error"):
            self._log_count(*self._live_counts())

    def _live_counts(self) -> Tuple[int, int]:
        if not self.v_log_query.get().strip() and self._levels() == set(LEVEL_GROUPS):
            return len(self.log_lines), len(self.log_lines)
        groups, q = self._levels(), self.v_log_query.get().strip().lower()
        shown = sum(1 for k, x in self.log_lines if self._live_matches(k, x, groups, q))
        return shown, len(self.log_lines)

    def _clear_log(self) -> None:
        self.log_lines.clear()
        self._log_render()

    # ------------------------------------------------------------------ buttons
    def _log_copy(self, selection: bool = False) -> str:
        try:
            text = self.txt.get("sel.first", "sel.last")
        except tk.TclError:
            text = "" if selection else self.txt.get("1.0", "end-1c")
        if text:
            self._copy_text(text)
        return "break"

    def _log_open_folder(self) -> None:
        path = self._log_path()
        if path is not None:
            self._open(str(path.parent))
        else:
            self._open_logs()

    def _make_bug_report(self) -> None:
        out = self._out_path()
        if out is None or not out.is_dir():
            self._info("No output folder", "There is nothing to report yet.\nChoose the output folder on Set up "
                                           "(the logs are kept there).", kind="warning")
            return
        name = time.strftime("cod3-bug-report-%Y%m%d-%H%M.zip")
        start = out / "logs" if (out / "logs").is_dir() else out
        dest = filedialog.asksaveasfilename(title="Save the bug report", parent=self.root, initialdir=str(start),
                                            initialfile=name, defaultextension=".zip",
                                            filetypes=[("Zip files", "*.zip"), ("All files", "*")])
        if not dest:
            return

        def work():
            zip_path, names = logtools.make_bug_report(out, Path(dest))

            def done():
                listing = "\n".join("- " + n for n in names[:20]) + (f"\n- and {len(names) - 20} more"
                                                                     if len(names) > 20 else "")
                widgets.message(self.root, "Bug report", f"Bug report made.\n{zip_path}\n\nIt has:\n{listing}\n\n"
                                "Personal folder names are replaced. Look it over before you share it.",
                                kind="done", buttons=[("Open folder", lambda: self._open(str(Path(zip_path).parent))),
                                                      ("Close", None)])
                self.v_status.set("Bug report made.")
            self.q.put(("call", done))
        self._bg(work, "Making the bug report...")

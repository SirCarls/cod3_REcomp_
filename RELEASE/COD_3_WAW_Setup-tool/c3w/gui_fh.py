"""The Finest Hour tab's pages ([acct2-decomp] 2026-11-21, owner 11:18 "add finest hour to the tabs"; he chose a full
tab like Big Red One's): Set up (your Xbox disc, then Build: c3w/fh.py), Play and Port status (gui_games.py's
_page_status). Big Red One's pages (gui_games.py) are the pattern; the port's own settings are in fh.toml, which the
game's F2 window changes too, so the Play page only has what a start needs.

A mixin for gui.App, like gui_games.Games: it uses the App's variables and helpers (v_out, v_work, v_install,
v_cleanup, _row, _open, _info, _start_task, q ...).
"""
from __future__ import annotations

import subprocess
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import ttk
from typing import Optional

from . import common, fh, widgets
from .common import IS_WINDOWS
from .pipeline import TOOL_VERSION
from .widgets import hint, px, section, tip

DISC_TYPES = [("Disc images", "*.iso *.ISO")]
DEFAULT_SIZE = "As in fh.toml"   # the window size list's first entry: fh.toml's own


class FhPages:
    # ------------------------------------------------------------------ settings
    def _init_fh_settings(self, s: dict) -> None:
        self.v_fh_source = tk.StringVar(value=s.get("fh_source") if isinstance(s.get("fh_source"), str) else "")
        self.v_fh_size = tk.StringVar()
        self.v_fh_language = tk.StringVar()
        self.v_fh_fullscreen = tk.BooleanVar()
        self.v_fh_vsync = tk.BooleanVar()
        self.v_fh_skip_films = tk.BooleanVar()
        self._fh_set_play(fh.PlayOptions.from_settings(s))
        self.v_fh_status = tk.StringVar(value="")
        self.v_fh_play_reason = tk.StringVar(value="")
        self._fh_disc_why = "Checking your disc..."   # why Build can't start for the disc ("" = it can)
        self._fh_gen = 0
        self._fh_proc = None
        self.v_fh_source.trace_add("write", lambda *a: self._fh_check_disc())

    def _fh_set_play(self, play: "fh.PlayOptions") -> None:
        self.v_fh_size.set(play.size or DEFAULT_SIZE)
        self.v_fh_language.set(dict(fh.LANGUAGES).get(play.language, fh.LANGUAGES[0][1]))
        self.v_fh_fullscreen.set(play.fullscreen)
        self.v_fh_vsync.set(play.vsync)
        self.v_fh_skip_films.set(play.skip_films)

    def _fh_play_options(self) -> "fh.PlayOptions":
        size = self.v_fh_size.get().strip()
        language = next((key for key, label in fh.LANGUAGES if label == self.v_fh_language.get()), "")
        return fh.PlayOptions(fullscreen=bool(self.v_fh_fullscreen.get()), vsync=bool(self.v_fh_vsync.get()),
                              size="" if size == DEFAULT_SIZE else size, language=language,
                              skip_films=bool(self.v_fh_skip_films.get()))

    def _fh_settings(self) -> dict:
        return {"fh_source": self.v_fh_source.get(), **self._fh_play_options().to_settings()}

    def _fh_apply_settings(self, s: dict) -> None:
        if isinstance(s.get("fh_source"), str):
            self.v_fh_source.set(s["fh_source"])
        merged = {**self._fh_play_options().to_settings(), **{k: v for k, v in s.items() if k.startswith("fh_")}}
        self._fh_set_play(fh.PlayOptions.from_settings(merged))

    def _fh_out(self) -> Optional[Path]:
        out = self._out_path()
        return out / fh.OUTPUT_SUBDIR if out else None

    # ------------------------------------------------------------------ Set up
    def _page_fh_setup(self, t) -> None:
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Set up", "Make Call of Duty: Finest Hour from your own Xbox disc, then press Build. "
                                         "Your disc is only read, never changed.", columns=1)
        self.pages["fh_setup"]["on_show"] = self._fh_check_disc
        d = section(t, "Your disc", 1, columns=1)
        self._grid_cols(d)
        self._row(d, 0, "Finest Hour (Xbox)", self.v_fh_source, [
            ("ISO...", lambda: self._pick_file(self.v_fh_source, "Finest Hour disc image", DISC_TYPES),
             "Pick the disc image (.iso, full or trimmed)."),
            ("Folder...", lambda: self._pick_dir(self.v_fh_source, "Folder copied from the Finest Hour disc"),
             "Pick a folder holding the disc's files (default.xbe and the rest).")],
            tooltip="Your original Xbox Call of Duty: Finest Hour: its disc image, or a folder copied from it. The "
                    "Europe disc (English, French, Spanish, Italian) builds; the USA disc not yet.")
        self.lbl_fh_status = hint(d, var=self.v_fh_status)
        self.lbl_fh_status.grid(row=1, column=1, columnspan=2, sticky="w", pady=(px(2), 0))

        o = section(t, "Where it goes", 3, columns=1)
        self._grid_cols(o)
        self._row(o, 0, "Output folder", self.v_out, [
            ("Browse...", lambda: self._pick_dir(self.v_out, "Output folder"), ""),
            ("Open", lambda: self._open(str(self._fh_out() or "")), "The game's folder (fh/ in the output folder).")],
            tooltip="The same output folder as the other tabs: Finest Hour goes into its fh/ folder.")
        hint(o, "The game goes into fh/ there: the disc's files (about 4.5 GB), the port, its settings (fh.toml), your "
                "saves and a launcher (play.sh). The build itself takes about 1.5 GB in the work folder while it "
                "runs.").grid(row=1, column=0, columnspan=3, sticky="w", pady=(px(4), 0))

        b = section(t, "The build", 5, columns=1)
        hint(b, "Big Red One's steps: your disc is checked and its files copied; xboxrecomp (the recompiler, MIT) "
                "and Capstone are downloaded once, after you approve them (Big Red One uses the same ones); "
                "default.xbe is turned into C (mostly on one core); then it is built with Vulkan graphics and SDL3 "
                "for the window, sound and pads. With Call of Duty 3 built in the same work folder, its SDK's SDL3, "
                "FFmpeg (the films) and the F2 window come along. A build that stops goes on where it stopped."
             ).pack(anchor="w")
        opts = ttk.Frame(b)
        opts.pack(anchor="w", pady=(px(8), 0))
        tip(ttk.Checkbutton(opts, text="Install what's missing (listed for approval first)", variable=self.v_install),
            "The same setting as Call of Duty 3's: the build's packages (CMake, a C compiler, SDL3, Vulkan, glslc, "
            "OpenSSL) through your package manager.").pack(anchor="w")
        tip(ttk.Checkbutton(opts, text="Delete the build files when done", variable=self.v_cleanup),
            "The same setting as Call of Duty 3's. The recompiled C is the game's code; it stays in the work "
            "folder only while it is needed.").pack(anchor="w")
        ok, why = fh.supported()
        if not ok:
            hint(b, why, style="Warn.TLabel").pack(anchor="w", pady=(px(8), 0))
        bf = ttk.Frame(b)
        bf.pack(anchor="w", pady=(px(10), px(18)))
        widgets.button_row(bf, [("Check disc", self._fh_check_disc, "Read the disc again."),
                                ("Show log", lambda: self.show_page("log"), "The build's output.")])

    def _fh_check_disc(self) -> None:
        """Checks the disc off the UI thread (it reads default.xbe, and a folder's file list)."""
        if not hasattr(self, "v_fh_source"):
            return
        self._fh_gen += 1
        gen, text = self._fh_gen, self.v_fh_source.get()
        if not text.strip():
            self._fh_disc_why = "Build needs your Finest Hour disc: choose its .iso (or a folder) on Set up."
            self._fh_show_status("Choose your disc image (.iso) or a folder copied from the disc.", "Hint.TLabel")
            self._update_actions()
            return
        self._fh_disc_why = "Checking your disc..."
        self._fh_show_status("Checking...", "Hint.TLabel")

        def work():
            disc, line = fh.check_disc(text)

            def done():
                if gen != self._fh_gen:
                    return   # a newer check started meanwhile
                self._fh_disc_why = "" if disc else line
                self._fh_show_status(line, "Good.TLabel" if disc else "Warn.TLabel")
                self._update_actions()
            self.q.put(("call", done))
        threading.Thread(target=work, daemon=True).start()

    def _fh_show_status(self, text: str, style: str) -> None:
        self.v_fh_status.set(text)
        if hasattr(self, "lbl_fh_status"):
            self.lbl_fh_status.configure(style=style)

    def _fh_build_reason(self) -> str:
        """Why Finest Hour's Build can't start ("" when it can)."""
        if self.worker and self.worker.is_alive():
            return "Busy: wait for the job running now, or cancel it."
        ok, why = fh.supported()
        if not ok:
            return why
        if self._fh_disc_why:
            return self._fh_disc_why
        if self._out_path() is None:
            return "Build needs an output folder: choose one on Set up."
        return ""

    def _fh_build(self) -> None:
        why = self._fh_build_reason()
        if why:
            self.v_status.set(why)
            return
        opts = fh.BuildOptions(source=self.v_fh_source.get(), output_dir=str(self._out_path()),
                               work_dir=self.v_work.get().strip() or str(common.default_work_dir()),
                               jobs=self._jobs(), install_deps=bool(self.v_install.get()),
                               cleanup=bool(self.v_cleanup.get()), tool_version=TOOL_VERSION)
        self._save_quietly()
        common.APPROVAL.reset()

        def work(runner: common.Runner) -> str:
            return fh.run_build(opts, runner, self.log, self.cancel,
                                on_step=lambda i, n, title: self.q.put(("step", i, n, title)))
        self.show_page("log")
        self._start_task("Build Finest Hour", work, [("Play", lambda: self.show_page("fh_play")),
                                                     ("Open game folder", lambda: self._open(str(self._fh_out())))])

    # ------------------------------------------------------------------ Play
    def _page_fh_play(self, t) -> None:
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Play", "Start Finest Hour. Its own settings are in its F2 window (and fh.toml).",
                            columns=1)
        self.pages["fh_play"]["on_show"] = self._update_actions
        g = section(t, "Call of Duty: Finest Hour", 1, columns=1)
        self._grid_cols(g)
        top = ttk.Frame(g)
        top.grid(row=0, column=0, columnspan=3, sticky="ew")
        self.btn_fh_play_page = ttk.Button(top, text="Play Finest Hour", style="Accent.TButton", command=self._play_fh)
        self.btn_fh_play_page.pack(side="left")
        self.lbl_fh_play_reason = ttk.Label(top, textvariable=self.v_fh_play_reason, style="Hint.TLabel",
                                            wraplength=px(440), justify="left")
        self.lbl_fh_play_reason.pack(side="left", padx=(px(14), 0))
        hint(g, "Keyboard: W A S D the left stick, I J K L the right stick, the arrows the d-pad, Return Start, Escape "
                "Back, Space A, Left Ctrl B, R X, F Y, Q white, E black, Z and X the triggers. A gamepad works too. "
                "F2 opens the settings window; ~ its console.").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(px(6), 0))

        s = section(t, "This start", 3, columns=1)
        self._grid_cols(s)
        rows = (("Window size", ttk.Combobox(s, textvariable=self.v_fh_size, state="readonly", width=18,
                                             values=[DEFAULT_SIZE] + [x for x in fh.WINDOW_SIZES if x]),
                 "The window's size (fh.toml's display.window_size when it says As in fh.toml)."),
                ("Language", ttk.Combobox(s, textvariable=self.v_fh_language, state="readonly", width=18,
                                          values=[label for _, label in fh.LANGUAGES]),
                 "The game's language, as the console's would choose it (fh.toml's game.language)."))
        for r, (label, field, help_text) in enumerate(rows):
            ttk.Label(s, text=label).grid(row=r, column=0, sticky="w", pady=px(3))
            field.grid(row=r, column=1, sticky="w", pady=px(3))
            tip(field, help_text)
        boxes = ttk.Frame(s)
        boxes.grid(row=len(rows), column=0, columnspan=3, sticky="w", pady=(px(6), 0))
        for text, var, help_text in (("Fullscreen", self.v_fh_fullscreen, "Fill the screen instead of a window."),
                                     ("VSync", self.v_fh_vsync, "Wait for the screen's refresh (no tearing)."),
                                     ("Skip films", self.v_fh_skip_films, "Every film stops as soon as it starts.")):
            tip(ttk.Checkbutton(boxes, text=text, variable=var), help_text).pack(side="left", padx=(0, px(14)))
        hint(s, "Saved for the next start; each wins over fh.toml for this start only. The game keeps its own "
                "options and saves in saves/.").grid(row=len(rows) + 1, column=0, columnspan=3, sticky="w",
                                                     pady=(px(6), 0))

        f = section(t, "Folders", 5, columns=1)
        bf = ttk.Frame(f)
        bf.pack(anchor="w", pady=(0, px(18)))
        widgets.button_row(bf, [("Open game folder", lambda: self._open(str(self._fh_out() or "")),
                                 "fh/ in the output folder: the game, fh.toml, its saves and play.sh."),
                                ("Open saves", lambda: self._open(str((self._fh_out() or Path()) / "saves")), ""),
                                ("Open logs", lambda: self._open(str((self._fh_out() or Path()) / "logs")),
                                 "fh.log: what the game wrote at its last start; build.log: the last build.")])

    def _fh_play_reason(self) -> str:
        out = self._out_path()
        if out is None:
            return "Choose an output folder on Set up first."
        if self.worker and self.worker.is_alive():
            return "A job is running: wait for it to finish (or Cancel it)."
        cmd, why = fh.play_command(out)
        if cmd is None:
            return why
        if self._fh_proc is not None and self._fh_proc.poll() is None:
            return "Finest Hour is running."
        return ""

    def _play_fh(self) -> None:
        why = self._fh_play_reason()
        if why:
            self._info("Not started", why, kind="warning")
            return
        opts = self._fh_play_options()
        problems = opts.problems()
        if problems:
            self._info("Not started", "\n".join(problems), kind="warning")
            return
        out = self._out_path()
        cmd, _ = fh.play_command(out)
        lay = fh.layout(out, out)
        self._save()
        lay.logs.mkdir(parents=True, exist_ok=True)
        log_path = lay.logs / "fh.log"
        if log_path.exists():
            log_path.replace(lay.logs / "fh.previous.log")
        self.log.info("Launching: " + " ".join(cmd) + f" (its output: {log_path})")
        started = time.monotonic()

        def ended(code: int) -> None:
            took = time.monotonic() - started
            self.q.put(("call", lambda: (self.log.info(f"Finest Hour exited after {took / 60:.1f} min (exit code "
                                                       f"{code}); its output is in {log_path}."),
                                         self._update_actions())))
        try:
            with open(log_path, "wb") as f:
                kwargs = {} if IS_WINDOWS else {"start_new_session": True}
                self._fh_proc = common.start_and_collect(cmd, on_exit=ended, cwd=str(lay.out),
                                                         env=fh.play_env(opts), stdout=f,
                                                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                                         **kwargs)
        except OSError as e:
            self._info("Could not start", f"Finest Hour didn't start.\n{e}", kind="error")
            return
        self.v_status.set("Finest Hour is starting...")
        self._update_actions()

    def _fh_update_actions(self, busy: bool) -> str:
        """Finest Hour's Build and Play buttons and its Play page; returns the reason Build is off."""
        if not hasattr(self, "fh_actions"):
            return ""
        why = "" if busy else self._fh_build_reason()
        play_why = self._fh_play_reason()
        built = fh.installed(self._out_path()) is not None if self._out_path() else False
        self.fh_actions["build"].configure(state="disabled" if busy or why else "normal",
                                           style=self._action_style(accent=not built))
        self.fh_actions["play"].configure(state="disabled" if play_why else "normal",
                                          style=self._action_style(accent=built))
        if hasattr(self, "btn_fh_play_page"):
            self.btn_fh_play_page.configure(state="disabled" if play_why else "normal")
            self.v_fh_play_reason.set(play_why or "Ready: built from your disc.")
            self.lbl_fh_play_reason.configure(style="Warn.TLabel" if play_why else "Good.TLabel")
        return why

    def _fh_make_actions(self, f, labels: dict) -> None:
        """The bottom bar's Build and Play for the Finest Hour tab (gui_games._build_actions)."""
        build = ttk.Button(f, text=labels["build"], style="Big.Accent.TButton", command=self._fh_build)
        build.pack(side="left")
        tip(build, "Build Finest Hour from your disc (Ctrl+B). Nothing is downloaded before you approve the list.",
            reason=self._fh_build_reason)
        play = ttk.Button(f, text=labels["play"], style="Big.TButton", command=self._play_fh)
        play.pack(side="left", padx=(px(6), 0))
        tip(play, "Start Finest Hour.", reason=self._fh_play_reason)
        self.fh_actions = {"build": build, "play": play}


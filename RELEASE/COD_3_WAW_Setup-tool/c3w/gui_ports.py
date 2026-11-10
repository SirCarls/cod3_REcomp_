"""The Set up and Play pages of the World at War, Call of Duty 2 and Call of Duty 4 tabs, and their Build and Play
buttons: each game recompiled from your own Xbox 360 disc on Call of Duty 3's SDK (c3w/x360_ports.py).
[waw-decomp] 2026-11-09 (owner 11-08 13:02: "Please include cod2 WAW and cod4 within this release").

A mixin for gui.App, like gui_games.Games: it uses the App's variables and helpers (v_out, v_work, _row, _open,
_info, _start_task ...). Everything is per game, in dicts keyed by the game's id ("cod2", "waw", "cod4").
"""
from __future__ import annotations

import subprocess
import time
import tkinter as tk
from pathlib import Path
from tkinter import ttk
from typing import Dict, Optional

from . import common, widgets, x360_ports
from .common import IS_WINDOWS
from .pipeline import BuildOptions
from .widgets import hint, px, section, tip

DISC_TYPES = [("Disc images", "*.iso *.ISO")]
NO_FOV = "Game's own"


class Ports:
    # ------------------------------------------------------------------ settings
    def _init_port_settings(self, s: dict) -> None:
        self.port_vars: Dict[str, dict] = {}
        self.port_disc_why: Dict[str, str] = {}
        self.port_gen: Dict[str, int] = {}
        self.port_procs: Dict[str, Optional[subprocess.Popen]] = {}
        self.port_widgets: Dict[str, dict] = {}
        self.port_actions: Dict[str, dict] = {}
        for pid in x360_ports.PORT_ORDER:
            p = x360_ports.PORTS[pid]
            src = s.get(f"port_{pid}_source")
            v = {"source": tk.StringVar(value=src if isinstance(src, str) else ""),
                 "status": tk.StringVar(value=""), "play_reason": tk.StringVar(value=""),
                 "program": tk.StringVar(), "fullscreen": tk.BooleanVar(), "mute": tk.BooleanVar(),
                 "skip_films": tk.BooleanVar(), "fov": tk.StringVar(), "max_fps": tk.StringVar(),
                 "graphics_api": tk.StringVar()}
            self.port_vars[pid] = v
            self._port_set_play(pid, x360_ports.PlayOptions.from_settings(p, s))
            self.port_disc_why[pid] = "Checking your disc..."
            self.port_gen[pid] = 0
            self.port_procs[pid] = None
            v["source"].trace_add("write", lambda *a, g=pid: self._port_check_disc(g))

    def _port_set_play(self, pid: str, o: "x360_ports.PlayOptions") -> None:
        p, v = x360_ports.PORTS[pid], self.port_vars[pid]
        labels = {q.name: q.label for q in p.programs}
        v["program"].set(labels.get(o.program, p.programs[0].label))
        v["fullscreen"].set(o.fullscreen)
        v["mute"].set(o.mute)
        v["skip_films"].set(o.skip_films)
        v["fov"].set(f"{o.fov:g}" if o.fov else NO_FOV)
        v["max_fps"].set("" if o.max_fps < 0 else str(o.max_fps))
        v["graphics_api"].set(o.graphics_api)

    def _port_play_options(self, pid: str) -> "x360_ports.PlayOptions":
        p, v = x360_ports.PORTS[pid], self.port_vars[pid]
        program = next((q.name for q in p.programs if q.label == v["program"].get()), "")
        try:
            fov = 0.0 if v["fov"].get().strip() in ("", NO_FOV) else float(v["fov"].get())
        except ValueError:
            fov = -1.0   # problems() says what's wrong
        text = v["max_fps"].get().strip()
        try:
            max_fps = -1 if not text else int(text)
        except ValueError:
            max_fps = -2
        api = v["graphics_api"].get()
        return x360_ports.PlayOptions(program="" if program == p.programs[0].name else program,
                                      fullscreen=bool(v["fullscreen"].get()), mute=bool(v["mute"].get()),
                                      skip_films=bool(v["skip_films"].get()), fov=fov, max_fps=max_fps,
                                      graphics_api=api if api in x360_ports.GRAPHICS_APIS else "auto")

    def _port_settings(self) -> dict:
        out: dict = {}
        for pid in x360_ports.PORT_ORDER:
            out[f"port_{pid}_source"] = self.port_vars[pid]["source"].get()
            o = self._port_play_options(pid)
            o.fov = max(o.fov, 0.0)
            o.max_fps = max(o.max_fps, -1)
            out.update(o.to_settings(x360_ports.PORTS[pid]))
        return out

    def _port_apply_settings(self, s: dict) -> None:
        for pid in x360_ports.PORT_ORDER:
            p = x360_ports.PORTS[pid]
            if isinstance(s.get(f"port_{pid}_source"), str):
                self.port_vars[pid]["source"].set(s[f"port_{pid}_source"])
            o = self._port_play_options(pid)
            merged = {**o.to_settings(p), **{k: val for k, val in s.items() if k.startswith(f"port_{pid}_")}}
            self._port_set_play(pid, x360_ports.PlayOptions.from_settings(p, merged))

    def _port_out(self, pid: str) -> Optional[Path]:
        out = self._out_path()
        return out / x360_ports.PORTS[pid].subdir if out else None

    def _port_source(self, pid: str) -> str:
        """The disc for this game: its own field, else (World at War) the Xbox 360 image of the Content page."""
        text = self.port_vars[pid]["source"].get().strip()
        if not text and pid == "waw" and hasattr(self, "v_xbox"):
            text = self.v_xbox.get().strip()
        return text

    # ------------------------------------------------------------------ Set up
    def _page_port_setup(self, t, pid: str) -> None:
        p, v = x360_ports.PORTS[pid], self.port_vars[pid]
        key = f"{pid}_setup"
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Set up", f"Make {p.name} from your own Xbox 360 disc, then press Build. Your disc is "
                                         "only read, never changed.", columns=1)
        self.pages[key]["on_show"] = lambda g=pid: self._port_check_disc(g)
        d = section(t, "Your disc", 1, columns=1)
        self._grid_cols(d)
        short = p.name.split(":")[0] if pid != "waw" else "World at War"
        self._row(d, 0, f"{short} (Xbox 360)", v["source"], [
            ("ISO...", lambda: self._pick_file(v["source"], f"{p.name} Xbox 360 disc image", DISC_TYPES),
             "Pick the disc image (.iso)."),
            ("Folder...", lambda: self._pick_dir(v["source"], f"Folder copied from the {p.name} disc"),
             "Pick a folder holding the disc's files (default.xex and the rest).")],
            tooltip=f"Your Xbox 360 {p.name}: its disc image, or a folder copied from it." +
                    (" Empty: the Xbox 360 image on the Content page." if pid == "waw" else ""))
        lbl = hint(d, var=v["status"])
        lbl.grid(row=1, column=1, columnspan=2, sticky="w", pady=(px(2), 0))
        self.port_widgets.setdefault(pid, {})["status"] = lbl

        o = section(t, "Where it goes", 3, columns=1)
        self._grid_cols(o)
        self._row(o, 0, "Output folder", self.v_out, [
            ("Browse...", lambda: self._pick_dir(self.v_out, "Output folder"), ""),
            ("Open", lambda: self._open(str(self._port_out(pid) or "")),
             f"The game's folder ({p.subdir}/ in the output folder).")],
            tooltip=f"The same output folder as the other tabs: {p.name} goes into its {p.subdir}/ folder.")
        hint(o, f"The game goes into {p.subdir}/ there: the disc's files (about {p.disc_size / 1e9:.0f} GB), the "
                "recompiled programs, your saves and a launcher. The build uses the work folder of the Settings on "
                "Call of Duty 3's Set up page, and the same SDK: built once, it serves every game.").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(px(4), 0))

        b = section(t, "The build", 5, columns=1)
        progs = " and ".join(f"{q.name} ({q.label.lower()})" for q in p.programs)
        hint(b, f"The same steps as Call of Duty 3: your disc is checked and its files copied; the ReXGlue SDK is "
                f"built (or reused) with the project's patches; the game's programs, {progs}, are translated to C++ "
                "and compiled. Graphics: Vulkan (and Direct3D 12 on Windows) through the SDK's GPU emulation for "
                "now; the game's native renderer comes later. A build that stops goes on where it stopped.").pack(
            anchor="w")
        opts = ttk.Frame(b)
        opts.pack(anchor="w", pady=(px(8), 0))
        tip(ttk.Checkbutton(opts, text="Linux version", variable=self.v_linux),
            "The same setting as Call of Duty 3's.").pack(anchor="w")
        tip(ttk.Checkbutton(opts, text="Windows version" + ("" if IS_WINDOWS else " (made with llvm-mingw)"),
                            variable=self.v_windows), "The same setting as Call of Duty 3's.").pack(anchor="w")
        tip(ttk.Checkbutton(opts, text="Install what's missing (listed for approval first)", variable=self.v_install),
            "The same setting as Call of Duty 3's.").pack(anchor="w")
        tip(ttk.Checkbutton(opts, text="Keep the SDK after the build (faster rebuilds of every game)",
                            variable=self.v_keep_sdk), "The same setting as Call of Duty 3's.").pack(anchor="w")
        bf = ttk.Frame(b)
        bf.pack(anchor="w", pady=(px(10), px(18)))
        widgets.button_row(bf, [("Check disc", lambda: self._port_check_disc(pid), "Read the disc again."),
                                ("Show log", lambda: self.show_page("log"), "The build's output.")])

    def _port_check_disc(self, pid: str) -> None:
        """Reads the disc on a thread; the line under it says what it is."""
        if not hasattr(self, "port_vars"):
            return
        p = x360_ports.PORTS[pid]
        self.port_gen[pid] += 1
        gen, text = self.port_gen[pid], self._port_source(pid)
        if not text:
            self.port_disc_why[pid] = f"Build needs your {p.name} Xbox 360 disc: choose its .iso (or a folder) on Set up."
            self._port_show_status(pid, "Choose your disc image (.iso) or a folder copied from the disc.", "Hint.TLabel")
            self._update_actions()
            return
        self.port_disc_why[pid] = "Checking your disc..."
        self._port_show_status(pid, "Checking...", "Hint.TLabel")

        def work():
            disc, line = x360_ports.check_disc(p, text)

            def done():
                if gen != self.port_gen[pid]:
                    return
                self.port_disc_why[pid] = "" if disc else line
                self._port_show_status(pid, line, "Good.TLabel" if disc else "Warn.TLabel")
                self._update_actions()
            self.q.put(("call", done))
        import threading
        threading.Thread(target=work, daemon=True).start()

    def _port_show_status(self, pid: str, text: str, style: str) -> None:
        self.port_vars[pid]["status"].set(text)
        lbl = self.port_widgets.get(pid, {}).get("status")
        if lbl is not None:
            lbl.configure(style=style)

    def _port_build_reason(self, pid: str) -> str:
        if self._out_path() is None:
            return "Choose an output folder on Set up."
        if not (self.v_linux.get() or self.v_windows.get()):
            return "Tick the Linux or the Windows version on Set up."
        return self.port_disc_why.get(pid, "")

    def _port_build(self, pid: str) -> None:
        why = self._port_build_reason(pid)
        if why:
            self.v_status.set(why)
            return
        p = x360_ports.PORTS[pid]
        opts = BuildOptions(output_dir=str(self._out_path()),
                            work_dir=self.v_work.get().strip() or str(common.default_work_dir()), jobs=self._jobs(),
                            install_deps=bool(self.v_install.get()), cleanup=bool(self.v_cleanup.get()),
                            keep_sdk=bool(self.v_keep_sdk.get()), build_linux=bool(self.v_linux.get()),
                            build_windows=bool(self.v_windows.get()))
        source = self._port_source(pid)
        self._save_quietly()
        common.APPROVAL.reset()

        def work(runner: common.Runner) -> str:
            return x360_ports.run_build(p, source, opts, self.log, self.cancel,
                                        on_step=lambda i, n, title: self.q.put(("step", i, n, title)))
        self.show_page("log")
        self._start_task(f"Build {p.name}", work, [("Play", lambda: self.show_page(f"{pid}_play")),
                                                   ("Open game folder", lambda: self._open(str(self._port_out(pid))))])

    # ------------------------------------------------------------------ Play
    def _page_port_play(self, t, pid: str) -> None:
        p, v = x360_ports.PORTS[pid], self.port_vars[pid]
        key = f"{pid}_play"
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Play", f"Start {p.name}, with the port's own settings.", columns=1)
        self.pages[key]["on_show"] = self._update_actions
        g = section(t, p.name, 1, columns=1)
        self._grid_cols(g)
        top = ttk.Frame(g)
        top.grid(row=0, column=0, columnspan=3, sticky="ew")
        btn = ttk.Button(top, text=f"Play {p.name.split(':')[0] if pid != 'waw' else 'World at War'}",
                         style="Accent.TButton", command=lambda: self._play_port(pid))
        btn.pack(side="left")
        reason = ttk.Label(top, textvariable=v["play_reason"], style="Hint.TLabel", wraplength=px(440),
                           justify="left")
        reason.pack(side="left", padx=(px(14), 0))
        self.port_widgets.setdefault(pid, {}).update({"play_btn": btn, "play_reason": reason})

        s = section(t, "Settings", 3, columns=1)
        self._grid_cols(s)
        rows = [("Program", ttk.Combobox(s, textvariable=v["program"], state="readonly", width=18,
                                         values=[q.label for q in p.programs]),
                 "The campaign, or multiplayer when the port has it.")]
        if p.fov:
            rows.append(("Field of view", ttk.Combobox(s, textvariable=v["fov"], width=10,
                                                       values=[NO_FOV, "65", "80", "90", "100", "110", "120"]),
                         "Degrees, 40-140; the game's own when left as it is."))
        if p.max_fps:
            rows.append(("Frame limit", ttk.Spinbox(s, from_=0, to=1000, increment=30, textvariable=v["max_fps"],
                                                    width=8),
                         "Frames a second, 0 = none; empty = the port's own."))
        if IS_WINDOWS:
            rows.append(("Graphics API", ttk.Combobox(s, textvariable=v["graphics_api"], state="readonly", width=10,
                                                      values=list(x360_ports.GRAPHICS_APIS)),
                         "auto: Vulkan, or Direct3D 12 when Vulkan can't start."))
        for r, (label, fld, help_text) in enumerate(rows):
            ttk.Label(s, text=label).grid(row=r, column=0, sticky="w", pady=px(3))
            fld.grid(row=r, column=1, sticky="w", pady=px(3))
            tip(fld, help_text)
        boxes = ttk.Frame(s)
        boxes.grid(row=len(rows), column=0, columnspan=3, sticky="w", pady=(px(6), 0))
        checks = [("Fullscreen", v["fullscreen"], "Fill the screen instead of a window."),
                  ("Mute", v["mute"], "No sound.")]
        if p.skip_films:
            checks.append(("Skip the start-up films", v["skip_films"], "Straight to Press START."))
        for text, var, help_text in checks:
            tip(ttk.Checkbutton(boxes, text=text, variable=var), help_text).pack(side="left", padx=(0, px(14)))
        hint(s, f"Saved for the next start. The game keeps its own settings in {p.settings_prefix}.toml and its "
                "saves in userdata/, in its folder.").grid(row=len(rows) + 1, column=0, columnspan=3, sticky="w",
                                                         pady=(px(6), 0))

        f = section(t, "Folders", 5, columns=1)
        bf = ttk.Frame(f)
        bf.pack(anchor="w", pady=(0, px(18)))
        widgets.button_row(bf, [("Open game folder", lambda: self._open(str(self._port_out(pid) or "")),
                                 f"{p.subdir}/ in the output folder."),
                                ("Open logs", lambda: self._open(str((self._port_out(pid) or Path()) / "userdata" /
                                                                    "logs")), "The game's own log.")])

    def _port_play_reason(self, pid: str) -> str:
        out = self._out_path()
        if out is None:
            return "Choose an output folder on Set up first."
        if self.worker and self.worker.is_alive():
            return "A job is running: wait for it to finish (or Cancel it)."
        p = x360_ports.PORTS[pid]
        cmd, why = x360_ports.play_command(p, out, self._port_play_options(pid))
        if cmd is None:
            return why
        proc = self.port_procs.get(pid)
        if proc is not None and proc.poll() is None:
            return f"{p.name} is running."
        return ""

    def _play_port(self, pid: str) -> None:
        why = self._port_play_reason(pid)
        if why:
            self._info("Not started", why, kind="warning")
            return
        p = x360_ports.PORTS[pid]
        out = self._out_path()
        cmd, _ = x360_ports.play_command(p, out, self._port_play_options(pid))
        root = out / p.subdir
        self._save()
        logs = root / "userdata" / "logs"
        logs.mkdir(parents=True, exist_ok=True)
        out_log = logs / "console.txt"
        self.log.info("Launching: " + " ".join(cmd) + f" (its log: {logs})")
        started = time.monotonic()

        def ended(code: int) -> None:
            took = time.monotonic() - started
            self.q.put(("call", lambda: (self.log.info(f"{p.name} exited after {took / 60:.1f} min (exit code "
                                                       f"{code}); its log is in {logs}."),
                                         self._update_actions())))
        try:
            with open(out_log, "wb") as f:
                kwargs = {} if IS_WINDOWS else {"start_new_session": True}
                self.port_procs[pid] = common.start_and_collect(cmd, on_exit=ended, cwd=str(root), stdout=f,
                                                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                                                **kwargs)
        except OSError as e:
            self._info("Could not start", f"{p.name} didn't start.\n{e}", kind="error")
            return
        self.v_status.set(f"{p.name} is starting...")
        self._update_actions()

    # ------------------------------------------------------------------ the bottom bar
    def _port_make_actions(self, frame, pid: str, labels: dict) -> None:
        p = x360_ports.PORTS[pid]
        build = ttk.Button(frame, text=labels["build"], style="Big.Accent.TButton", command=lambda: self._port_build(pid))
        build.pack(side="left")
        tip(build, f"Build {p.name} from your disc (Ctrl+B). Nothing is downloaded before you approve the list.",
            reason=lambda: self._port_build_reason(pid))
        play = ttk.Button(frame, text=labels["play"], style="Big.TButton", command=lambda: self._play_port(pid))
        play.pack(side="left", padx=(px(6), 0))
        tip(play, f"Start {p.name}.", reason=lambda: self._port_play_reason(pid))
        self.port_actions[pid] = {"build": build, "play": play}

    def _port_update_actions(self, busy: bool) -> Dict[str, str]:
        """Each port's Build and Play buttons and its Play page; returns each one's reason Build is off."""
        reasons: Dict[str, str] = {}
        for pid, b in getattr(self, "port_actions", {}).items():
            why = "" if busy else self._port_build_reason(pid)
            play_why = self._port_play_reason(pid)
            out = self._out_path()
            built = bool(out and x360_ports.installed(x360_ports.PORTS[pid], out))
            b["build"].configure(state="disabled" if busy or why else "normal",
                                 style=self._action_style(accent=not built))
            b["play"].configure(state="disabled" if play_why else "normal", style=self._action_style(accent=built))
            w = self.port_widgets.get(pid, {})
            if "play_btn" in w:
                w["play_btn"].configure(state="disabled" if play_why else "normal")
                self.port_vars[pid]["play_reason"].set(play_why or "Ready: built from your disc. " +
                                                       x360_ports.last_build(x360_ports.PORTS[pid], out))
                w["play_reason"].configure(style="Warn.TLabel" if play_why else "Good.TLabel")
            reasons[pid] = why
        return reasons

    def _port_action_buttons(self) -> list:
        out = []
        for b in getattr(self, "port_actions", {}).values():
            out += [b["build"], b["play"]]
        return out

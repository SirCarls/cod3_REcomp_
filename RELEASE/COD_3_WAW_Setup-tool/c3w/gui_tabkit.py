"""[acct2-ocr] 2026-11-22 The other games' tabs in Call of Duty 3's layout (owner 00:26: "Also the other game's tabs need
to be more akin to cod3's"; asked which parts, he chose all of them, and "We also need to extend the waifu2x options
for them; but this will come way later"):

- Set up as Call of Duty 3's: Your disc, What to build, then More options behind "Show more options";
- Play as Call of Duty 3's: the game's box with its Play button, then Game settings in sub-tabs (Display, Sound,
  Language, Controls, ...) with Save, Undo changes and Game defaults;
- a Tools page (This PC, Folders, Profiles, Appearance) and an About page (Documents, Keyboard shortcuts, Licenses
  and credits) on every tab, each about its own game.

A mixin for gui.App, first among its bases: the Set up and Play builders here take the place of the older ones of
the same names in gui_games.py (Big Red One), gui_fh.py (Finest Hour) and gui_ports.py (World at War, Call of Duty 2
and 4), which keep all the logic (disc checks, builds, starts, the settings' values and files) and are not changed.
The pages keep the attributes that logic updates (btn_bro_play_page, lbl_fh_status, port_widgets, ...).
"""
from __future__ import annotations

import threading
import tkinter as tk
from pathlib import Path
from tkinter import ttk
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from . import bro, common, fh, games, theme, widgets, x360_ports
from .common import IS_LINUX, IS_WINDOWS
from .pipeline import TOOL_VERSION
from .widgets import hint, px, section, tip

DISC_TYPES = [("Disc images", "*.iso *.ISO")]
NO_FOV = "Game's own"          # gui_ports' word for "the game decides"
BRO_DEFAULT_SIZE = "Default"   # gui_games' first window size
FH_DEFAULT_SIZE = "As in fh.toml"   # gui_fh's first window size

# Each tab's documents on its About page: (path in the project, button, what it is). The project's own follow.
GAME_DOCS: Dict[str, List[Tuple[str, str, str]]] = {
    games.BRO: [("bro/README.md", "Big Red One", "The port: how it is built, what works, how to play.")],
    games.FH: [("fh/README.md", "Finest Hour", "The port: how it is built, what works, how to play.")],
    games.WAW: [("docs/X360_FAMILY.md", "Xbox 360 ports", "World at War, Call of Duty 2 and 4 on Call of Duty 3's SDK."),
                ("docs/WAW_DECOMP.md", "World at War", "What is known of the game's code.")],
    games.COD2: [("docs/X360_FAMILY.md", "Xbox 360 ports", "World at War, Call of Duty 2 and 4 on Call of Duty 3's SDK.")],
    games.COD4: [("docs/X360_FAMILY.md", "Xbox 360 ports", "World at War, Call of Duty 2 and 4 on Call of Duty 3's SDK.")],
    games.CODUO: [("docs/NEW_COPY.md", "New copy", "The data-only copy of your PC game."),
                  ("docs/PC_SERVER.md", "LAN server", "The LAN server and how to join it."),
                  ("docs/PC_MULTIPLAYER.md", "Multiplayer", "Playing over the network.")],
}
COMMON_DOCS = [("README.md", "Read me", "How to set up and play; the project folder."),
               ("docs/SECURITY.md", "Security", "What the tool downloads, runs and deletes, and how it checks."),
               ("docs/SOURCES.md", "Sources", "Every download, its version and checksum."),
               ("docs/REQUIREMENTS.md", "Requirements", "What your PC needs."),
               ("LICENSE", "License", "MIT, for this project's code.")]
# What each tab's game is built with, for its About page.
CREDITS = {
    games.BRO: "Big Red One is recompiled with xboxrecomp (MIT) and its disassembler Capstone (BSD), and drawn with "
               "Vulkan; SDL3 gives it its window, sound and pads.",
    games.FH: "Finest Hour is recompiled with xboxrecomp (MIT) and its disassembler Capstone (BSD), as Big Red One "
              "is, and drawn with Vulkan; SDL3 gives it its window, sound and pads.",
    games.WAW: "World at War is recompiled on Call of Duty 3's SDK: the ReXGlue SDK (BSD 3-Clause) and the Xenia "
               "project it derives from. The SDK patches in COD_3_WAW_Setup-tool/patches are under the same license.",
    games.COD2: "Call of Duty 2 is recompiled on Call of Duty 3's SDK: the ReXGlue SDK (BSD 3-Clause) and the Xenia "
                "project it derives from.",
    games.COD4: "Call of Duty 4 is recompiled on Call of Duty 3's SDK: the ReXGlue SDK (BSD 3-Clause) and the Xenia "
                "project it derives from.",
    games.CODUO: "United Offensive's tools (the data-only copy and the LAN server) are this project's own code.",
}
TRADEMARK = ("Call of Duty is a trademark of Activision. This project contains no Activision code or assets: "
             "everything game-specific is made on your PC from your own copies. The tab headers are drawn by this "
             "tool; no game artwork or logo is used.")


def kit_pages(game_id: str) -> Tuple[str, str]:
    """The keys of a tab's Tools and About pages (Call of Duty 3's are plain "tools" and "about")."""
    return f"{game_id}_tools", f"{game_id}_about"


class TabKit:
    # ------------------------------------------------------------------ shared pieces
    def _kit_header(self, t, subtitle: str) -> None:
        """A Set up page's title, with "Open input folder" beside it, as on Call of Duty 3's."""
        head = widgets.page_header(t, "Set up", subtitle, columns=1)
        b = ttk.Button(head, text="Open input folder", command=self._open_input)
        b.pack(side="right", anchor="n", padx=(px(12), 0))
        tip(b, "Put your disc images and downloads here: they're found by themselves.")

    def _kit_disc_tip(self, d, row: int) -> None:
        hint(d, "Tip: put your disc images and downloads in the input folder and they're found by "
                "themselves.").grid(row=row, column=0, columnspan=3, sticky="w", pady=(px(6), 0))

    def _kit_output_row(self, w, row: int, game_dir: Callable[[], Optional[Path]], where: str) -> None:
        self._row(w, row, "Output folder", self.v_out, [
            ("Browse...", lambda: self._pick_dir(self.v_out, "Where everything goes"), ""),
            ("Open", lambda: self._open(str(game_dir() or self.v_out.get())), f"The game's folder ({where}).")],
            tooltip=f"The same output folder as Call of Duty 3's: this game goes into its {where} there.")

    def _kit_more_options(self, t, row: int, boxes: Sequence[Tuple[str, tk.Variable, str]], page_key: str,
                          sdk_note: str = "") -> None:
        """Call of Duty 3's "Show more options" link and its section: the work folder, build threads and the
        tab's boxes (the same settings as Call of Duty 3's)."""
        more = ttk.Frame(t)
        more.grid(row=row, column=0, sticky="w", padx=px(24), pady=(px(14), 0))
        adv = section(t, "More options", row + 1, columns=1, top=8)
        self._grid_cols(adv)
        self._row(adv, 0, "Work folder (temporary)", self.v_work, [
            ("Browse...", lambda: self._pick_dir(self.v_work, "Temporary build folder"), "")],
            tooltip="Temporary build files: the same work folder as Call of Duty 3's." + (" " + sdk_note
                                                                                          if sdk_note else ""))
        ttk.Label(adv, text="Build threads").grid(row=1, column=0, sticky="w", pady=px(3))
        jf = ttk.Frame(adv)
        jf.grid(row=1, column=1, columnspan=2, sticky="w", pady=px(3))
        ttk.Spinbox(jf, from_=1, to=max(1, common.cpu_count()), textvariable=self.v_jobs, width=5).pack(side="left")
        ttk.Label(jf, text=f"{common.cpu_count()} CPU threads, {common.total_ram_gb():.0f} GB RAM "
                           f"({common.recommended_jobs()} recommended)", style="Hint.TLabel").pack(
            side="left", padx=(px(10), 0))
        for i, (text, var, help_text) in enumerate(boxes):
            cb = ttk.Checkbutton(adv, text=text, variable=var)
            cb.grid(row=2 + i, column=0, columnspan=3, sticky="w", pady=px(1))
            if help_text:
                tip(cb, help_text)
        state = {"shown": False}

        def toggle() -> None:
            state["shown"] = not state["shown"]
            for w in (adv, adv.head):
                if state["shown"]:
                    w.grid()
                else:
                    w.grid_remove()
            link.configure(text="Hide more options" if state["shown"] else "Show more options")
            page = getattr(self, "pages", {}).get(page_key)
            if page:
                page["scroll"].relayout()
        link = widgets.link(more, "Show more options", toggle)
        link.pack(side="left")
        adv.grid_remove()
        adv.head.grid_remove()

    def _kit_footer(self, t, row: int) -> None:
        hint(t, "Nothing is downloaded before you approve the list (what, from where, into which folder). "
                "Everything stays in this project folder (Tools > Folders). Cancel any time; Build continues where it "
                "stopped.").grid(row=row, column=0, sticky="w", padx=px(24), pady=(px(18), px(18)))

    def _kit_game_box(self, t, title: str, button: str, command: Callable[[], None], reason_var: tk.StringVar,
                      keys: str) -> Tuple[ttk.Button, ttk.Label]:
        """The Play page's first section, as Call of Duty 3's: the game's Play button, why it is off (or that it
        is ready), and how it is played."""
        g = section(t, title, 1, columns=1)
        self._grid_cols(g)
        top = ttk.Frame(g)
        top.grid(row=0, column=0, columnspan=3, sticky="ew")
        btn = ttk.Button(top, text=button, style="Accent.TButton", command=command)
        btn.pack(side="left")
        lbl = widgets.autowrap(ttk.Label(top, textvariable=reason_var, style="Hint.TLabel", justify="left"), 440)
        lbl.pack(side="left", padx=(px(14), 0))
        hint(g, keys).grid(row=1, column=0, columnspan=3, sticky="w", pady=(px(6), 0))
        return btn, lbl

    # ------------------------------------------------------------------ Game settings, as Call of Duty 3's
    def _kit_settings(self, s, page_key: str, groups: Sequence[Tuple[str, Sequence[tuple]]], intro: str,
                      where: str, save: Callable[[], None], undo: Callable[[], None], defaults: Callable[[], None],
                      dirty: Callable[[], bool], reason: Optional[tk.StringVar] = None,
                      save_text: str = "Save game settings", top: Sequence[tuple] = ()) -> dict:
        """Call of Duty 3's Game settings for another game: sub-tabs (one shown at a time), each a column of rows
        (label | control, its help under the control), then Save, Undo changes and Game defaults. A row is
        ("check", label, var, help), ("choice", label, var, values, help[, editable]), ("number", label, var,
        (low, high, step), help) or ("heading", text); `top` rows stand above the tabs (which program's file the
        tabs are). Returns {"refresh", "show", "reason", "titles"}."""
        hint(s, intro).grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, px(2)))
        variables: List[tk.Variable] = []
        if top:
            tf = ttk.Frame(s)
            self._grid_cols(tf)
            tf.grid(row=1, column=0, columnspan=3, sticky="ew", pady=(px(4), 0))
            r = 0
            for row in top:
                r = self._kit_row(tf, row, r, variables)
        bar = ttk.Frame(s)
        bar.grid(row=2, column=0, columnspan=3, sticky="w", pady=(px(10), px(4)))
        frames: Dict[str, ttk.Frame] = {}
        buttons: Dict[str, ttk.Button] = {}
        for title, rows in groups:
            f = ttk.Frame(s)
            self._grid_cols(f)
            f.grid(row=3, column=0, columnspan=3, sticky="nsew")
            r = 0
            for row in rows:
                r = self._kit_row(f, row, r, variables)
            frames[title] = f
            buttons[title] = ttk.Button(bar, text=title, style="SubTab.Ghost.TButton", width=0,
                                        command=lambda t=title: show(t))
        widgets.flow(bar, gap=4)
        act = ttk.Frame(s)
        act.grid(row=4, column=1, columnspan=2, sticky="w", pady=(px(14), px(4)))
        b_save = ttk.Button(act, text=save_text, style="Accent.TButton", command=lambda: (save(), refresh()))
        b_undo = ttk.Button(act, text="Undo changes", command=lambda: (undo(), refresh()))
        b_defaults = ttk.Button(act, text="Game defaults", command=lambda: (defaults(), refresh()))
        tip(b_save, "Keep these for the game's next start.")
        tip(b_undo, "Back to what was saved.")
        tip(b_defaults, "Fill in the game's defaults (kept when you press Save).")
        widgets.flow(act, gap=6)
        reason = reason or tk.StringVar(value="")
        hint(s, var=reason).grid(row=5, column=1, columnspan=2, sticky="w")
        hint(s, where).grid(row=6, column=0, columnspan=3, sticky="w", pady=(px(8), px(18)))

        def show(title: str) -> None:
            for t, f in frames.items():
                if t == title:
                    f.grid()
                else:
                    f.grid_remove()
            for t, b in buttons.items():
                b.configure(style="SubTab.Accent.TButton" if t == title else "SubTab.Ghost.TButton")
            page = getattr(self, "pages", {}).get(page_key)
            if page:
                page["scroll"].relayout()

        def refresh() -> None:
            try:
                changed = bool(dirty())
            except Exception:  # noqa: BLE001 - a row being typed into must never break the page
                changed = False
            for b in (b_save, b_undo):
                b.configure(state="normal" if changed else "disabled")

        for var in variables:
            var.trace_add("write", lambda *a: refresh())
        if groups:
            show(groups[0][0])
        refresh()
        return {"refresh": refresh, "show": show, "reason": reason, "titles": [title for title, _ in groups]}

    def _kit_row(self, f, row: tuple, r: int, variables: List[tk.Variable]) -> int:
        kind = row[0]
        if kind == "heading":
            ttk.Label(f, text=row[1], style="Accent.TLabel").grid(row=r, column=0, columnspan=3, sticky="w",
                                                                 pady=(px(12 if r else 4), px(2)))
            return r + 1
        if kind == "check":
            _, label, var, help_text = row
            w = ttk.Checkbutton(f, text=label, variable=var)
        else:
            label, var = row[1], row[2]
            ttk.Label(f, text=label).grid(row=r, column=0, sticky="w", pady=(px(5), 0))
            if kind == "choice":
                values, help_text = list(row[3]), row[4]
                editable = len(row) > 5 and row[5]
                w = ttk.Combobox(f, textvariable=var, state="normal" if editable else "readonly", values=values)
                widgets.fit_list_width(w, values)
            else:   # number
                (low, high, step), help_text = row[3], row[4]
                w = ttk.Spinbox(f, from_=low, to=high, increment=step, textvariable=var, width=8)
        w.grid(row=r, column=1, columnspan=2, sticky="w", pady=(px(5), 0))
        variables.append(var)
        if help_text:
            hint(f, help_text, style="Small.TLabel", wrap=560).grid(row=r + 1, column=1, columnspan=2, sticky="w",
                                                                   padx=(px(24), 0) if kind == "check" else 0)
            return r + 2
        return r + 1

    def _kit_saved(self) -> dict:
        """The tool's settings as saved (what Undo goes back to)."""
        return common.read_json(Path(self.config_dir) / "settings.json")

    # ------------------------------------------------------------------ Big Red One
    def _page_bro_setup(self, t) -> None:
        t.columnconfigure(0, weight=1)
        self._kit_header(t, "Make Call of Duty 2: Big Red One from your own Xbox disc, then press Build. Your disc is "
                            "only read, never changed.")
        self.pages["bro_setup"]["on_show"] = self._bro_check_disc
        d = section(t, "Your disc", 1, columns=1)
        self._grid_cols(d)
        self._row(d, 0, "Big Red One (Xbox)", self.v_bro_source, [
            ("ISO...", lambda: self._pick_file(self.v_bro_source, "Big Red One disc image", DISC_TYPES),
             "Pick the disc image (.iso, full or trimmed)."),
            ("Folder...", lambda: self._pick_dir(self.v_bro_source, "Folder copied from the Big Red One disc"),
             "Pick a folder holding the disc's files (default.xbe and the rest).")],
            tooltip="Your original Xbox Call of Duty 2: Big Red One: its disc image, or a folder copied from it.")
        self.lbl_bro_status = hint(d, var=self.v_bro_status)
        self.lbl_bro_status.grid(row=1, column=1, columnspan=2, sticky="w", pady=(px(2), 0))
        self._kit_disc_tip(d, 2)

        w = section(t, "What to build", 3, columns=1)
        self._grid_cols(w)
        widgets.autowrap(ttk.Label(w, text="The playable Big Red One, recompiled from your disc (about an hour the "
                                           "first time)", style="Bold.TLabel")).grid(row=0, column=0, columnspan=3,
                                                                                     sticky="w")
        hint(w, f"Version: {'Windows (.exe)' if IS_WINDOWS else 'Linux'}, this PC's own (each system builds its "
                "own). The steps are Call of Duty 3's: your disc is checked and its files copied; xboxrecomp (the "
                "recompiler, MIT) and Capstone are downloaded once, after you approve them; default.xbe is turned "
                "into C (mostly on one core); then it is built with Vulkan graphics and SDL sound.").grid(
            row=1, column=0, columnspan=3, sticky="w", padx=(px(26), 0), pady=(px(2), px(4)))
        self._kit_output_row(w, 2, self._bro_out, "bro/")
        hint(w, "The game goes into bro/ there: the disc's files (about 4 GB), the port, your saves and a launcher "
                "(play.sh). The build itself takes about 1.5 GB in the work folder while it runs.").grid(
            row=3, column=1, columnspan=2, sticky="w")
        tip(widgets.autowrap(ttk.Checkbutton(w, text="Install anything that's missing (you approve the list first; "
                                                             "recommended)", variable=self.v_install)),
            "The same setting as Call of Duty 3's: the build's packages (CMake, a C compiler, SDL, Vulkan, glslc, "
            "OpenSSL) through your package manager.").grid(row=4, column=0, columnspan=3, sticky="w",
                                                           pady=(px(4), 0))
        ok, why = bro.supported()
        if not ok:
            hint(w, why, style="Warn.TLabel").grid(row=5, column=0, columnspan=3, sticky="w", pady=(px(4), 0))
        self._kit_more_options(t, 7, [
            ("Delete the build files when done", self.v_cleanup,
             "The same setting as Call of Duty 3's. The recompiled C is the game's code; it stays in the work "
             "folder only while it is needed.")], "bro_setup")
        self._kit_footer(t, 10)

    def _page_bro_play(self, t) -> None:
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Play", "Start Big Red One, and change its settings before it starts.", columns=1)
        self.pages["bro_play"]["on_show"] = lambda: (self._update_actions(), self._kit_bro.get("refresh", int)())
        self.btn_bro_play_page, self.lbl_bro_play_reason = self._kit_game_box(
            t, "Call of Duty 2: Big Red One", "Play Big Red One", self._play_bro, self.v_bro_play_reason,
            "Keyboard: W A S D move, the mouse looks, the left and right mouse buttons are the triggers, Space is A, "
            "Ctrl B, R X, F Y, Q and E white and black, Enter Start, Esc Back. A gamepad works too.")
        s = section(t, "Game settings", 3, columns=1)
        self._grid_cols(s)

        def saved() -> "bro.PlayOptions":
            return bro.PlayOptions.from_settings(self._kit_saved())

        def save() -> None:
            problems = self._bro_play_options().problems()
            if problems:
                reason.set("; ".join(problems))
                return
            self._save()
            reason.set("Saved: the game gets them at its next start.")

        def undo() -> None:
            self._bro_set_play(saved())
            reason.set("Back to what was saved.")

        def defaults() -> None:
            self._bro_set_play(bro.PlayOptions())
            reason.set("The port's defaults: Save keeps them.")
        reason = tk.StringVar(value="")
        self._kit_bro = self._kit_settings(
            s, "bro_play", [
                ("Display", [("choice", "Window size", self.v_bro_size,
                              [BRO_DEFAULT_SIZE] + [x for x in bro.WINDOW_SIZES if x],
                              "The size the game draws at (Default: 1280 x 960 in a window)."),
                             ("check", "Fullscreen", self.v_bro_fullscreen, "Fill the screen instead of a window."),
                             ("check", "VSync", self.v_bro_vsync, "Wait for the screen's refresh (no tearing).")]),
                ("Sound", [("check", "Mute", self.v_bro_mute, "No sound.")]),
                ("Controls", [("check", "Mouse look", self.v_bro_mouse_look,
                               "The mouse turns the view while the window has the focus."),
                              ("number", "Mouse speed", self.v_bro_mouse_speed,
                               (bro.MOUSE_SPEED_RANGE[0], bro.MOUSE_SPEED_RANGE[1], 100),
                               f"How far the view turns for the mouse ({bro.DEFAULT_MOUSE_SPEED} is the port's own).")]),
                ("Game", [("choice", "Start in", self.v_bro_level, [label for _, label in bro.LEVELS],
                           "The menus, or straight into a level.")]),
            ],
            intro="The port's own settings, given to the game when it starts (Play saves them too). The game keeps "
                  "its own options and your saves in saves/.",
            where="Kept with the setup tool's settings (config/settings.json in the project folder).",
            save=save, undo=undo, defaults=defaults, reason=reason,
            dirty=lambda: self._bro_play_options().to_settings() != saved().to_settings())

    # ------------------------------------------------------------------ Finest Hour
    def _page_fh_setup(self, t) -> None:
        t.columnconfigure(0, weight=1)
        self._kit_header(t, "Make Call of Duty: Finest Hour from your own Xbox disc, then press Build. Your disc is "
                            "only read, never changed.")
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
        self._kit_disc_tip(d, 2)

        w = section(t, "What to build", 3, columns=1)
        self._grid_cols(w)
        widgets.autowrap(ttk.Label(w, text="The playable Finest Hour, recompiled from your disc",
                                   style="Bold.TLabel")).grid(row=0, column=0, columnspan=3, sticky="w")
        hint(w, "Version: Linux, this PC's own (the Windows build comes next). Big Red One's steps: your disc is "
                "checked and its files copied; xboxrecomp (the recompiler, MIT) and Capstone are downloaded once, "
                "after you approve them (Big Red One uses the same ones); default.xbe is turned into C; then it is "
                "built with Vulkan graphics and SDL3 for the window, sound and pads. With Call of Duty 3 built in the "
                "same work folder, its SDK's SDL3, FFmpeg (the films) and the F2 window come along.").grid(
            row=1, column=0, columnspan=3, sticky="w", padx=(px(26), 0), pady=(px(2), px(4)))
        self._kit_output_row(w, 2, self._fh_out, "fh/")
        hint(w, "The game goes into fh/ there: the disc's files (about 4.5 GB), the port, its settings (fh.toml), "
                "your saves and a launcher (play.sh). The build takes about 1.5 GB in the work folder while it "
                "runs.").grid(row=3, column=1, columnspan=2, sticky="w")
        tip(widgets.autowrap(ttk.Checkbutton(w, text="Install anything that's missing (you approve the list first; "
                                                             "recommended)", variable=self.v_install)),
            "The same setting as Call of Duty 3's: the build's packages (CMake, a C compiler, SDL3, Vulkan, glslc, "
            "OpenSSL) through your package manager.").grid(row=4, column=0, columnspan=3, sticky="w",
                                                           pady=(px(4), 0))
        ok, why = fh.supported()
        if not ok:
            hint(w, why, style="Warn.TLabel").grid(row=5, column=0, columnspan=3, sticky="w", pady=(px(4), 0))
        self._kit_more_options(t, 7, [
            ("Delete the build files when done", self.v_cleanup,
             "The same setting as Call of Duty 3's. The recompiled C is the game's code; it stays in the work "
             "folder only while it is needed.")], "fh_setup")
        self._kit_footer(t, 10)

    def _page_fh_play(self, t) -> None:
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Play", "Start Finest Hour, and change its settings before it starts. The rest are in "
                                       "its F2 window (and fh.toml).", columns=1)
        self.pages["fh_play"]["on_show"] = lambda: (self._update_actions(), self._kit_fh.get("refresh", int)())
        self.btn_fh_play_page, self.lbl_fh_play_reason = self._kit_game_box(
            t, "Call of Duty: Finest Hour", "Play Finest Hour", self._play_fh, self.v_fh_play_reason,
            "Keyboard: W A S D the left stick, I J K L the right stick, the arrows the d-pad, Return Start, Escape "
            "Back, Space A, Left Ctrl B, R X, F Y, Q white, E black, Z and X the triggers. A gamepad works too. "
            "F2 opens the settings window; ~ its console.")
        s = section(t, "Game settings", 3, columns=1)
        self._grid_cols(s)

        def saved() -> "fh.PlayOptions":
            return fh.PlayOptions.from_settings(self._kit_saved())

        def save() -> None:
            problems = self._fh_play_options().problems()
            if problems:
                reason.set("; ".join(problems))
                return
            self._save()
            reason.set("Saved: the game gets them at its next start.")

        def undo() -> None:
            self._fh_set_play(saved())
            reason.set("Back to what was saved.")

        def defaults() -> None:
            self._fh_set_play(fh.PlayOptions())
            reason.set("The port's defaults: Save keeps them.")
        reason = tk.StringVar(value="")
        self._kit_fh = self._kit_settings(
            s, "fh_play", [
                ("Display", [("choice", "Window size", self.v_fh_size,
                              [FH_DEFAULT_SIZE] + [x for x in fh.WINDOW_SIZES if x],
                              "The window's size (fh.toml's display.window_size with As in fh.toml)."),
                             ("check", "Fullscreen", self.v_fh_fullscreen, "Fill the screen instead of a window."),
                             ("check", "VSync", self.v_fh_vsync, "Wait for the screen's refresh (no tearing).")]),
                ("Language", [("choice", "Language", self.v_fh_language, [label for _, label in fh.LANGUAGES],
                               "The game's language, as the console's would choose it (fh.toml's game.language).")]),
                ("Cutscenes", [("check", "Skip films", self.v_fh_skip_films,
                                "Every film stops as soon as it starts.")]),
            ],
            intro="Given to the game when it starts, each one winning over fh.toml for that start (Play saves them "
                  "too). The game keeps its own options and your saves in saves/.",
            where="Kept with the setup tool's settings (config/settings.json in the project folder).",
            save=save, undo=undo, defaults=defaults, reason=reason,
            dirty=lambda: self._fh_play_options().to_settings() != saved().to_settings())

    # ------------------------------------------------------------------ World at War, Call of Duty 2 and 4
    def _page_port_setup(self, t, pid: str) -> None:
        p, v = x360_ports.PORTS[pid], self.port_vars[pid]
        key = f"{pid}_setup"
        t.columnconfigure(0, weight=1)
        self._kit_header(t, f"Make {p.name} from your own Xbox 360 disc, then press Build. Your disc is only read, "
                            "never changed.")
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
        self._kit_disc_tip(d, 2)

        w = section(t, "What to build", 3, columns=1)
        self._grid_cols(w)
        progs = " and ".join(f"{q.name} ({q.label.lower()})" for q in p.programs)
        widgets.autowrap(ttk.Label(w, text=f"The playable {short}, recompiled on Call of Duty 3's SDK",
                                   style="Bold.TLabel")).grid(row=0, column=0, columnspan=3, sticky="w")
        vf = ttk.Frame(w)
        vf.grid(row=1, column=0, columnspan=3, sticky="w", padx=(px(26), 0), pady=(px(2), 0))
        ttk.Label(vf, text="Versions:").pack(side="left", padx=(0, px(8)))
        tip(ttk.Checkbutton(vf, text="Linux", variable=self.v_linux),
            "The same setting as Call of Duty 3's.").pack(side="left")
        tip(ttk.Checkbutton(vf, text="Windows (.exe)", variable=self.v_windows),
            "The same setting as Call of Duty 3's." + ("" if IS_WINDOWS else " Made here with llvm-mingw.")).pack(
            side="left", padx=(px(14), 0))
        hint(w, f"The same steps as Call of Duty 3: your disc is checked and its files copied; the ReXGlue SDK is "
                f"built (or reused) with the project's patches; the game's programs, {progs}, are translated to C++ "
                "and compiled. Graphics: Vulkan (and Direct3D 12 on Windows) through the SDK's GPU emulation for "
                "now; the game's native renderer comes later.").grid(
            row=2, column=0, columnspan=3, sticky="w", padx=(px(26), 0), pady=(px(2), px(4)))
        self._kit_output_row(w, 3, lambda g=pid: self._port_out(g), f"{p.subdir}/")
        hint(w, f"The game goes into {p.subdir}/ there: the disc's files (about {p.disc_size / 1e9:.0f} GB), the "
                "recompiled programs, your saves and a launcher.").grid(row=4, column=1, columnspan=2, sticky="w")
        tip(widgets.autowrap(ttk.Checkbutton(w, text="Install anything that's missing (you approve the list first; "
                                                             "recommended)", variable=self.v_install)), "The same setting as Call of Duty 3's.").grid(
            row=5, column=0, columnspan=3, sticky="w", pady=(px(4), 0))
        self._kit_more_options(t, 7, [
            ("Delete temporary files when done", self.v_cleanup, "The same setting as Call of Duty 3's."),
            ("Keep the built SDK for faster rebuilds of every game", self.v_keep_sdk,
             "The same setting as Call of Duty 3's: one SDK serves every game.")], key,
            sdk_note="The SDK built there serves every game.")
        self._kit_footer(t, 10)

    def _page_port_play(self, t, pid: str) -> None:
        p, v = x360_ports.PORTS[pid], self.port_vars[pid]
        key = f"{pid}_play"
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Play", f"Start {p.name}, and change its settings before it starts.", columns=1)
        panel: dict = {}
        self.pages[key]["on_show"] = lambda g=pid: (self._port_gs_load(g, keep_edits=True), self._update_actions(),
                                                    panel.get("refresh", int)())
        v["program"].trace_add("write", lambda *a, g=pid: self._port_gs_load(g))   # [f2-menus] each program its file
        name = p.name.split(":")[0] if pid != "waw" else "World at War"
        btn, reason_lbl = self._kit_game_box(
            t, p.name, f"Play {name}", lambda: self._play_port(pid), v["play_reason"],
            "In the game: F2 opens the settings window (display and resolution, frame limit, field of view, "
            "controls and more). The Game settings below are the same ones, kept in the game's own file: change "
            "them here or there.")
        self.port_widgets.setdefault(pid, {}).update({"play_btn": btn, "play_reason": reason_lbl})
        s = section(t, "Game settings", 3, columns=1)
        self._grid_cols(s)
        display: List[tuple] = [("check", "Fullscreen", v["fullscreen"],
                                 "Fill the screen instead of a window (the game's display mode; F2 > Video has it "
                                 "too).")]
        if p.fov:
            display.append(("choice", "Field of view", v["fov"], [NO_FOV, "65", "80", "90", "100", "110", "120"],
                            "Degrees, 40-140; Game's own leaves it to the game.", True))
        if p.max_fps:
            display.append(("number", "Frame limit", v["max_fps"], (0, p.max_fps_top, 30),
                            f"Frames a second, up to {p.max_fps_top}; 0 = {p.max_fps_zero}."))
        if IS_WINDOWS:
            display.append(("choice", "Graphics API", v["graphics_api"], list(x360_ports.GRAPHICS_APIS),
                            "auto: Vulkan, or Direct3D 12 when Vulkan can't start."))
        groups: List[Tuple[str, List[tuple]]] = [
            ("Display", display),
            ("Sound", [("check", "Mute", v["mute"], "No sound.")])]
        if p.skip_films:
            groups.append(("Cutscenes", [("check", "Skip the start-up films", v["skip_films"],
                                          "Straight to Press START.")]))

        def defaults() -> None:
            v["fullscreen"].set(False)
            v["mute"].set(False)
            v["skip_films"].set(False)
            v["fov"].set(NO_FOV)
            v["max_fps"].set(str(p.max_fps_default) if p.max_fps else "")
            v["graphics_api"].set("auto")
            v["gs_reason"].set("The game's defaults: Save writes them.")
        files = " and ".join(x360_ports.settings_file_name(p, q.name) for q in p.programs)
        panel.update(self._kit_settings(
            s, key, groups,
            intro="The game's own settings file, the one F2 writes in the game: read when this page is shown, "
                  "written by Save and before Play.",
            where=f"In the game's folder ({files}): the game reads them when it starts. Its saves are in userdata/.",
            save=lambda g=pid: self._port_gs_save(g), undo=lambda g=pid: self._port_gs_load(g),
            defaults=defaults, reason=v["gs_reason"], dirty=lambda g=pid: bool(self._port_gs_changed(g)),
            top=[("choice", "Program", v["program"], [q.label for q in p.programs],
                  "The campaign, or multiplayer when the port has it. Each has its own settings file, and the tabs "
                  "below are that file's.")]))
        self._kit_ports = getattr(self, "_kit_ports", {})
        self._kit_ports[pid] = panel

    # ------------------------------------------------------------------ Tools, on every tab
    def _kit_after_pages(self) -> None:
        """After every page is made: Call of Duty 3's profile list becomes one of several (each tab's Tools page has
        its own), and its Tools page takes it back when shown."""
        if not hasattr(self, "lb_profiles"):
            return
        self._kit_profiles: Dict[str, tuple] = getattr(self, "_kit_profiles", {})
        self._kit_profiles[games.COD3] = (self.lb_profiles, self.profile_buttons, self.v_profiles_empty)
        if "tools" in self.pages:
            self.pages["tools"]["on_show"] = lambda: self._kit_use_profiles(games.COD3)

    def _kit_use_profiles(self, gid: str) -> None:
        widgets_ = getattr(self, "_kit_profiles", {}).get(gid)
        if widgets_:
            self.lb_profiles, self.profile_buttons, self.v_profiles_empty = widgets_
        self._refresh_profiles()
        self._update_accent_reset()

    def _page_game_tools(self, t, gid: str) -> None:
        g = games.game(gid)
        key = kit_pages(gid)[0]
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Tools", f"Check this PC for {g.name}'s build, find its files, keep your settings as "
                                        "profiles, and change this tab's look.", columns=1)
        self.pages[key]["on_show"] = lambda: self._kit_use_profiles(gid)
        row = 1
        check = {games.BRO: self._kit_check_xbox, games.FH: self._kit_check_xbox}.get(gid)
        if check is None and gid in games.X360_PORTS:
            check = self._check_tools
        if check is not None:
            pc = section(t, "This PC", row, columns=1)
            hint(pc, "What the build needs (compilers, tools, libraries) and whether this PC has it. Build installs "
                     "what's missing by itself, after you approve the list.").pack(anchor="w")
            bf = ttk.Frame(pc)
            bf.pack(anchor="w", pady=(px(8), 0))
            widgets.button_row(bf, [("Check this PC", check, "List what's missing.")])
            row += 2

        f = section(t, "Folders", row, columns=1)
        hint(f, "Everything stays in the project folder unless you chose other places.").pack(anchor="w")
        bf = ttk.Frame(f)
        bf.pack(anchor="w", pady=(px(8), 0))
        for text, cmd, tooltip in self._kit_folders(gid):
            b = ttk.Button(bf, text=text, command=cmd)
            if tooltip:
                tip(b, tooltip)
        widgets.flow(bf, gap=6)   # seven buttons: they wrap on a narrow page
        row += 2

        p = section(t, "Profiles", row, columns=1)
        hint(p, "Keep your settings (discs, folders, options of every tab) under a name and switch between them. "
                "The same profiles as on Call of Duty 3's Tools page.").pack(anchor="w")
        pf = ttk.Frame(p)
        pf.pack(anchor="w", pady=(px(8), 0))
        lb = tk.Listbox(pf, height=6, borderwidth=0, highlightthickness=1, activestyle="none", exportselection=False,
                        width=28)
        lb.grid(row=0, column=0, sticky="ns")
        theme.recolor(lb)
        lb.bind("<<ListboxSelect>>", lambda e: self._update_profile_buttons())
        lb.bind("<Double-Button-1>", lambda e: self._profile_load())
        empty = tk.StringVar()
        pb = ttk.Frame(pf)
        pb.grid(row=0, column=1, sticky="n", padx=(px(10), 0))
        buttons = {}
        for k, text, cmd, tooltip in (
                ("save", "Save current as...", self._profile_save, "Keep the current settings under a name."),
                ("load", "Load", self._profile_load, "Put this profile's settings in the form."),
                ("rename", "Rename...", self._profile_rename, ""),
                ("delete", "Delete", self._profile_delete, ""),
                ("export", "Export...", self._profile_export, "Save it as a file to share or move to another PC."),
                ("import", "Import...", self._profile_import, "Add a profile from an exported file.")):
            b = ttk.Button(pb, text=text, command=cmd)
            b.pack(fill="x", pady=(0, px(4)))
            if tooltip:
                tip(b, tooltip)
            buttons[k] = b
        hint(p, var=empty).pack(anchor="w", pady=(px(4), 0))
        self._kit_profiles = getattr(self, "_kit_profiles", {})
        self._kit_profiles[gid] = (lb, buttons, empty)
        row += 2

        a = section(t, "Appearance", row, columns=1)
        hint(a, f"{g.name}'s own colors, the game's F2 themes, and a highlight color of your own, for this tab "
                "(each tab keeps its own).").grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, px(6)))
        for i, tid in enumerate(theme.palettes_for(g.palette)):
            label = theme.picker_label(tid, g.palette)
            ttk.Radiobutton(a, text=label, value=label, variable=self.v_theme,
                            command=lambda x=tid: self._set_theme(x)).grid(row=1 + i, column=0, sticky="w",
                                                                           pady=px(2))
            hint(a, theme.picker_description(tid, g.palette)).grid(row=1 + i, column=1, sticky="w",
                                                                    padx=(px(12), 0))
        af = ttk.Frame(a)
        af.grid(row=20, column=0, columnspan=3, sticky="w", pady=(px(10), px(18)))
        ttk.Label(af, text="Accent color").pack(side="left", padx=(0, px(10)))
        sw = tk.Frame(af, width=px(22), height=px(22), cursor="hand2")
        sw.c3w_role = "accent"
        sw.pack(side="left", padx=(0, px(10)))
        sw.bind("<Button-1>", lambda e: self._pick_accent())
        theme.recolor(sw)
        self.swatches.append(sw)
        b = ttk.Button(af, text="Pick...", command=self._pick_accent)
        b.pack(side="left")
        tip(b, "Buttons, the selected page and headings use it. Text in it is adjusted to stay readable.")
        b = ttk.Button(af, text="Reset accent", command=self._reset_accent)
        b.pack(side="left", padx=(px(6), 0))
        tip(b, lambda: "Back to the theme's own accent." if self.accent else "The theme's own accent is in use.")

    def _kit_folders(self, gid: str) -> List[tuple]:
        """The Folders row of a tab's Tools page: the game's own folders first."""
        def at(get: Callable[[], Optional[Path]], *sub: str) -> Callable[[], None]:
            return lambda: self._open(str(Path(get() or self.v_out.get(), *sub)))
        rows: List[tuple] = []
        if gid == games.BRO:
            rows += [("Game folder", at(self._bro_out), "bro/ in the output folder: the game, its saves and play.sh."),
                     ("Saves", at(self._bro_out, "saves"), ""),
                     ("Game logs", at(self._bro_out, "logs"),
                      "bro.log: what the game wrote at its last start; build.log: the last build.")]
        elif gid == games.FH:
            rows += [("Game folder", at(self._fh_out), "fh/ in the output folder: the game, fh.toml, saves, play.sh."),
                     ("Saves", at(self._fh_out, "saves"), ""),
                     ("Game logs", at(self._fh_out, "logs"),
                      "fh.log: what the game wrote at its last start; build.log: the last build.")]
        elif gid in games.X360_PORTS:
            rows += [("Game folder", at(lambda g=gid: self._port_out(g)), "The game's folder in the output folder."),
                     ("Game logs", at(lambda g=gid: self._port_out(g), "userdata", "logs"), "The game's own log.")]
        rows += [("Where things go", self._show_locations, "Every folder the tool uses, and what is in it."),
                 ("Output folder", lambda: self._open(self.v_out.get()), "Every game's folder, saves and logs."),
                 ("Input folder", self._open_input, "Your discs and downloads (only ever read)."),
                 ("Log folder", self._open_logs, "The setup tool's logs.")]
        return rows

    def _kit_check_xbox(self) -> None:
        """This PC, for Big Red One's and Finest Hour's builds (they need the same tools): what is missing."""
        def work():
            runner = common.Runner(self.log, threading.Event())
            tools = bro.find_tools(runner)
            problems = tools.problems()
            if problems:
                text = "Missing for the build:\n" + "\n".join(f"- {p}" for p in problems) + \
                       ("\n\nBuild installs them by itself after you approve the list (Install anything that's "
                        "missing, on Set up)." if IS_LINUX else "")
                self.log.warn("This PC, for the Xbox games' builds: " + "; ".join(problems))
            else:
                text = "Everything the build needs is installed."
                self.log.info("This PC, for the Xbox games' builds: everything is there.")
            self.q.put(("call", lambda: (self.v_status.set("Ready."),
                                         self._info("This PC", text, kind="warning" if problems else "info"))))
        self._bg(work, "Checking this PC...")

    # ------------------------------------------------------------------ About, on every tab
    def _page_game_about(self, t, gid: str) -> None:
        from .gui_pages import SHORTCUTS
        g = games.game(gid)
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, g.name, f"{g.tagline}. The {common.APP_NAME}, version {TOOL_VERSION}.", columns=1)
        d = section(t, "Documents", 1, columns=1)
        d.columnconfigure(1, weight=1)
        for r, (rel, label, what) in enumerate(GAME_DOCS.get(gid, []) + COMMON_DOCS):
            path = self._doc_path(rel)
            b = ttk.Button(d, text=label, width=20, command=lambda p=path: self._open_file(p))
            b.grid(row=r, column=0, sticky="w", pady=px(2))
            if path is None:
                b.configure(state="disabled")
                tip(b, f"{rel} isn't in this copy of the tool (it's in the project folder).")
            else:
                tip(b, str(path))
            hint(d, what).grid(row=r, column=1, sticky="w", padx=(px(12), 0))
        k = section(t, "Keyboard shortcuts", 3, columns=1)
        for r, (keys, what) in enumerate(SHORTCUTS):
            keys = keys.format(n=min(9, len(self.page_lists.get(gid, ())) or 1))
            ttk.Label(k, text=keys, style="Bold.TLabel").grid(row=r, column=0, sticky="w", pady=px(1))
            ttk.Label(k, text=what).grid(row=r, column=1, sticky="w", padx=(px(16), 0))
        lic = section(t, "Licenses and credits", 5, columns=1)
        hint(lic, "This project's code is under the MIT license. " + CREDITS.get(gid, "") +
             " Every download is listed in docs/SOURCES.md.").pack(anchor="w")
        hint(lic, TRADEMARK).pack(anchor="w", pady=(px(8), px(18)))

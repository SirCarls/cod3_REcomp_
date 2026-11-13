"""The setup tool's pages besides Set up and Log: Play (with the game's
settings), Textures, Mods & maps, Tools (this PC, folders, repair,
profiles, appearance) and About.

A mixin for gui.App: it uses the App's variables (v_out, v_iso, ...) and
helpers (_bg, _info, _open, _start_build, ...).
"""
from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import threading
import time
import tkinter as tk
import webbrowser
from pathlib import Path
from tkinter import filedialog, ttk
from typing import Dict, List, Optional

from . import (common, deps, game_settings, pc_check, profiles, repair, rippers, theme, upscale, user_textures,
               waw, widgets)
from . import voices  # [voice] another disc's voices
from . import games  # [waw-decomp] the tabs built on Call of Duty 3's SDK (X360_PORTS)
from . import censored  # [acct2-decomp] the censored campaign
from . import censor_builtin  # [acct2-decomp] the built-in censor mode
from . import disc_needs  # [acct2-decomp] which options need which disc
from . import build_needs  # [acct2-decomp] what the build with these options needs on this PC
from .common import IS_LINUX, IS_WINDOWS, SetupError
from .pipeline import TOOL_VERSION, upscaled_size
from .widgets import ScrollFrame, hint, px, section, tip

# What each automatic repair redoes, in plain words (repair.Issue.fix);
# repair.FIX_LABELS when it has them.
FIX_TEXT = {
    "rebuild_game": "Build the game again",
    "rebuild_windows": "Build the Windows version again",
    "repack_hd": "Pack the HD textures again",
    "rerip": "Rip the game files again",
    "relaunchers": "Write the launchers again",
}
SHORTCUTS = [("Ctrl+B", "The tab's main button (Build, Rip, Make a copy)"), ("Ctrl+L", "Show the log"),
             ("Ctrl+1 to Ctrl+{n}", "Go to a page of the tab"), ("Ctrl+Tab", "Next game tab (Shift: back)"),
             ("F5", "Check this page again"), ("Ctrl+F", "Search the log"),
             ("Esc", "Close a dialog"), ("Enter", "Pick a dialog's main button")]
# The game's settings on the Play page, in groups (game_settings.SETTINGS
# keys; one the list doesn't name goes under "More").
GS_GROUPS = [
    ("Display", ["fullscreen", "cod3_render_resolution", "cod3_render_method",
                 "cod3_render_percent", "cod3_live_resolution", "cod3_aspect_ratio", "vsync", "cod3_max_fps"]),
    ("Graphics", ["swap_post_effect", "anisotropic_override", "cod3_shadow_quality",
                  "cod3_depth_of_field", "cod3_motion_blur",  # [post-effects]
                  "cod3_hd_textures", "cod3_user_textures", "cod3_graphics_api"]),
    ("Sound and controls", ["cod3_volume", "cod3_voice_language", "user_language", "cod3_glyph_style",  # [voice]
                            "cod3_glyph_color_mode",  # [claude-9b-think]; [claude-setup] was cod3_glyph_colors
                            "cod3_use_hold_ms"]),  # [acct2-decomp] 2026-11-05 the Use hold (game/use_hold.h)
    # [acct2-ocr] 2026-11-07 the text's language and its kinds (F2 > Sound > Text), after the voices' rows
    ("Text and subtitles", ["cod3_text_language", "cod3_text_subtitles", "cod3_text_menus", "cod3_text_prompts",
                            "cod3_text_objectives", "cod3_text_loading", "cod3_text_credits",
                            "cod3_text_film_cards", "cod3_font_fill"]),
    # [acct2-decomp] the cutscene rows had been under "Sound and controls"
    ("Cutscenes", ["cod3_skip_movies", "cod3_letterbox", "cod3_skip_prompt_position",
                   "cod3_skip_hold_ms"]),  # [claude-9b-think]
    ("Player", ["user_name", "user_name_2", "user_name_3", "user_name_4"]),  # [claude-9b] SDK patches 0070, 0074
]


def clean_path_text(text: str) -> str:
    """[acct2-decomp] A path as typed or pasted: spaces and the quotes of Windows'
    "Copy as path" around it taken off, ~ expanded (as the pipeline does)."""
    s = (text or "").strip()
    if len(s) >= 2 and s[0] == s[-1] and s[0] in "\"'":
        s = s[1:-1].strip()
    return os.path.expanduser(s) if s else ""


def _fix_label(fix: str) -> str:
    return getattr(repair, "FIX_LABELS", {}).get(fix) or FIX_TEXT.get(fix, fix)


class Pages:
    # ------------------------------------------------------------------ shared
    def _update_page_states(self) -> None:
        """Buttons that can't do anything right now are off, with the reason
        next to them."""
        if hasattr(self, "btn_play"):
            exe, why = self._play_exe()
            if exe and self._job_running():   # [acct2-decomp] a build or repair may be replacing the game
                exe, why = None, "A job is running: wait for it to finish (or Cancel it)."
            self.btn_play.configure(state="normal" if exe else "disabled")
            self.v_play_reason.set(why)
            self.lbl_play_reason.configure(style="Good.TLabel" if exe else "Warn.TLabel")
        if hasattr(self, "cb_censored"):   # [acct2-decomp]
            self.cb_censored.configure(state="disabled" if self._job_running() else "normal")
        if hasattr(self, "btn_user_pack"):
            rip = self._installed_rip()
            self.btn_user_pack.configure(state="normal" if rip else "disabled")
            self.v_user_pack_reason.set("" if rip else "Build first: the pack is made with the ripper the build "
                                                       "puts in the output folder's tools/.")
        self._update_repair_state()
        self._update_game_settings_state()
        self._update_profile_buttons()
        self._update_accent_reset()

    def _output_changed(self) -> None:
        """The output folder changed (or a build finished there)."""
        self._gs_loaded_for = None
        self._repair_report = None
        self._show_repair(None)
        self._update_page_states()
        self._log_refresh_sources()
        if getattr(self, "page", "") == "play":
            self._load_game_settings()

    def _refresh_page(self, key: str) -> None:
        """F5 on a page."""
        if key == "play":
            self._update_page_states()
            self._load_game_settings(keep_edits=True)
        elif key == "tools":
            self._refresh_profiles()
            if getattr(self, "_repair_report", None) is not None:
                self._run_repair_check()
        elif key == "log":
            self._log_reload()
        elif key == "textures":
            self._update_texture_state()
        elif key == "maps":
            self._refresh_maps()
        elif key == "waw_content":
            self._validate_waw()
        elif key == "bro_setup":   # [acct2-bro]
            self._bro_check_disc()
        elif key.endswith("_setup") and key[:-6] in games.X360_PORTS:   # [waw-decomp]
            self._port_check_disc(key[:-6])
        elif key.endswith("_play") and key[:-5] in games.X360_PORTS:
            self._update_actions()
        elif key == "bro_play":
            self._update_actions()
        elif key.endswith("_copy") and key[:-5] in getattr(self, "pc", {}):
            self._pc_check_install(key[:-5])
        elif key.endswith("_server") and key[:-7] in getattr(self, "pc", {}):
            self._pc_refresh(key[:-7])

    def _theme_pages(self, c: Dict[str, str]) -> None:
        for sw in getattr(self, "swatches", []):
            try:
                sw.configure(background=c["accent"])
            except tk.TclError:
                pass
        self._update_accent_reset()

    def _out_path(self) -> Optional[Path]:
        out = clean_path_text(self.v_out.get())   # [acct2-decomp] ~ and pasted quotes, as the pipeline reads it
        return Path(out) if out else None

    def _job_running(self) -> bool:  # [acct2-decomp]
        return bool(self.worker and self.worker.is_alive())

    # ------------------------------------------------------------------ Play page
    def _page_play(self, t) -> None:
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Play", "Start the game, and change its settings before it starts.", columns=1)
        self.pages["play"]["on_show"] = self._on_show_play

        g = section(t, "Call of Duty 3", 1, columns=1)
        self._grid_cols(g)
        top = ttk.Frame(g)
        top.grid(row=0, column=0, columnspan=3, sticky="ew")
        self.btn_play = ttk.Button(top, text="Play Call of Duty 3", style="Accent.TButton", command=self._play_cod3)
        self.btn_play.pack(side="left")
        tip(self.btn_play, lambda: (f"Start {self._play_exe()[0]}" if self._play_exe()[0] else
                                    self.v_play_reason.get()))
        self.v_play_reason = tk.StringVar()
        self.lbl_play_reason = ttk.Label(top, textvariable=self.v_play_reason, style="Hint.TLabel",
                                         wraplength=px(440), justify="left")
        self.lbl_play_reason.pack(side="left", padx=(px(14), 0))
        # Windowed or fullscreen is the game's own Display mode (Game settings
        # below, F2 in the game): one setting, so the last change made in
        # either place is the one the next start uses. (An older "Start in a
        # window" box sent --fullscreen=false at every start, which undid F2.)
        hint(g, "In game: ~ console (Shift+~ full), F2 settings (resolution up to 8K, FPS, AA/FSR, FOV, crosshair, "
                "mouse). FOV is also in Options > Gameplay.").grid(row=2, column=0, columnspan=3, sticky="w",
                                                                   pady=(px(4), 0))
        # [acct2-decomp] Extra arguments (for testing) after the player's own rows and the discs note: row 7.
        ttk.Label(g, text="Extra arguments").grid(row=7, column=0, sticky="w", pady=(px(10), px(3)))
        e = ttk.Entry(g, textvariable=self.v_cod3_args)
        e.grid(row=7, column=1, columnspan=2, sticky="ew", pady=(px(10), px(3)))
        tip(e, "For testing: passed to the game as they are, for each start from here. They win over the game "
               "settings for that start and are never saved in them. Quote a value with spaces "
               "(--name=\"My Name\"). Leave empty normally.")
        # [voice] Another disc's voices (c3w/voices.py); Voices under Game settings picks the language.
        vb = ttk.Button(g, text="Add voices from another disc...", command=self._add_voices)
        vb.grid(row=3, column=0, columnspan=3, sticky="w", pady=(px(10), 0))
        tip(vb, "Needs another disc: German voices come from the German disc; French, Spanish and Italian from the "
                "Europe (Fr, Es, It) disc; English from the USA/Europe disc. Copies them from its .iso into the "
                "game, next to its own; then choose them with Voices under Game settings, or in F2 > Game > "
                "Sound.")  # [acct2-decomp] which disc gives which language
        # [rexglue-native-settings] 2026-11-11 (owner 08:28: "a texture and censoring tab"): the censored campaign's
        # box moved, unchanged, to the Textures & censoring page (_page_textures), with the built-in censor.
        # [acct2-decomp] Which options need which disc, and where this game stands (c3w/disc_needs.py;
        # owner 2026-11-05 08:41: tell the user which options are disc dependent). Filled by _refresh_disc_needs.
        ttk.Label(g, text="Discs", style="Accent.TLabel").grid(row=5, column=0, sticky="w", pady=(px(12), px(2)))
        self.v_disc_needs = tk.StringVar(value="")
        self._disc_found: Dict[str, Path] = {}
        self._voices_have: Optional[Dict[str, int]] = None
        hint(g, var=self.v_disc_needs).grid(row=6, column=0, columnspan=3, sticky="w")

        s = section(t, "Game settings", 3, columns=1)
        self._grid_cols(s)
        self._game_settings_section(s)

        d = section(t, "Ripped game files", 5, columns=1)
        hint(d, "Textures (DDS + PNG), meshes (glTF) and materials (JSON), in open formats.").pack(anchor="w")  # [f2-menus]
        b = ttk.Button(d, text="Open ripped files",  # [acct2-decomp] nothing chosen: not the current folder
                       command=lambda: self._open(str(self._out_path() / "data") if self._out_path() else ""))
        b.pack(anchor="w", pady=(px(8), px(18)))
        tip(b, "The data folder in the output folder.")

    def _add_voices(self) -> None:  # [voice]
        """Copies another Call of Duty 3 disc's voice files into the built
        game (c3w/voices.py: never overwrites anything)."""
        out = self._out_path()
        try:
            if out is None:
                raise SetupError("no output folder")
            voices.assets_folder(out)
        except SetupError:
            self._info("Build the game first", "Voices go into a built game: build Call of Duty 3 on Set up "
                                               "first.", kind="warning")
            return
        if self._job_running():   # [acct2-decomp] before the file dialog, not after it
            self.v_status.set("Busy: wait for the job running now, or cancel it.")
            return
        own = disc_needs.game_disc(out)
        disc = filedialog.askopenfilename(
            title="Another Call of Duty 3 disc (German, or Europe (Fr, Es, It), or USA/Europe)", parent=self.root,
            filetypes=[("Disc images", "*.iso *.ISO"), ("All files", "*")],
            **self._disc_dialog_place([k for k in (disc_needs.GERMANY, disc_needs.FR_ES_IT, disc_needs.USA)
                                       if own is None or k != own.key]))  # [acct2-decomp]
        if not disc:
            return

        def work(runner: common.Runner) -> str:
            # [acct2-decomp] what the disc is, before anything is copied
            picked = disc_needs.identify(Path(disc))
            if own is not None and picked.key == own.key:
                raise SetupError(f"{Path(disc).name} is {disc_needs.RELEASES[own.key].short}, the one your game was "
                                 "built from: its voices are in the game already. German voices come from the "
                                 "German disc; French, Spanish and Italian from the Europe (Fr, Es, It) disc.")
            r = voices.add_voices(Path(disc), out, runner.log, runner.cancel)
            have = ", ".join(voices.LANGUAGES[c] for c, n in r["languages"].items() if n)
            first = (f"Copied {r['copied']} voice files from {r['disc']} ({common.human(r['bytes'])})."
                     if r["copied"] else f"The game already has the voices of {r['disc']}.")
            return f"{first} Voices the game can use: {have}. Choose one with Voices under Game settings."
        self._start_task("Adding voices", work, actions=[])

    def _refresh_censored(self) -> None:  # [acct2-decomp]
        """The Censored campaign box as the game's files are (by size and
        what waits in assets/censored/: instant), and the built-in censor's
        choice as saved."""
        out = self._out_path()
        own = None
        try:
            st = censored.status(out) if out is not None else None
            on, own = (st.censored, st.own) if st is not None else (False, None)
        except (SetupError, OSError):
            on = False
        if hasattr(self, "v_censored"):   # [rexglue-native-settings] 2026-11-11 the box is on the Textures page now
            self.v_censored.set(on)
        if hasattr(self, "cb_censored"):   # [acct2-decomp] say which disc it needs, for this game
            self.cb_censored.configure(text="Censored campaign (the German release's textures; " + (
                "your game's own: off needs the USA/Europe or Fr/Es/It disc once)" if own == censored.GERMAN else
                "needs the German disc once)"))
        self._censor_builtin_load()
        self._refresh_disc_needs()

    # [acct2-decomp] The built-in censor (c3w/censor_builtin.py), joined with your own pictures in the
    # Textures page's section (owner 2026-11-05 07:24): the choice is saved as it is made, and every
    # build of the texture pack (the button there, or a full build) makes or takes back its pictures.
    def _censor_builtin_box(self, parent) -> None:
        """A tick box; the two designs show only while it is ticked."""
        labels = [label for _, label in censor_builtin.DESIGNS]
        self.v_cb_on = tk.BooleanVar(value=False)
        self._cb_shown = {"flags": "rainbow", "insignia": "blend"}
        self._cb_colors = {"flags": "#808080", "insignia": "#808080"}
        self.v_cb_flags = tk.StringVar(value=censor_builtin.LABELS["rainbow"])
        self.v_cb_insignia = tk.StringVar(value=censor_builtin.LABELS["blend"])
        self._cb_dirty = False
        box = ttk.Frame(parent)
        box.pack(anchor="w", fill="x")
        cb = ttk.Checkbutton(box, text="Cover the Nazi flags and insignia", variable=self.v_cb_on,
                             command=self._censor_builtin_toggle)
        cb.pack(anchor="w")
        tip(cb, "No disc needed. Campaign and multiplayer.")
        self.cb_designs = ttk.Frame(box)
        for col, (key, title, var) in enumerate((("flags", "Flags", self.v_cb_flags),
                                                  ("insignia", "Insignia", self.v_cb_insignia))):
            ttk.Label(self.cb_designs, text=title).grid(row=0, column=2 * col, sticky="w",
                                                        padx=(px(16) if col else 0, px(6)))
            c = ttk.Combobox(self.cb_designs, textvariable=var, values=labels, state="readonly", width=26)
            c.grid(row=0, column=2 * col + 1, sticky="w")
            c.bind("<<ComboboxSelected>>", lambda _e, k=key, v=var: self._censor_builtin_pick(k, v))
            tip(c, lambda k=key: self._censor_builtin_tip(k))
        self.v_cb_state = tk.StringVar()
        self.lbl_cb_state = hint(box, var=self.v_cb_state, style="Accent.TLabel")

    def _censor_builtin_tip(self, key: str) -> str:
        text = ("The whole flag or banner." if key == "flags" else
                "Only where the symbol is. Blend in paints it out.")
        if self._cb_shown.get(key) == "custom":
            text += f" Colour {self._cb_colors.get(key, '')}."
        return text

    def _censor_builtin_show(self) -> None:
        """The designs while the box is ticked; the note while a change isn't packed yet."""
        if not hasattr(self, "cb_designs"):
            return
        self.cb_designs.pack_forget()
        self.lbl_cb_state.pack_forget()
        if self.v_cb_on.get():
            self.cb_designs.pack(anchor="w", padx=(px(26), 0), pady=(px(4), 0))
        if self.v_cb_state.get():
            self.lbl_cb_state.pack(anchor="w", padx=(px(26), 0), pady=(px(4), 0))

    def _censor_builtin_toggle(self) -> None:
        if self.v_cb_on.get() and self._cb_shown == {"flags": "off", "insignia": "off"}:
            self._cb_shown = {"flags": "rainbow", "insignia": "blend"}
            self.v_cb_flags.set(censor_builtin.LABELS["rainbow"])
            self.v_cb_insignia.set(censor_builtin.LABELS["blend"])
        self._censor_builtin_save()

    def _censor_builtin_pick(self, key: str, var: tk.StringVar) -> None:
        """A design chosen; "A colour of your choice" asks for the colour."""
        design = {label: k for k, label in censor_builtin.DESIGNS}.get(var.get(), "off")
        if design == "custom":
            from tkinter import colorchooser
            picked = colorchooser.askcolor(color=self._cb_colors.get(key, "#808080"), parent=self.root,
                                           title="The colour for the " + key)
            if not picked or not picked[1]:
                var.set(censor_builtin.LABELS[self._cb_shown.get(key, "off")])
                return
            self._cb_colors[key] = picked[1]
        self._cb_shown[key] = design
        self._censor_builtin_save()

    def _censor_builtin_save(self) -> None:
        """Saves the choice for the next texture pack build."""
        on = self.v_cb_on.get()
        s = censor_builtin.Settings(flags=self._cb_shown["flags"] if on else "off",
                                    insignia=self._cb_shown["insignia"] if on else "off",
                                    flags_color=self._cb_colors.get("flags", "#808080"),
                                    insignia_color=self._cb_colors.get("insignia", "#808080"))
        out = self._out_path()
        if out is None or not out.is_dir():
            self.v_cb_state.set("Build the game first (Set up), then the texture pack.")
        else:
            try:
                censor_builtin.save_settings(out / "data" / "cod3", s)
                self._cb_dirty = True
                # [rexglue-native-settings] 2026-11-11 Play builds the pack first now (_play_builds_pack_first)
                self.v_cb_state.set("Saved. Play builds the texture pack first (or press Build texture pack now).")
            except (OSError, SetupError) as e:
                self.v_cb_state.set(f"Not saved: {e}")
        self._censor_builtin_show()

    def _censor_builtin_load(self) -> None:
        """The choice as saved in the output folder."""
        if not hasattr(self, "v_cb_on"):
            return
        out = self._out_path()
        s = censor_builtin.load_settings(out / "data" / "cod3") if out is not None else censor_builtin.Settings()
        self.v_cb_on.set(s.on)
        if s.on:
            self._cb_shown = {"flags": s.flags, "insignia": s.insignia}
            self._cb_colors = {"flags": s.flags_color, "insignia": s.insignia_color}
            self.v_cb_flags.set(censor_builtin.LABELS[s.flags])
            self.v_cb_insignia.set(censor_builtin.LABELS[s.insignia])
        if not self._cb_dirty:
            self.v_cb_state.set("")
        self._censor_builtin_show()

    def _toggle_censored(self) -> None:  # [acct2-decomp]
        """Swaps the campaign's censored files in or out (c3w/censored.py),
        asking for the disc with the other version the first time."""
        on = self.v_censored.get()
        out = self._out_path()
        if self._job_running():
            self.v_censored.set(not on)
            self.v_status.set("Busy: wait for the job running now, or cancel it.")
            return
        try:
            if out is not None and game_settings.game_running(out):
                self.v_censored.set(not on)
                self._info("Close the game first", "Call of Duty 3 is running: close it, then switch the censored "
                                                   "campaign (the game has these files open).", kind="warning")
                return
        except Exception:  # noqa: BLE001 - best effort, as _play_cod3
            pass
        try:
            if out is None:
                raise SetupError("Build Call of Duty 3 on Set up first.")
            st = censored.status(out)
        except SetupError as e:
            self.v_censored.set(not on)
            self._info("Censored campaign", str(e), kind="warning")
            return
        disc = ""
        wanted = (disc_needs.GERMANY,) if on else (disc_needs.USA, disc_needs.FR_ES_IT)   # [acct2-decomp]
        if not st.ready(censored.GERMAN if on else censored.UNCUT):
            which = "the German disc" if on else "the USA/Europe or Europe (Fr, Es, It) disc"
            disc = filedialog.askopenfilename(title=f"Call of Duty 3: {which} (for the censored campaign)",
                                              parent=self.root,
                                              filetypes=[("Disc images", "*.iso *.ISO"), ("All files", "*")],
                                              **self._disc_dialog_place(list(wanted)))  # [acct2-decomp]
            if not disc:
                self.v_censored.set(not on)
                return

        def work(runner: common.Runner) -> str:
            if disc:   # [acct2-decomp] the right disc, before anything is copied
                disc_needs.check_choice(Path(disc), wanted, "The censored campaign" if on else
                                        "Switching the censored campaign off")
                censored.add(Path(disc), out, runner.log, runner.cancel)
            r = censored.set_censored(out, on, runner.log, runner.cancel)
            if r["lacking"] or r["changed"]:
                raise SetupError(censored.status(out).summary())
            first = ("The campaign now has the German release's textures (insignia taken off)." if on else
                     "The campaign now has the uncut textures.")
            return f"{first} Text, voices and multiplayer are as before. From the next level you load."
        self._start_task("Censored campaign", work, actions=[])

    # [acct2-decomp] Which options need which disc (c3w/disc_needs.py), on the Play page.
    def _disc_dialog_place(self, keys: List[str]) -> dict:
        """Where a disc picker opens: the input folder, with the first disc of `keys` (release keys) found
        there picked (from the last check of the input folder)."""
        place: dict = {}
        folder = getattr(self, "input_dir", None)
        if folder is not None and Path(folder).is_dir():
            place["initialdir"] = str(folder)
        found = getattr(self, "_disc_found", {}) or {}
        hit = next((found[k] for k in keys if k in found), None)
        if hit is not None:
            place["initialdir"], place["initialfile"] = str(Path(hit).parent), Path(hit).name
        return place

    def _refresh_disc_needs(self) -> None:
        """The Play page's Discs note and the note under Voices, worked out off the window's thread (it reads
        the game's files and the input folder's discs)."""
        if not hasattr(self, "v_disc_needs"):
            return
        out, folder = self._out_path(), getattr(self, "input_dir", None)
        self._disc_gen = getattr(self, "_disc_gen", 0) + 1
        gen = self._disc_gen

        def work() -> None:
            try:
                found = disc_needs.found_discs(folder)
            except OSError:
                found = {}
            try:
                text = disc_needs.text(out, folder, found)
            except Exception as e:  # noqa: BLE001 - a note, never a reason for the page to fail
                text = f"Couldn't check the discs: {type(e).__name__}: {e}"
            have = None
            try:
                if out is not None and disc_needs.game_disc(out) is not None:
                    have = voices.languages_in(out / "assets" if (out / "assets").is_dir() else out)
            except OSError:
                have = None
            texts = None   # [f2-menus] 2026-11-11 the text languages the game's folder has (assets/text/<code>.txt)
            try:
                if out is not None and disc_needs.game_disc(out) is not None:
                    tdir = (out / "assets" if (out / "assets").is_dir() else out) / "text"
                    texts = {p.stem for p in tdir.glob("*.txt")} if tdir.is_dir() else set()
            except OSError:
                texts = None

            def show() -> None:
                if gen != getattr(self, "_disc_gen", 0):
                    return   # a newer check is on its way
                self._disc_found, self._voices_have = found, have
                self.v_disc_needs.set(text)
                self._update_voice_note()
                self._text_have = texts   # [f2-menus] 2026-11-11
                self._update_text_note()
            self.q.put(("call", show))
        threading.Thread(target=work, daemon=True).start()

    def _update_voice_note(self) -> None:
        """Under Voices: the chosen language when the game doesn't have it, and which disc gives it."""
        if not hasattr(self, "v_voice_note") or "cod3_voice_language" not in getattr(self, "_gs_vars", {}):
            return
        st, var, _ = self._gs_vars["cod3_voice_language"]
        code = self._gs_value(st, var)
        have = self._voices_have
        if have is None or code in (None, "disc") or have.get(code):
            self.v_voice_note.set("")
            return
        name = disc_needs.LANGUAGE_NAMES.get(code, code)
        needs = disc_needs.RELEASES[disc_needs.VOICE_DISC.get(code, disc_needs.USA)].short
        self.v_voice_note.set(f"{name} isn't in this game yet: it needs {needs} (Add voices from another disc, "
                              "above). Until then the game's own voices play.")

    def _update_text_note(self) -> None:
        """[f2-menus] 2026-11-11 Under Text, as under Voices: the chosen language when the game's folder has no text
        file for it (they are made on this PC and come with the next build; not every language is there)."""
        if not hasattr(self, "v_text_note") or "cod3_text_language" not in getattr(self, "_gs_vars", {}):
            return
        st, var, _ = self._gs_vars["cod3_text_language"]
        code = self._gs_value(st, var)
        have = getattr(self, "_text_have", None)
        if have is None or code in (None, "disc") or code in have:
            self.v_text_note.set("")
            return
        name = next((label for value, label in st.choices if value == code), code)
        self.v_text_note.set(f"{name} has no text file in this game yet (assets/text/{code}.txt), so the game's "
                             "own text shows until it is there.")

    def _on_show_play(self) -> None:
        # The game may have changed its settings (F2) or been started or
        # closed since: read them again, keeping changes not saved yet.
        self._update_page_states()
        self._load_game_settings(keep_edits=self._gs_loaded_for == self.v_out.get().strip())
        self._refresh_censored()   # [acct2-decomp]

    def _play_exe(self):
        """(the game for this system, "") or (None, why it can't start)."""
        out = self._out_path()
        if out is None:
            return None, "Choose an output folder on Set up first."
        m = self._manifest()
        # The version for this system (an output folder can hold both).
        rel = m.get("cod3_exe_windows" if IS_WINDOWS else "cod3_exe_linux")
        if not rel and "cod3_exe_linux" not in m and "cod3_exe_windows" not in m and \
                m.get("platform", "").startswith("win" if IS_WINDOWS else "linux"):
            rel = m.get("cod3_exe")   # built by an older version of this tool
        exe = out / rel if rel else None
        system = "Windows" if IS_WINDOWS else "Linux"
        if not exe or not exe.exists():
            return None, (f"Not built yet: tick the {system} version on Set up and press Build.")
        return exe, f"Ready: the {system} version is built."

    def _migrate_windowed(self, out: Path) -> None:
        """An older version's "Start in a window" box (the tool's own setting,
        sent as --fullscreen=false at every start) becomes the game's Display
        mode once, unless the game's settings already say."""
        if not self.v_cod3_windowed.get():
            return
        try:
            if "fullscreen" not in game_settings.read_values(out):
                game_settings.save(out, {"fullscreen": False})
                self.log.info("\"Start in a window\" is now the game's own setting: Display mode = Windowed "
                              "(Game settings here, F2 > Video in the game).")
        except SetupError as e:
            self.log.warn(f"Couldn't turn \"Start in a window\" into the game's Display mode: {e}")
            return
        self.v_cod3_windowed.set(False)
        self._gs_loaded_for = None   # read the file again, with the new line

    def _play_cod3(self, pack_ready: bool = False) -> None:
        # [rexglue-native-settings] 2026-11-11 pack_ready: the texture pack was just built for this start
        # (_play_builds_pack_first calls this again when it is done); both Play buttons call this with nothing.
        exe, why = self._play_exe()
        if exe is None:
            self._info("Not built yet", f"No {'Windows' if IS_WINDOWS else 'Linux'} build of Call of Duty 3 "
                                        f"found.\n{why}", kind="warning")
            return
        out = self._out_path()   # [acct2-decomp] as _play_exe found it
        try:
            running = game_settings.game_running(out)
        except Exception:  # noqa: BLE001 - best effort, as on the Play page
            running = False
        if running:
            self._info("Already running", "Call of Duty 3 is already running. Close it first to start it again "
                                          "(its settings are saved when you change them in the game).",
                       kind="warning")
            return
        # [rexglue-native-settings] 2026-11-11 (owner 07:33 "insignia censoring doesn't work"): the insignia censor's
        # choice and the player's own pictures reach the game only through the user texture pack; build it first
        # when it is out of date, then this starts the game.
        if not pack_ready and self._play_builds_pack_first(out):
            return
        self._migrate_windowed(out)
        args = [str(exe)]
        if self.v_cod3_args.get().strip():
            try:
                args += shlex.split(self.v_cod3_args.get(), posix=not IS_WINDOWS)
            except ValueError:   # [acct2-decomp] an unclosed quote: say so instead of doing nothing
                self._info("Not started", "Extra arguments: a quote isn't closed.", kind="warning")
                return
        env = dict(os.environ)
        if not IS_WINDOWS and not os.access(exe, os.X_OK):
            # Built on Windows (WSL) or copied from a Windows drive: no exec bit.
            try:
                exe.chmod(exe.stat().st_mode | 0o755)
            except OSError:
                pass
        if exe.suffix.lower() == ".appimage" and not (shutil.which("fusermount") or shutil.which("fusermount3")):
            env["APPIMAGE_EXTRACT_AND_RUN"] = "1"  # no FUSE: run the AppImage without mounting it
        if self._gs_dirty() and not self._save_game_settings(quiet=True):
            return   # said why; nothing started with settings that weren't saved
        self._save()
        self.log.info("Launching: " + " ".join(args))
        kwargs = {} if IS_WINDOWS else {"start_new_session": True}
        started = time.monotonic()

        def ended(code: int) -> None:
            # On the collecting thread: the log line goes through the UI queue.
            took = time.monotonic() - started
            self.q.put(("call", lambda: self.log.info(
                f"The game process started here exited after {took / 60:.1f} min (exit code {code}).")))
        try:
            common.start_and_collect(args, on_exit=ended, cwd=str(out), env=env, **kwargs)
            self.v_status.set("Call of Duty 3 is starting...")
            self._gs_launched = time.monotonic()   # _gs_watch follows it until it ends
        except OSError as e:
            self._info("Could not start", f"The game didn't start.\n{e}\n\nIf the output folder is on a drive "
                                          "mounted without execute permission, move it to your home folder.",
                       kind="error")

    # ------------------------------------------------------------------ game settings (Play page)
    def _game_settings_section(self, s) -> None:
        """The game's own settings (game_settings.py: the game's cod3.toml)
        in groups, one row each: a list, a box or a number, with its help
        under it. Every control and help text lines up in one column."""
        self._gs_vars: Dict[str, tuple] = {}
        self._gs_loaded: Dict[str, object] = {}
        self._gs_loaded_for: Optional[str] = None
        system = "windows" if IS_WINDOWS else "linux"
        shown = {st.key: st for st in game_settings.SETTINGS if not st.platforms or system in st.platforms}
        hint(s, "Saved in the game's settings file; the game reads them when it starts. F2 in the game has them "
                "all, and more.").grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, px(2)))
        r = 1
        if not shown:
            hint(s, "No game settings to show.").grid(row=r, column=0, columnspan=3, sticky="w")
            r += 1
        grouped = [(title, [shown[k] for k in keys if k in shown]) for title, keys in GS_GROUPS]
        placed = {st.key for _, group in grouped for st in group}
        grouped.append(("More", [st for k, st in shown.items() if k not in placed]))
        for title, group in grouped:
            if not group:
                continue
            ttk.Label(s, text=title, style="Accent.TLabel").grid(row=r, column=0, sticky="w", pady=(px(12), px(2)))
            r += 1
            for st in group:
                r = self._gs_row(s, st, r)
        bar = ttk.Frame(s)
        bar.grid(row=r, column=1, columnspan=2, sticky="w", pady=(px(14), px(4)))
        self.btn_gs_save = ttk.Button(bar, text="Save game settings", style="Accent.TButton",
                                      command=self._save_game_settings)
        self.btn_gs_save.pack(side="left")
        tip(self.btn_gs_save, lambda: self.v_gs_reason.get() or "Write these to the game's settings file.")
        self.btn_gs_undo = ttk.Button(bar, text="Undo changes", command=self._load_game_settings)
        self.btn_gs_undo.pack(side="left", padx=(px(6), 0))
        tip(self.btn_gs_undo, "Back to what the settings file has now.")
        self.btn_gs_defaults = ttk.Button(bar, text="Game defaults", command=self._game_settings_defaults)
        self.btn_gs_defaults.pack(side="left", padx=(px(6), 0))
        tip(self.btn_gs_defaults, "Fill in the game's defaults (saved when you press Save).")
        self.v_gs_reason = tk.StringVar()
        self.lbl_gs_reason = hint(s, var=self.v_gs_reason, style="Hint.TLabel")
        self.lbl_gs_reason.grid(row=r + 1, column=1, columnspan=2, sticky="w")
        self._gs_running = False
        self._gs_loading = self._gs_watching = False
        self._gs_mtime: Optional[int] = None
        self._gs_launched: Optional[float] = None
        self.root.after(2000, self._gs_watch)
        if not shown:
            for b in (self.btn_gs_save, self.btn_gs_undo, self.btn_gs_defaults):
                b.configure(state="disabled")

    def _gs_row(self, s, st, r: int) -> int:
        """One game setting: label | control, help under the control."""
        if st.kind == "bool":
            var = tk.BooleanVar(value=bool(st.default))
            w = ttk.Checkbutton(s, text=st.label, variable=var)
        else:
            ttk.Label(s, text=st.label).grid(row=r, column=0, sticky="w", pady=(px(5), 0))
            if st.kind == "choice":
                labels = [label for _, label in st.choices]
                var = tk.StringVar(value=self._gs_label(st, st.default))
                # [claude-9b] A setting that also takes values of the player's own (an aspect ratio
                # such as 24:10, a render size such as 2560x1080) can be typed into (owner 2026-10-26:
                # the custom aspect ratio boxes weren't respected: the list only took its own rows).
                w = ttk.Combobox(s, textvariable=var, state="normal" if st.also else "readonly",
                                 values=labels)
                widgets.fit_list_width(w, labels)
            elif st.choices:
                # A number with suggestions (the frame rate limit): pick one or type your own.
                var = tk.StringVar(value=self._gs_label(st, st.default))
                w = ttk.Combobox(s, textvariable=var, values=[label for _, label in st.choices], width=12)
            else:
                var = tk.StringVar(value=str(st.default if st.default is not None else st.min))
                w = ttk.Spinbox(s, from_=st.min, to=st.max, textvariable=var, width=8,
                                increment=5 if st.max - st.min >= 100 else 1)
        w.grid(row=r, column=1, columnspan=2, sticky="w", pady=(px(5), 0))
        if st.help:   # [acct2-decomp] shown once, under the control (it was a tooltip as well)
            hint(s, st.help, style="Small.TLabel", wrap=560).grid(row=r + 1, column=1, columnspan=2, sticky="w",
                                                                  padx=(px(24), 0) if st.kind == "bool" else 0)
        self._gs_vars[st.key] = (st, var, w)
        var.trace_add("write", lambda *a: self._update_game_settings_state())
        if st.key == "cod3_voice_language":   # [acct2-decomp] a language the game doesn't have: which disc gives it
            self.v_voice_note = tk.StringVar(value="")
            hint(s, var=self.v_voice_note, style="Warn.TLabel", wrap=560).grid(row=r + 2, column=1, columnspan=2,
                                                                              sticky="w")
            var.trace_add("write", lambda *a: self._update_voice_note())
            return r + 3
        if st.key == "cod3_text_language":   # [f2-menus] 2026-11-11 a text language the game's folder has no file for
            self.v_text_note = tk.StringVar(value="")
            hint(s, var=self.v_text_note, style="Warn.TLabel", wrap=560).grid(row=r + 2, column=1, columnspan=2,
                                                                             sticky="w")
            var.trace_add("write", lambda *a: self._update_text_note())
            return r + 3
        return r + 2

    @staticmethod
    def _gs_same(a, b) -> bool:
        # Strict: False isn't 0, True isn't 1 (a setting's choices can be either).
        return type(a) is type(b) and a == b

    def _gs_label(self, st, value) -> str:
        """What the form shows for a value: its choice's label, or the value
        itself (a size the game takes besides the named ones, a number)."""
        label = next((lb for v, lb in st.choices if self._gs_same(v, value)), None)
        if label is not None:
            return label
        if value is None:
            return st.choices[0][1] if st.choices else ""
        return str(value)

    def _gs_value(self, st, var):
        """The form's value for a setting in the setting's own type, or None
        when the box holds something the game doesn't take."""
        if st.kind == "bool":
            return bool(var.get())
        text = str(var.get()).strip()
        match = next((v for v, lb in st.choices if lb == text), None)
        if match is not None or any(lb == text for _, lb in st.choices):
            return match
        if st.kind == "choice":
            return text if st.also and st.also(text) else None
        try:
            return int(text)
        except ValueError:
            return None

    def _gs_values(self) -> dict:
        """The form's values that the game takes (others are left out)."""
        values = {}
        for key, (st, var, _) in self._gs_vars.items():
            value = self._gs_value(st, var)
            valid = getattr(st, "valid", None)
            if value is not None and (valid is None or valid(value)):
                values[key] = value
        return values

    def _gs_problems(self) -> List[str]:
        bad = []
        for st, var, _ in self._gs_vars.values():
            value = self._gs_value(st, var)
            valid = getattr(st, "valid", None)
            if value is None or (valid is not None and not valid(value)):
                if st.kind == "int":
                    bad.append(f"{st.label}: pick a whole number from {st.min} to {st.max}.")
                elif st.also:  # [claude-9b] typed values of the player's own
                    own = {"cod3_aspect_ratio": "W:H from 4:3 to 48:9, e.g. 24:10",
                           "user_name": "up to 15 characters, ^0-^9 and ^: colors; no quotes, \\, % or ;",
                           "user_name_2": "up to 14 characters, ^0-^9 and ^: colors; no quotes, \\, % or ;",  # [claude-9b] 0074
                           "user_name_3": "up to 14 characters, ^0-^9 and ^: colors; no quotes, \\, % or ;",
                           "user_name_4": "up to 14 characters, ^0-^9 and ^: colors; no quotes, \\, % or ;"}.get(st.key, "WxH, e.g. 2560x1080")
                    bad.append(f"{st.label}: pick one from the list or type your own ({own}).")
                else:
                    bad.append(f"{st.label}: pick one from the list.")
        return bad

    def _gs_edits(self) -> dict:
        """The settings changed in the form since the file was read."""
        if self._gs_loaded_for is None:
            return {}
        return {k: v for k, v in self._gs_values().items() if not self._gs_same(self._gs_loaded.get(k), v)}

    @staticmethod
    def _gs_file_mtime(out: Optional[Path]) -> Optional[int]:
        try:
            return game_settings.settings_file(out).stat().st_mtime_ns if out is not None else None
        except OSError:
            return None

    def _gs_watch(self) -> None:
        """Every 2 s: when the game's settings file changed (the game saves it
        when a setting changes in F2 or its menus) or the game started or
        ended, the Play page reads it again, keeping only what was changed
        here and not saved. Off the UI thread: the running check asks the
        system for its processes."""
        self.root.after(2000, self._gs_watch)
        if getattr(self, "page", "") != "play" or getattr(self, "_gs_loading", False) or \
                getattr(self, "_gs_watching", False) or not getattr(self, "_gs_vars", None):
            return
        out = self._out_path()
        if out is None:
            return
        launched = getattr(self, "_gs_launched", None)
        follow = self._gs_running or (launched is not None and time.monotonic() - launched < 60)
        self._gs_watching = True

        def work():
            mtime = self._gs_file_mtime(out)
            running = self._gs_running
            if follow:
                try:
                    running = bool(game_settings.game_running(out))
                except Exception:  # noqa: BLE001 - best effort
                    pass

            def done():
                self._gs_watching = False
                if not running and launched is not None and time.monotonic() - launched >= 60:
                    self._gs_launched = None   # started and gone (or never came up)
                if mtime != getattr(self, "_gs_mtime", None) or running != self._gs_running:
                    self._load_game_settings(keep_edits=True)
            self.q.put(("call", done))
        threading.Thread(target=work, daemon=True).start()

    def _gs_dirty(self) -> bool:
        if not getattr(self, "_gs_vars", None) or self._gs_loaded_for is None:
            return False
        values = self._gs_values()
        return len(values) != len(self._gs_vars) or \
            any(not self._gs_same(self._gs_loaded.get(k), v) for k, v in values.items())

    def _set_gs_form(self, values: dict) -> None:
        for key, (st, var, w) in self._gs_vars.items():
            v = values.get(key, st.default)
            if st.kind == "bool":
                var.set(bool(v))
                continue
            label = self._gs_label(st, v)
            if st.kind == "choice":
                # A value the list doesn't name (a size typed in F2) is shown as itself.
                labels = [lb for _, lb in st.choices]
                w.configure(values=labels + ([label] if label not in labels else []))
            var.set(label)

    def _load_game_settings(self, keep_edits: bool = False) -> None:
        """Reads the game's settings file into the form (off the UI thread),
        and whether the game is running. keep_edits: changes not saved yet
        stay in the form (only the running check is new)."""
        if not getattr(self, "_gs_vars", None):
            return
        out = self._out_path()
        folder = self.v_out.get().strip()

        self._gs_loading = True

        def work():
            values, err = {}, ""
            mtime = self._gs_file_mtime(out)
            if out is not None:
                try:
                    values = game_settings.load(out)
                except (SetupError, OSError, ValueError) as e:
                    err = str(e)
            try:
                running = bool(game_settings.game_running(out))
            except Exception:  # noqa: BLE001 - best effort
                running = False

            def done():
                self._gs_loading = False
                if self.v_out.get().strip() != folder:
                    return   # the output folder changed meanwhile: its own load follows
                self._gs_running = running
                self._gs_mtime = mtime
                # The file's values everywhere, except the settings changed here
                # and not saved yet (keep_edits): so a change made in the game
                # shows here, and saving here never puts an old value back.
                edits = self._gs_edits() if keep_edits and self._gs_loaded_for == folder else {}
                if not err:
                    self._gs_loaded = dict(values)
                    self._gs_loaded_for = folder
                    self._set_gs_form({**values, **edits})
                self._update_game_settings_state(err)
            self.q.put(("call", done))
        threading.Thread(target=work, daemon=True).start()

    def _game_settings_defaults(self) -> None:
        self._set_gs_form({key: st.default for key, (st, _, _) in self._gs_vars.items()})

    def _update_game_settings_state(self, error: str = "") -> None:
        if not hasattr(self, "btn_gs_save") or not self._gs_vars:
            return
        reason, style = "", "Hint.TLabel"
        if self._out_path() is None:
            reason, style = "Choose an output folder on Set up first.", "Warn.TLabel"
        elif self._gs_running:
            reason, style = ("Call of Duty 3 is running: close it first. This page reads its settings again "
                             "when it quits."), "Warn.TLabel"   # [acct2-decomp] _gs_watch does
        elif error:
            reason, style = f"Couldn't read the settings file: {error}", "Warn.TLabel"
        elif self._gs_problems():
            reason, style = self._gs_problems()[0], "Warn.TLabel"
        self.btn_gs_save.configure(state="disabled" if reason else "normal")
        self.btn_gs_undo.configure(state="normal" if self._gs_dirty() else "disabled")
        if not reason:
            reason = "Changed: press Save to keep it." if self._gs_dirty() else ""
        self.v_gs_reason.set(reason)
        self.lbl_gs_reason.configure(style=style)

    def _save_game_settings(self, quiet: bool = False) -> bool:
        """Writes the settings changed here, and only those: a setting changed
        in the game since this page read the file keeps the game's value.
        False when nothing could be saved (the reason is shown)."""
        out = self._out_path()
        if out is None:
            return False
        if self._gs_problems():
            self._info("Not saved", "The game settings weren't saved.\n" + self._gs_problems()[0], kind="warning")
            return False
        changed = self._gs_edits()
        try:
            if game_settings.game_running(out):
                self._gs_running = True
                self._update_game_settings_state()
                self._info("Close the game first", "Call of Duty 3 is running.\nChange settings in the game (F2) "
                                                   "while it runs, or close it and save them here.",
                           kind="warning")
                return False
            game_settings.save(out, changed)
        except (SetupError, OSError, ValueError, NotImplementedError) as e:
            self._info("Not saved", f"The game settings weren't saved.\n{e}", kind="error")
            return False
        self._gs_loaded = {**self._gs_loaded, **changed}
        self._gs_mtime = self._gs_file_mtime(out)
        self._update_game_settings_state()
        if not quiet:
            self.v_status.set("Game settings saved." if changed else "Nothing to save.")
        return True

    # ------------------------------------------------------------------ Textures page
    def _page_textures(self, t) -> None:
        t.columnconfigure(0, weight=1)
        # [rexglue-native-settings] 2026-11-11 (owner 08:28: "a texture and censoring tab"): the page's name, and the
        # censored campaign's box from the Play page below the insignia section.
        widgets.page_header(t, "Textures & censoring", "Sharper copies of the game's textures, your own pictures in "
                                                       "place of any texture, and the insignia covered.", columns=1)
        self.pages["textures"]["on_show"] = self._update_texture_state

        u = section(t, "HD textures (waifu2x)", 1, columns=1)
        self._grid_cols(u)

        def either(words: List[str]) -> str:
            return ", ".join(words[:-1]) + " or " + words[-1]
        sizes = either([f"{upscaled_size(k) / 1024 ** 3:.0f}" for k in upscale.SCALES])
        scales = either([f"{k}x" for k in upscale.SCALES])
        # A short box label (a check box's text can't wrap); the details wrap under it.
        top = ttk.Frame(u)
        top.grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, px(6)))
        ttk.Checkbutton(top, text="Upscale the extracted textures with waifu2x", variable=self.v_upscale,
                        command=self._update_upscale).pack(anchor="w")
        hint(top, f"Uses your graphics card; about {sizes} GB more at {scales}.").pack(anchor="w",
                                                                                       padx=(px(26), 0))
        ttk.Label(u, text="Style").grid(row=1, column=0, sticky="w", pady=px(3))
        self.cb_upscale_model = ttk.Combobox(u, textvariable=self.v_upscale_model, state="readonly", width=36,
                                             values=[label for _, label in upscale.MODELS.values()])
        self.cb_upscale_model.grid(row=1, column=1, sticky="w", pady=px(3))
        ttk.Label(u, text="Noise reduction").grid(row=2, column=0, sticky="w", pady=px(3))
        self.cb_upscale_noise = ttk.Combobox(u, textvariable=self.v_upscale_noise, state="readonly", width=16,
                                             values=list(upscale.NOISE_LEVELS.values()))
        self.cb_upscale_noise.grid(row=2, column=1, sticky="w", pady=px(3))
        ttk.Label(u, text="Scale").grid(row=3, column=0, sticky="w", pady=px(3))
        self.cb_upscale_scale = ttk.Combobox(u, textvariable=self.v_upscale_scale, state="readonly", width=36,
                                             values=list(upscale.SCALES.values()))
        self.cb_upscale_scale.grid(row=3, column=1, sticky="w", pady=px(3))
        hint(u, "The copies go to data/cod3/textures_upscaled; the originals stay as they are. They are then "
                "packed for the playable game into data/cod3/hd_textures: at the copies' size, in the same "
                "compressed formats as the game's own, so at 2x they take about 4 times the texture memory (16 "
                "times at 4x). 1x only reduces noise (with the best-quality style). Turn them off in the game with "
                "F2 > Video > HD textures.").grid(row=4, column=0, columnspan=3, sticky="w", pady=(px(6), 0))
        self.v_hd_state = tk.StringVar()
        self.lbl_hd_state = hint(u, var=self.v_hd_state, style="Accent.TLabel")
        self.lbl_hd_state.grid(row=5, column=0, columnspan=3, sticky="w", pady=(px(6), 0))
        for cb, text in ((self.cb_upscale_model, "Photo suits Call of Duty 3's textures; Best quality is several "
                                                  "times slower."),
                         (self.cb_upscale_noise, "How much compression noise to remove."),
                         (self.cb_upscale_scale, "How much bigger the copies are.")):
            tip(cb, text, reason=lambda: "" if self.v_upscale.get() else
                "Tick \"Upscale the extracted textures\" first.")
        self._update_upscale()

        # The user texture pack (c3w/user_textures.py): local work only.
        # [acct2-decomp] Joined with the built-in censor (owner 2026-11-05 07:24); short texts (07:25).
        m = section(t, "Insignia and your own textures", 6, columns=1)
        self._censor_builtin_box(m)   # [acct2-decomp]
        hint(m, "Your own pictures: PNGs named like the ripped textures, in data/cod3/user_textures_png. "
                "They win over the designs.").pack(anchor="w", pady=(px(8), 0))
        bf = ttk.Frame(m)
        bf.pack(anchor="w", pady=(px(8), 0))
        b = ttk.Button(bf, text="Open picture folder", command=self._open_user_textures)
        b.pack(side="left")
        tip(b, "insignia_textures.txt there lists the insignia and their sizes.")
        self.btn_user_pack = ttk.Button(bf, text="Build texture pack", command=self._build_user_textures)
        self.btn_user_pack.pack(side="left", padx=(px(6), 0))
        tip(self.btn_user_pack, lambda: self.v_user_pack_reason.get() or
            "The designs and your pictures, for the game's next start.")
        doc = self._doc_path("docs/TEXTURE_REPLACEMENT.md")
        if doc is not None:
            widgets.link(bf, "How it works", lambda: self._open_file(doc)).pack(side="left", padx=(px(12), 0))
        self.v_user_pack_reason = tk.StringVar()
        hint(m, var=self.v_user_pack_reason, style="Warn.TLabel").pack(anchor="w", pady=(px(4), 0))
        # [rexglue-native-settings] 2026-11-11 where the pack stands (filled by _update_texture_state)
        self.v_user_pack_state = tk.StringVar()
        hint(m, var=self.v_user_pack_state, style="Accent.TLabel").pack(anchor="w", pady=(px(2), px(18)))

        # [acct2-decomp] The censored campaign (c3w/censored.py): the German release's textures, on or off.
        # [rexglue-native-settings] 2026-11-11 moved here from the Play page as it was (owner 08:28).
        cs = section(t, "Censored campaign (German disc)", 7, columns=1)
        cf = ttk.Frame(cs)
        cf.pack(anchor="w", pady=(0, px(18)))
        self.v_censored = tk.BooleanVar(value=False)
        cz = ttk.Checkbutton(cf, text="Censored campaign (the German release's textures; needs the German disc once)",
                             variable=self.v_censored, command=self._toggle_censored)
        cz.grid(row=0, column=0, columnspan=6, sticky="w")
        self.cb_censored = cz
        tip(cz, "The campaign with the German release's textures (insignia taken off). Needs the German disc once: "
                "the first time it asks for its .iso and copies about 1.2 GB from it; after that it switches at "
                "once. A game built from the German disc is censored already: switching it off needs the "
                "USA/Europe or Europe (Fr, Es, It) disc once. Text, voices and multiplayer don't change. Close the "
                "game first.")
        self._refresh_censored()   # [rexglue-native-settings] the box as the game's files are, now that it exists

    def _update_upscale(self) -> None:
        state = "readonly" if self.v_upscale.get() else "disabled"
        for cb in (self.cb_upscale_model, self.cb_upscale_noise, self.cb_upscale_scale):
            cb.configure(state=state)
        self._update_texture_state()

    def _update_texture_state(self) -> None:
        if not hasattr(self, "v_hd_state"):
            return
        self._censor_builtin_load()   # [acct2-decomp]
        out = self._out_path()
        packed = out is not None and (out / "data" / "cod3" / "hd_textures").is_dir()
        if self.v_upscale.get():
            text = "Made or updated at the next Build." + (" HD textures are already packed in the output "
                                                            "folder." if packed else "")
        else:
            text = ("HD textures from an earlier build are in the output folder; the game uses them unless "
                    "turned off in F2." if packed else "")
        self.v_hd_state.set(text)
        if text:
            self.lbl_hd_state.grid()
        else:
            self.lbl_hd_state.grid_remove()
        if hasattr(self, "v_user_pack_state"):   # [rexglue-native-settings] 2026-11-11 the user pack's state
            self.v_user_pack_state.set(self._user_pack_status(out))
        self._update_page_states()

    def _user_pack_status(self, out: Optional[Path]) -> str:
        """[rexglue-native-settings] 2026-11-11 One line on the user texture pack (the insignia censor's pictures and
        your own): how many textures it has, and whether Play will build it first."""
        if out is None:
            return ""
        data = out / "data" / "cod3"
        try:
            keys = user_textures.pack_keys(data)
            why = user_textures.pack_out_of_date(data)
            censor = censor_builtin.status(data)
        except Exception as e:  # noqa: BLE001 - a status line only
            return f"Texture pack: can't be read ({e})."
        text = (f"Texture pack: {keys} texture(s), used from the game's next start." if keys else
                "Texture pack: none built.")
        if why:
            text += f" {why[0].upper()}{why[1:]}: Play builds it first."
        return f"{text} {censor}"

    def _user_data(self) -> Optional[Path]:
        """<output>/data/cod3, or None (with a message) without an output folder."""
        out = self._out_path()
        if out is None:
            self._info("No output folder", "Choose the output folder on Set up first.", kind="warning")
            return None
        return out / "data" / "cod3"

    def _installed_rip(self) -> Optional[Path]:
        out = self._out_path()
        if out is None:
            return None
        try:
            return user_textures.installed_rip(out, common.exe_name("rip"))
        except OSError:
            return None

    def _open_user_textures(self) -> None:
        data = self._user_data()
        if data is None:
            return
        try:
            folder = user_textures.prepare_folder(data, Path(__file__).resolve().parent.parent)
        except OSError as e:
            self._info("Could not make the folder", str(e), kind="error")
            return
        self._open(str(folder))

    def _play_builds_pack_first(self, out: Path) -> bool:
        """[rexglue-native-settings] 2026-11-11 When the user texture pack is older than the insignia censor's choice
        or the player's own pictures (user_textures.pack_out_of_date), builds it the way "Build texture pack" does
        (same progress and log lines, in the background) and starts the game when that is done: True. False when
        there is nothing to build or no ripper to build it with (said in the log): the game starts as it is."""
        data = out / "data" / "cod3"
        try:
            why = user_textures.pack_out_of_date(data)
        except Exception as e:  # noqa: BLE001 - a check must never keep the game from starting
            self.log.warn(f"Couldn't check the texture pack: {e}")
            return False
        if not why:
            return False
        if getattr(self, "_pack_for_play", False):   # Play pressed again while it builds
            self.v_status.set("Building the texture pack: the game starts when it is done.")
            return True
        rip = user_textures.installed_rip(out, common.exe_name("rip"))
        if rip is None:
            self.log.warn(f"Texture pack: {why}, but there is no ripper in {out / 'tools'} to build it (build the "
                          "game first). The game starts with the texture pack as it is.")
            return False
        self.log.info(f"Texture pack: {why}. Building it before the game starts...")
        self._pack_for_play = True
        self._cb_dirty = False   # this build applies the saved choice, as _build_user_textures
        tool_root = Path(__file__).resolve().parent.parent

        def work():
            try:
                runner = common.Runner(self.log, threading.Event())
                keys = user_textures.build(rip, data, tool_root, self._jobs(), runner, self.log)
                self.log.info(f"Texture pack built before the game starts: {keys} texture(s). "
                              f"{censor_builtin.status(data)}")
            except Exception as e:  # noqa: BLE001 - the game still starts, with the pack as it was
                self.log.warn(f"The texture pack wasn't built ({e}); the game starts with the pack as it was.")
            finally:
                def start():   # on the UI thread
                    self._pack_for_play = False
                    self._censor_builtin_load()
                    self._play_cod3(pack_ready=True)
                self.q.put(("call", start))
        self._bg(work, "Building the texture pack before the game starts...")
        return True

    def _build_user_textures(self) -> None:
        """`rip cod3-user-pack` with the ripper the build left in tools/."""
        if self._job_running():   # [acct2-decomp] a build writes the same pack (pipeline: user_textures.build)
            self.v_status.set("Busy: wait for the job running now, or cancel it.")
            return
        data = self._user_data()
        if data is None:
            return
        output = data.parent.parent
        rip = user_textures.installed_rip(output, common.exe_name("rip"))
        if rip is None:
            self._info("Not built yet", f"No ripper in {output / 'tools'}.\nBuild first (Set up).", kind="warning")
            return
        try:   # [acct2-decomp] the game reads the pack it would replace
            if game_settings.game_running(output):
                self._info("Close the game first", "Call of Duty 3 is running: close it, then build the texture "
                                                   "pack.", kind="warning")
                return
        except Exception:  # noqa: BLE001 - best effort, as _play_cod3
            pass
        self._cb_dirty = False   # [acct2-decomp] this build applies the saved choice
        self._censor_builtin_load()
        tool_root = Path(__file__).resolve().parent.parent

        def work():
            runner = common.Runner(self.log, threading.Event())
            keys = user_textures.build(rip, data, tool_root, self._jobs(), runner, self.log)
            pack = user_textures.pack_dir(data)
            censor = censor_builtin.status(data)   # [acct2-decomp]
            self.q.put(("info", "Texture pack",
                        f"{keys} texture(s) in the pack, used from the game's next start.\n{censor}\nSkipped "
                        "pictures and why: Log page." if keys else
                        f"No usable pictures, so there is no texture pack.\n{censor}\nWhy: Log page."))
        self._bg(work, "Building the texture pack...")

    # ------------------------------------------------------------------ Mods & maps page
    def _page_maps(self, t) -> None:
        t.columnconfigure(0, weight=1)
        t.rowconfigure(2, weight=1)
        head = widgets.page_header(t, "Mods & maps", "Add World at War mods and custom maps (UGX and others): an "
                                   "installer, an archive or a folder. Installers are unpacked, never run; everything "
                                   "goes to data/waw/custom_maps in the output folder.", columns=1)
        widgets.link(head, "ugx-mods.com", lambda: webbrowser.open("https://www.ugx-mods.com/")).pack(
            side="right", anchor="n")
        lf = ttk.Frame(t)
        lf.grid(row=2, column=0, sticky="nsew", padx=px(24), pady=(px(12), px(6)))
        lf.columnconfigure(0, weight=1)
        lf.rowconfigure(0, weight=1)
        self.lb_maps = tk.Listbox(lf, height=10, borderwidth=0, highlightthickness=1, activestyle="none")
        self.lb_maps.grid(row=0, column=0, sticky="nsew")
        theme.recolor(self.lb_maps)
        bf = ttk.Frame(t)
        bf.grid(row=3, column=0, sticky="ew", padx=px(24), pady=(px(6), px(18)))
        widgets.button_row(bf, [("Add from installer / archive...", self._install_file, ""),
                                ("Add from folder...", self._install_folder, ""),
                                ("Remove selected", self._remove_map, "Delete the selected map from the output "
                                                                      "folder."),
                                ("Open folder", self._open_map_folder, "")])
        ttk.Button(bf, text="Refresh", command=self._refresh_maps).pack(side="right")
        self._refresh_maps()

    def _refresh_maps(self) -> None:
        if not hasattr(self, "lb_maps"):
            return
        self.lb_maps.delete(0, "end")
        out = self.v_out.get().strip()
        self._map_list = waw.list_custom_maps(Path(out)) if out else []
        for m in self._map_list:
            self.lb_maps.insert("end", f"{m.name}    ({len(m.fastfiles)} fastfiles)")

    def _install(self, src: str) -> None:
        out = self.v_out.get().strip()
        if not out:
            self._info("No output folder", "Choose the output folder on Set up first.", kind="warning")
            return
        output = Path(out)

        def work():
            runner = common.Runner(self.log, self.cancel)
            tc = deps.detect(self._tools_dir(), runner)
            maps = waw.install_custom_map(Path(src), output, Path(self.v_work.get() or common.default_work_dir()) /
                                          "scratch", waw.Unpackers(tc.sevenzip, tc.innoextract, tc.unar),
                                          runner, self.log, threading.Event())
            rip = output / "tools" / common.exe_name("rip")
            ripped = ""
            if rip.is_file():
                for m in maps:
                    ok, errors = rippers.rip_fastfiles(rip, m.fastfiles, m.folder / "ripped", runner, self.log)
                    for e in errors:
                        self.log.warn(e)
                ripped = "\nIts fastfiles were ripped too."
            self.q.put(("info", "Added", "Added: " + ", ".join(m.name for m in maps) + ripped))
            self.q.put(("refresh",))
        self._bg(work, f"Adding {Path(src).name}...")

    def _install_file(self) -> None:
        p = filedialog.askopenfilename(title="Mod or map installer, or archive", parent=self.root, filetypes=[
            ("Installers and archives", "*.exe *.zip *.7z *.rar *.EXE *.ZIP *.7Z *.RAR"), ("All files", "*")])
        if p:
            self._install(p)

    def _install_folder(self) -> None:
        p = filedialog.askdirectory(title="Unpacked mod or map folder", parent=self.root)
        if p:
            self._install(p)

    def _selected_map(self) -> Optional[waw.CustomMap]:
        sel = self.lb_maps.curselection()
        return self._map_list[sel[0]] if sel else None

    def _remove_map(self) -> None:
        m = self._selected_map()
        if not m:
            return
        if not widgets.ask_yes_no(self.root, "Remove map", f"Delete {m.name}?\n{m.folder}", yes="Delete",
                                  no="Keep", danger=True):
            return
        try:
            waw.remove_custom_map(m, Path(self.v_out.get()), self.log)
        except SetupError as e:
            self._info("Not removed", str(e), kind="error")
        self._refresh_maps()

    def _open_map_folder(self) -> None:
        m = self._selected_map()
        out = self.v_out.get().strip()
        self._open(str(m.folder) if m else (str(Path(out) / waw.CUSTOM_MAPS) if out else ""))

    # ------------------------------------------------------------------ Tools page
    def _page_tools(self, t) -> None:
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Tools", "Check this PC, find your files, repair a build, keep your settings as "
                                        "profiles, and change the look.", columns=1)
        self.pages["tools"]["on_show"] = self._refresh_profiles

        pc = section(t, "This PC", 1, columns=1)
        hint(pc, "What the build needs (compilers, tools, libraries) and whether this PC has it. Build installs "
                 "what's missing by itself, after you approve the list.").pack(anchor="w")
        bf = ttk.Frame(pc)
        bf.pack(anchor="w", pady=(px(8), 0))
        widgets.button_row(bf, [("Check this PC", self._check_tools, "List what's installed and what's missing.")])

        f = section(t, "Folders", 3, columns=1)
        hint(f, "Everything stays in the project folder unless you chose other places.").pack(anchor="w")
        bf = ttk.Frame(f)
        bf.pack(anchor="w", pady=(px(8), 0))
        widgets.button_row(bf, [
            ("Where things go", self._show_locations, "Every folder the tool uses, and what is in it."),
            ("Output folder", lambda: self._open(self.v_out.get()), "The game, the ripped files, saves and logs."),
            ("Input folder", self._open_input, "Your discs and downloads (only ever read)."),
            ("Log folder", self._open_logs, "The build logs.")])

        r = section(t, "Repair", 5, columns=1)
        hint(r, "Looks over the build in the output folder for missing or broken files, and fixes what it can by "
                "redoing only that part of the build (with the same approval before any download).").pack(anchor="w")
        bf = ttk.Frame(r)
        bf.pack(anchor="w", pady=(px(8), 0))
        b = ttk.Button(bf, text="Check the build", command=self._run_repair_check)
        b.pack(side="left")
        tip(b, "Look for problems in the output folder (F5 checks again).")
        self.btn_repair = ttk.Button(bf, text="Repair", style="Accent.TButton", command=self._start_repair)
        self.btn_repair.pack(side="left", padx=(px(6), 0))
        tip(self.btn_repair, lambda: self.v_repair_reason.get() or "Redo the parts of the build that fix these.")
        # [acct2-decomp] the disc's MD5s, file by file (c3w/disc_files.py)
        b = ttk.Button(bf, text="Check game files (MD5)", command=self._check_disc_md5)
        b.pack(side="left", padx=(px(6), 0))
        tip(b, "Read every file taken off the disc and compare it with the disc's own MD5 (a minute or two; "
               "nothing is changed).")
        self.v_repair_reason = tk.StringVar()
        self.lbl_repair_reason = hint(r, var=self.v_repair_reason)
        self.lbl_repair_reason.pack(anchor="w", pady=(px(4), 0))
        self.repair_list = ttk.Frame(r)
        self.repair_list.pack(fill="x", anchor="w")
        self._repair_report = None

        p = section(t, "Profiles", 7, columns=1)
        hint(p, "Keep your settings (discs, folders, options) under a name and switch between them. Exported "
                "profiles leave out paths outside this project folder, so they can be shared.").pack(anchor="w")
        pf = ttk.Frame(p)
        pf.pack(anchor="w", pady=(px(8), 0))
        self.lb_profiles = tk.Listbox(pf, height=6, borderwidth=0, highlightthickness=1, activestyle="none",
                                      exportselection=False, width=40)
        self.lb_profiles.grid(row=0, column=0, sticky="ns")
        theme.recolor(self.lb_profiles)
        self.lb_profiles.bind("<<ListboxSelect>>", lambda e: self._update_profile_buttons())
        self.lb_profiles.bind("<Double-Button-1>", lambda e: self._profile_load())
        self.v_profiles_empty = tk.StringVar()
        pb = ttk.Frame(pf)
        pb.grid(row=0, column=1, sticky="n", padx=(px(10), 0))
        self.profile_buttons = {}
        for key, text, cmd, tooltip in (
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
            self.profile_buttons[key] = b
        hint(p, var=self.v_profiles_empty).pack(anchor="w", pady=(px(4), 0))

        a = section(t, "Appearance", 9, columns=1)
        hint(a, "The game's F2 themes, and a highlight color of your own. The other tabs pick theirs in the "
                "sidebar (widen the window if it's hidden).").grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, px(6)))
        for i, tid in enumerate(theme.THEME_ORDER):
            th = theme.THEMES[tid]
            ttk.Radiobutton(a, text=th.label, value=th.label, variable=self.v_theme,
                            command=lambda x=tid: self._set_theme(x)).grid(row=1 + i, column=0, sticky="w", pady=px(2))
            hint(a, th.description).grid(row=1 + i, column=1, sticky="w", padx=(px(12), 0))
        af = ttk.Frame(a)
        af.grid(row=6, column=0, columnspan=3, sticky="w", pady=(px(10), px(18)))
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
        self.btn_reset_accent = ttk.Button(af, text="Reset accent", command=self._reset_accent)
        self.btn_reset_accent.pack(side="left", padx=(px(6), 0))
        tip(self.btn_reset_accent, lambda: "Back to the theme's own accent." if self.accent else
            "The theme's own accent is in use.")
        self._refresh_profiles()

    def _update_accent_reset(self) -> None:
        if hasattr(self, "btn_reset_accent"):
            self.btn_reset_accent.configure(state="normal" if self.accent else "disabled")

    # This PC
    def _check_tools(self) -> None:
        # What the build would compile here: this system's own version, and on
        # Linux also the Windows one (cross-built). A Linux version made in WSL
        # needs nothing compiled on Windows. [acct2-decomp] 2026-11-05 (owner: "refresh the dependency
        # checks and options offered"): worked out by build_needs.py, the build's own rules: no Rust when
        # the build needs none (a release download's prebuilt ripper), and rows for the tools only some
        # options add (llvm-mingw, appimagetool, waifu2x).
        opts = self._options()

        def work():
            runner = common.Runner(self.log, threading.Event())
            tc = deps.detect(self._tools_dir(), runner)
            needs = build_needs.check(opts, tc=tc, runner=runner)
            recomp, problems = needs.recomp, needs.problems
            rows = [(n, True, (d + "; " if d else "") + "not needed for this build")
                    if n == "Rust" and not found and not needs.rust else (n, found, d)
                    for n, found, d in tc.rows()] + needs.option_rows
            # For each problem why it matters and the fix, and what about
            # this PC can get in the way (pc_check); the log gets them too.
            work_dir = Path(self.v_work.get() or common.default_work_dir())
            out = Path(self.v_out.get()) if self.v_out.get().strip() else None
            notes = pc_check.tool_notes(tc, recomp=recomp, rust=needs.rust) + pc_check.system_notes(work_dir, out)
            self.log.info("This PC:\n" + "\n".join(pc_check.system_lines()) + "\n" + tc.summary() +
                          "".join(f"\n{n:<20}  {d}" for n, _f, d in needs.option_rows) +
                          "".join("\n" + n.text() for n in notes))
            self.q.put(("checked", rows, problems, recomp, notes))
        self._bg(work, "Checking this PC...")

    def _show_check(self, rows: List[tuple], problems: List[str], recomp: bool,
                    notes: Optional[list] = None) -> None:
        """The result of "Check this PC" in its own window: a table that
        follows the font size and the window (long paths wrap, the list
        scrolls on a small screen) with the buttons always in view. Below it,
        `notes` (pc_check.Note): what each problem is for and how to fix it."""
        notes = notes or []
        self.v_status.set("Ready.")
        d = widgets.dialog(self.root, "This PC")
        heading = ttk.Label(d, text="Everything needed is installed." if not problems else
                            "Some things the build needs are missing.",
                            style="GoodBold.TLabel" if not problems else "WarnBold.TLabel")
        heading.pack(side="top", anchor="w", padx=px(16), pady=(px(14), px(8)))
        buttons = ttk.Frame(d)
        buttons.pack(side="bottom", fill="x", padx=px(16), pady=px(14))
        body = ScrollFrame(d)
        body.pack(side="top", fill="both", expand=True, padx=px(16))
        t = body.inner
        t.columnconfigure(2, weight=1)
        details = []
        for r, (name, found, detail) in enumerate(rows):
            ttk.Label(t, text=name, style="Bold.TLabel").grid(row=r, column=0, sticky="nw", padx=(0, px(12)),
                                                             pady=px(2))
            optional = "optional" in name or detail.startswith(build_needs.AT_BUILD)   # [acct2-decomp]
            ttk.Label(t, text="Found" if found else ("Not installed" if optional else "Missing"),
                      style="Good.TLabel" if found else "Hint.TLabel" if optional else "Bad.TLabel").grid(
                row=r, column=1, sticky="nw", padx=(0, px(12)), pady=px(2))
            lbl = ttk.Label(t, text=detail, style="Hint.TLabel", justify="left")
            lbl.grid(row=r, column=2, sticky="nw", pady=px(2))
            details.append(lbl)
        wide = []   # labels across the whole table (the notes)
        r = len(rows)
        if notes:
            ttk.Label(t, text="What to do" if problems else "Worth knowing", style="Bold.TLabel").grid(
                row=r, column=0, columnspan=3, sticky="w", pady=(px(14), 0))
            r += 1
            for n in notes:
                style = {"error": "Bad.TLabel", "warn": "Warn.TLabel"}.get(n.level, "Hint.TLabel")
                title = ttk.Label(t, text=n.title, style=style, justify="left")
                title.grid(row=r, column=0, columnspan=3, sticky="w", pady=(px(8), 0))
                text = "\n".join(x for x in (f"Why: {n.detail}" if n.detail else "",
                                             f"Fix: {n.fix}" if n.fix else "") if x)
                body_lbl = ttk.Label(t, text=text, style="Hint.TLabel", justify="left")
                body_lbl.grid(row=r + 1, column=0, columnspan=3, sticky="w", padx=(px(12), 0))
                wide += [title, body_lbl]
                r += 2
        if problems:
            ttk.Label(t, text="Install them now? You may be asked for your password. Build also does this by "
                      "itself, after you approve the list." if notes else
                      "Missing:\n" + "\n".join("  - " + p for p in problems) +
                      "\n\nInstall them now? You may be asked for your password. Build also does this by itself.",
                      justify="left").grid(row=r, column=0, columnspan=3, sticky="w", pady=(px(12), 0))

        def rewrap(_=None) -> None:
            # Paths wrap at the width the third column has in view, so the
            # table never needs sideways scrolling; the notes at the whole width.
            bar = body.vbar.winfo_width() if body.vbar.winfo_ismapped() else 0
            width = max(120, body.winfo_width() - bar - t.grid_bbox(0, 0, 1, 0)[2] - 8)
            full = max(160, body.winfo_width() - bar - px(20))
            changed = False
            for labels, w in ((details, width), (wide, full)):
                if any(int(lbl.cget("wraplength") or 0) != w for lbl in labels):
                    for lbl in labels:
                        lbl.configure(wraplength=w)
                    changed = True
            if changed:
                body.relayout()
        for lbl in details + wide:   # until the window has a size: most of the screen
            lbl.configure(wraplength=int(d.winfo_screenwidth() * 0.5))
        body.bind("<Configure>", rewrap, add="+")

        def close(install: bool = False) -> None:
            try:
                d.grab_release()
            except tk.TclError:
                pass
            d.destroy()
            if install:
                self._install_missing(recomp)
        if problems:
            ttk.Button(buttons, text="Install missing", style="Accent.TButton",
                       command=lambda: close(True)).pack(side="right")
        ttk.Button(buttons, text="Close", style="TButton" if problems else "Accent.TButton",
                   command=close).pack(side="right", padx=(0, px(6)) if problems else 0)
        ttk.Button(buttons, text="Copy", command=lambda: self._copy_text(
            "\n".join(f"{n}: {'found' if f else 'missing'} {x}".rstrip() for n, f, x in rows) +
            "".join("\n\n" + n.text() for n in notes))).pack(side="left")
        d.protocol("WM_DELETE_WINDOW", close)
        d.bind("<Escape>", lambda e: close())
        # Open at the content's size, at most 80% of the screen, centered on
        # the main window; it can be resized either way. The height is
        # measured once the paths are wrapped at the width it opens with.
        d.update_idletasks()
        sw, sh = d.winfo_screenwidth(), d.winfo_screenheight()
        w = min(max(t.winfo_reqwidth() + 60, heading.winfo_reqwidth() + 40, 420), int(sw * 0.8))
        first_col = t.grid_bbox(0, 0, 1, 0)[2]
        for lbl in details:
            lbl.configure(wraplength=max(120, w - 60 - first_col))
        for lbl in wide:
            lbl.configure(wraplength=max(160, w - 60))
        d.update_idletasks()
        h = min(t.winfo_reqheight() + heading.winfo_reqheight() + buttons.winfo_reqheight() + 60, int(sh * 0.8))
        x = self.root.winfo_rootx() + max(0, (self.root.winfo_width() - w) // 2)
        y = self.root.winfo_rooty() + max(0, (self.root.winfo_height() - h) // 3)
        d.geometry(f"{w}x{h}+{min(x, sw - w)}+{min(y, sh - h)}")
        d.minsize(min(w, 360), min(h, 200))
        d.deiconify()
        try:
            d.grab_set()
        except tk.TclError:
            pass
        d.focus_set()

    def _install_missing(self, recomp: bool) -> None:
        if self._job_running():   # [acct2-decomp] a build's own installs and approvals run then
            self.v_status.set("Busy: wait for the job running now, or cancel it.")
            return
        self.show_page("log")
        common.APPROVAL.reset()
        common.APPROVAL.ask = self._ask_approval

        opts = self._options()   # [acct2-decomp]

        def work():
            runner = common.Runner(self.log, self.cancel)
            rust = build_needs.check(opts, runner=runner).rust   # [acct2-decomp] no Rust when none is needed
            tc = deps.ensure(self._tools_dir(), runner, self.log, self.cancel, True, recomp=recomp,
                             windows_exe=IS_LINUX and self.v_windows.get(), rust=rust)
            left = tc.problems(recomp=recomp)
            if not rust:
                left = [x for x in left if x not in tc.rust_problems()]
            fixes = "".join("\n\n" + n.text() for n in pc_check.tool_notes(tc, recomp=recomp, rust=rust))
            self.q.put(("call", lambda: self._needs_soon(0)))   # [acct2-decomp] the Set up line, now
            self.q.put(("info", "This PC", "All set." if not left else
                        "Still missing:\n- " + "\n- ".join(left) + fixes + "\n\nSee the Log page for details."))
        self._bg(work, "Installing missing tools...")

    # Where things go
    def _locations(self) -> List[tuple]:
        """Every place the tool reads or writes, as (what, folder, note)."""
        out = Path(self.v_out.get() or common.default_output_dir())
        work = Path(self.v_work.get() or common.default_work_dir())
        tools = work / "tools"
        rows = [("Your discs and downloads", str(common.default_input_dir()), "Only read; never changed or deleted."),
                ("The game, the rips, saves and logs", str(out),
                 "The game, data/ (ripped files), userdata/ (saves, game settings, the game's log), logs/ "
                 "(build logs), tools/rip."),
                ("Temporary build files", str(work),
                 "The SDK source and build and the compiled game; removed after a build unless you keep them."),
                ("Downloaded tools", str(tools),
                 "CMake, Ninja, 7-Zip, LLVM, llvm-mingw, appimagetool, waifu2x as needed; kept for the next "
                 "build. Delete the folder to free the space; they are downloaded again (with your approval)."),
                ("This tool's settings and profiles", str(self.config_dir), ""),
                # [acct2-bro]
                ("Big Red One", str(out / "bro"), "The disc's files (game/), the port (bin/), saves/, logs/ and "
                 "play.sh."),
                ("Big Red One's build", str(work / "bro"), "The recompiled C and the build; removed after a build "
                 "unless you keep them. xboxrecomp and its Python stay in the downloaded tools.")]
        local_rust = deps.rust_dir(tools)
        if local_rust.exists() or not deps.find_cargo():
            rows.append(("Rust", str(local_rust), "Installed here by the tool when the PC has no Rust (cargo/, "
                                                  "rustup/); nothing goes into your home folder."))
        else:
            rows.append(("Rust (already on this PC)", str(deps.cargo_bin_dir().parent),
                         "Yours, installed before; used as it is."))
        if IS_LINUX:
            rows.append(("Build packages (compilers, libraries)", "/usr (the system)",
                         "Installed by your package manager, which can only put them there; listed for approval "
                         "first, then it asks for your password."))
            if " " in str(work):
                rows.append(("Build link", str(common.cache_dir() / "work-link"),
                             "Only a link to the work folder (its path has spaces, which some build tools can't "
                             "take); the files stay in the work folder."))
        if IS_WINDOWS:
            rows.append(("Python (from setup.bat)", str(common.project_dir() / "work" / "tools" / "python"),
                         "Where setup.bat installs Python when it isn't on the PC."))
            rows.append(("Visual Studio Build Tools and Windows SDK",
                         r"C:\Program Files (x86)\Microsoft Visual Studio and Windows Kits",
                         "Installed by Microsoft's installer, which decides the place; an existing Visual Studio "
                         "is used as it is."))
            rows.append(("Build Ubuntu for the Linux version (WSL)", str(work / "wsl"),
                         "Only when the Linux version is built; removed again afterwards."))
        return rows

    def _show_locations(self) -> None:
        rows = self._locations()
        self._table_window("Where things go", "Every folder the tool uses, and what is in it.",
                           ["What", "Folder", "Notes"], rows, "",
                           [("Close", None)], open_column=1, shares=(0.22, 0.30, 0.36))

    # Repair
    def _run_repair_check(self) -> None:
        out = self._out_path()
        if out is None:
            self._info("No output folder", "Choose the output folder on Set up first.", kind="warning")
            return
        self.v_repair_reason.set("Checking the build...")

        def work():
            report = repair.check_install(out)
            self.q.put(("call", lambda: self._show_repair(report)))
        self._bg(work, "Checking the build...")

    def _show_repair(self, report) -> None:
        if not hasattr(self, "repair_list"):
            return
        self._repair_report = report
        for w in self.repair_list.winfo_children():
            w.destroy()
        if report is not None:
            self.v_status.set("Ready.")
            if not report.issues:
                ttk.Label(self.repair_list, text="No problems found.", style="GoodBold.TLabel").pack(
                    anchor="w", pady=(px(8), 0))
            for issue in report.issues[:40]:
                row = ttk.Frame(self.repair_list)
                row.pack(fill="x", anchor="w", pady=(px(8), 0))
                ttk.Label(row, text="Error" if issue.severity == "error" else "Warning",
                          style="BadBold.TLabel" if issue.severity == "error" else "WarnBold.TLabel",
                          width=9).grid(row=0, column=0, sticky="nw")
                ttk.Label(row, text=issue.title, style="Bold.TLabel", wraplength=px(560),
                          justify="left").grid(row=0, column=1, sticky="w")
                more = issue.detail
                if issue.fix:
                    more = (more + "\n" if more else "") + "Repair: " + _fix_label(issue.fix) + "."
                elif issue.severity == "error":
                    more = (more + "\n" if more else "") + "Repair can't fix this by itself."
                if more:
                    hint(row, more, wrap=560).grid(row=1, column=1, sticky="w")
            if len(report.issues) > 40:
                hint(self.repair_list, f"... and {len(report.issues) - 40} more (see the log).").pack(anchor="w")
        self._update_repair_state()
        if "tools" in getattr(self, "pages", {}):
            self.pages["tools"]["scroll"].relayout()

    def _update_repair_state(self) -> None:
        if not hasattr(self, "btn_repair"):
            return
        report = self._repair_report
        fixable = [i for i in report.issues if i.fix] if report is not None else []
        running = bool(self.worker and self.worker.is_alive())
        reason, style, blocked = "", "Hint.TLabel", True
        if running:
            reason = "A job is running."   # [acct2-decomp] any job, not only a build
        elif report is None:
            reason = "Check the build first."
        elif not fixable:
            reason = "Nothing to repair." if not report.issues else "Nothing here that Repair can fix by itself."
        else:
            # A repair takes the game files from the output folder when they're
            # fine there, so it may not need the disc.
            reason = self._blocker(self._options_with(repair.repair_overrides(report)), "Repair")
            style = "Warn.TLabel"
            blocked = bool(reason)
        if not reason and fixable:
            steps = [s[:1].lower() + s[1:] for s in dict.fromkeys(_fix_label(i.fix) for i in fixable)]
            reason = "Repair will " + (", ".join(steps[:-1]) + " and " + steps[-1] if len(steps) > 1
                                       else steps[0]) + "."
            style = "Accent.TLabel"
        self.btn_repair.configure(state="disabled" if blocked else "normal")
        self.v_repair_reason.set(reason)
        self.lbl_repair_reason.configure(style=style)

    def _start_repair(self) -> None:
        report = self._repair_report
        if report is None:
            return
        overrides = repair.repair_overrides(report)
        steps = list(dict.fromkeys(_fix_label(i.fix) for i in report.issues if i.fix))
        if not widgets.ask_yes_no(self.root, "Repair", "Repair the build?\nThis will:\n- " + "\n- ".join(steps) +
                                  "\n\nAnything it needs to download is listed for your approval first.",
                                  yes="Repair", no="Cancel"):
            return
        self._repair_report = None
        self._show_repair(None)
        self._start_build(overrides, what="Repair")

    # [acct2-decomp] The disc's MD5s, file by file (c3w/disc_files.py)
    def _check_disc_md5(self) -> None:
        out = self._out_path()
        if out is None:
            self._info("No output folder", "Choose the output folder on Set up first.", kind="warning")
            return
        from . import disc_files

        def work():
            last = [0.0]

            def progress(done: int, total: int, rel: str) -> None:
                now = time.time()
                if total and now - last[0] > 0.3:
                    last[0] = now
                    text = f"Checking game files: {100 * done / total:.0f}% ({rel})"
                    self.q.put(("call", lambda t=text: self.v_status.set(t)))
            report = disc_files.check(out, deep=True, progress=progress)
            self.q.put(("call", lambda: self._show_disc_md5(report)))
        self._bg(work, "Checking game files...")

    def _show_disc_md5(self, report) -> None:
        from . import disc_files
        self.v_status.set("Ready.")
        if report.disc is None:
            self._info("Game files", report.summary(), kind="warning")
            return
        order = {s: i for i, s in enumerate(("changed", "size", "missing", "other_disc", "extra", "ok"))}
        rows = []
        for r in sorted(report.rows, key=lambda r: (order.get(r.status, 9), r.path.lower())):
            if r.status == "missing":
                yours = "(missing)"
            elif r.md5 and r.md5 == r.expected_md5:
                yours = "the same"
            else:
                yours = r.md5 or "-"
            status = disc_files.STATUS_TEXT.get(r.status, r.status)
            if r.note:
                status += f" ({r.note})"
            rows.append((r.path, status, r.expected_md5 or "-", yours))

        def save() -> None:
            p = filedialog.asksaveasfilename(parent=self.root, title="Save the list", defaultextension=".tsv",
                                             initialfile="game-files-md5.tsv",
                                             filetypes=[("Table", "*.tsv"), ("All files", "*")])
            if p:
                disc_files.write_tsv(report, Path(p))
        note = ("Files that are missing or another size: Repair takes them off the disc again. A file with "
                "another MD5 but the same size: delete it, then Repair.") if report.problems else ""
        self._table_window("Game files", report.summary(), ["File", "Status", "MD5 on the disc", "Your file"],
                           rows, note, [("Close", None), ("Save the list...", save)],
                           shares=(0.34, 0.18, 0.24, 0.24))

    # Profiles
    def _refresh_profiles(self, select: Optional[str] = None) -> None:
        if not hasattr(self, "lb_profiles"):
            return
        try:
            names = profiles.list_profiles(self.config_dir)
        except (SetupError, OSError):
            names = []
        current = select or self._selected_profile()
        self.lb_profiles.delete(0, "end")
        for n in names:
            self.lb_profiles.insert("end", n)
        if current in names:
            i = names.index(current)
            self.lb_profiles.selection_set(i)
            self.lb_profiles.see(i)
        self.v_profiles_empty.set("" if names else "No profiles yet: Save current as... makes one.")
        self._update_profile_buttons()

    def _selected_profile(self) -> Optional[str]:
        if not hasattr(self, "lb_profiles"):
            return None
        sel = self.lb_profiles.curselection()
        return self.lb_profiles.get(sel[0]) if sel else None

    def _update_profile_buttons(self) -> None:
        if not hasattr(self, "profile_buttons"):
            return
        has = self._selected_profile() is not None
        for key in ("load", "rename", "delete", "export"):
            self.profile_buttons[key].configure(state="normal" if has else "disabled")

    def _profile_name_problem(self, name: str) -> str:
        try:
            profiles.check_name(name)
        except SetupError as e:
            return str(e)
        return ""

    def _profile_notes(self, title: str, first: str, notes: List[str]) -> None:
        if notes:
            self._info(title, first + "\n" + "\n".join("- " + n for n in notes))
        else:
            self.v_status.set(first)

    def _profile_save(self) -> None:
        name = widgets.ask_string(self.root, "Save profile", "Name for a profile with the current settings:",
                                  initial=self._selected_profile() or "", ok="Save",
                                  check=self._profile_name_problem)
        if not name:
            return
        try:
            existing = [n.lower() for n in profiles.list_profiles(self.config_dir)]
            if name.lower() in existing and not widgets.ask_yes_no(
                    self.root, "Replace profile", f"Replace the profile \"{name}\"?\nIts settings are replaced by "
                                                  "the current ones.", yes="Replace", no="Cancel"):
                return
            notes: List[str] = []
            profiles.save_profile(self.config_dir, name, self._settings(), notes=notes)
        except (SetupError, OSError) as e:
            self._info("Not saved", f"The profile wasn't saved.\n{e}", kind="error")
            return
        self._refresh_profiles(select=name)
        self._profile_notes("Profile saved", f"Saved the profile \"{name}\".", notes)

    def _profile_load(self) -> None:
        name = self._selected_profile()
        if not name:
            return
        if self.worker and self.worker.is_alive():
            self._info("Build running", "A build is running.\nLoad the profile when it's done.", kind="warning")
            return
        if not widgets.ask_yes_no(self.root, "Load profile", f"Load the profile \"{name}\"?\nThe current settings "
                                  "are replaced. To keep them, save them as a profile first.", yes="Load",
                                  no="Cancel"):
            return
        try:
            notes: List[str] = []
            settings = profiles.load_profile(self.config_dir, name, notes=notes)
        except (SetupError, OSError) as e:
            self._info("Not loaded", f"The profile wasn't loaded.\n{e}", kind="error")
            return
        self._apply_settings(settings)
        self._save_quietly()
        self._profile_notes("Profile loaded", f"Loaded the profile \"{name}\".", notes)

    def _profile_rename(self) -> None:
        old = self._selected_profile()
        if not old:
            return
        new = widgets.ask_string(self.root, "Rename profile", f"New name for \"{old}\":", initial=old, ok="Rename",
                                 check=self._profile_name_problem)
        if not new or new == old:
            return
        try:
            profiles.rename_profile(self.config_dir, old, new)
        except (SetupError, OSError) as e:
            self._info("Not renamed", f"The profile wasn't renamed.\n{e}", kind="error")
            return
        self._refresh_profiles(select=new)

    def _profile_delete(self) -> None:
        name = self._selected_profile()
        if not name or not widgets.ask_yes_no(self.root, "Delete profile", f"Delete the profile \"{name}\"?\nYour "
                                              "current settings stay as they are.", yes="Delete", no="Keep",
                                              danger=True):
            return
        try:
            profiles.delete_profile(self.config_dir, name)
        except (SetupError, OSError) as e:
            self._info("Not deleted", str(e), kind="error")
            return
        self._refresh_profiles()

    def _profile_export(self) -> None:
        name = self._selected_profile()
        if not name:
            return
        dest = filedialog.asksaveasfilename(title="Export profile", parent=self.root, initialfile=f"{name}.json",
                                            defaultextension=".json", filetypes=[("Profiles", "*.json"),
                                                                                 ("All files", "*")])
        if not dest:
            return
        try:
            notes: List[str] = []
            more = profiles.export_profile(self.config_dir, name, Path(dest), notes=notes)
            if isinstance(more, list):
                notes += [n for n in more if n not in notes]
        except (SetupError, OSError) as e:
            self._info("Not exported", f"The profile wasn't exported.\n{e}", kind="error")
            return
        self._profile_notes("Profile exported", f"Exported \"{name}\" to {dest}.", notes)

    def _profile_import(self) -> None:
        src = filedialog.askopenfilename(title="Import profile", parent=self.root,
                                         filetypes=[("Profiles", "*.json"), ("All files", "*")])
        if not src:
            return
        try:
            notes: List[str] = []
            name = profiles.import_profile(self.config_dir, Path(src), None, notes=notes)
        except (SetupError, OSError) as e:
            self._info("Not imported", f"The profile wasn't imported.\n{e}", kind="error")
            return
        self._refresh_profiles(select=name)
        self._profile_notes("Profile imported", f"Imported the profile \"{name}\". Load it to use it.", notes)

    # ------------------------------------------------------------------ About page
    def _doc_path(self, rel: str) -> Optional[Path]:
        """A file of the project (README, docs/...), or None when this copy
        doesn't have it (the launcher copy in an output folder)."""
        for base in (common.project_dir(), Path(__file__).resolve().parent.parent.parent):
            p = base / rel
            if p.is_file():
                return p
        return None

    def _open_file(self, path: Path) -> None:
        try:
            common.open_in_file_manager(path)
        except OSError as e:
            self._info("Could not open", f"{path}\n{e}", kind="error")

    def _page_about(self, t) -> None:
        t.columnconfigure(0, weight=1)
        # [f2-menus] 2026-11-10 the three Xbox 360 ports named; "the Rust tools" said what they are instead
        widgets.page_header(t, common.APP_NAME, f"Version {TOOL_VERSION}. Call of Duty 3, Big Red One, World at "
                                                "War, Call of Duty 2 and Call of Duty 4 for PC from your own discs, "
                                                "World at War's content for Call of Duty 3, and the copy and LAN "
                                                "server tools for the PC Call of Duty 2 and United Offensive.",
                            columns=1)   # [acct2-bro]
        d = section(t, "Documents", 1, columns=1)
        d.columnconfigure(1, weight=1)
        docs = [("README.md", "Read me", "How to set up and play; the project folder."),
                ("docs/SECURITY.md", "Security", "What the tool downloads, runs and deletes, and how it checks."),
                ("docs/UPDATES.md", "Updates", "How the setup tool checks and signs its updates."),  # [updater]
                ("docs/TEXTURE_REPLACEMENT.md", "Texture replacement", "Your own pictures in place of textures."),
                ("docs/SOURCES.md", "Sources", "Every download, its version and checksum."),
                ("docs/REQUIREMENTS.md", "Requirements", "What your PC needs."),
                ("LICENSE", "License", "MIT, for this project's code.")]
        for r, (rel, label, what) in enumerate(docs):
            path = self._doc_path(rel)
            b = ttk.Button(d, text=label, width=20, command=lambda p=path: self._open_file(p))
            b.grid(row=r, column=0, sticky="w", pady=px(2))
            if path is None:
                b.configure(state="disabled")
                tip(b, f"{rel} isn't in this copy of the tool (it's in the project folder).")
            else:
                tip(b, str(path))
            hint(d, what).grid(row=r, column=1, sticky="w", padx=(px(12), 0))

        self._about_updates(t, 3)   # [updater] c3w/gui_updates.py

        k = section(t, "Keyboard shortcuts", 5, columns=1)
        for r, (keys, what) in enumerate(SHORTCUTS):
            keys = keys.format(n=min(9, max(len(pages) for pages in self.page_lists.values())))
            ttk.Label(k, text=keys, style="Bold.TLabel").grid(row=r, column=0, sticky="w", pady=px(1))
            ttk.Label(k, text=what).grid(row=r, column=1, sticky="w", padx=(px(16), 0))

        lic = section(t, "Licenses and credits", 7, columns=1)
        hint(lic, "This project's code is under the MIT license. The SDK patches in COD_3_WAW_Setup-tool/patches "
                  "are under the ReXGlue SDK's BSD 3-Clause license. Built on the ReXGlue SDK and the Xenia "
                  "project it derives from. The game's settings window uses the Barlow font (SIL Open Font "
                  "License). Texture upscaling uses waifu2x and waifu2x-ncnn-vulkan (MIT). Big Red One is "
                  "recompiled with xboxrecomp (MIT) and its disassembler Capstone (BSD). Every download is "
                  "listed in docs/SOURCES.md.").pack(anchor="w")   # [acct2-bro] xboxrecomp
        hint(lic, "Call of Duty is a trademark of Activision. This project contains no Activision code or assets: "
                  "everything game-specific is made on your PC from your own copies. The tab headers are drawn by "
                  "this tool; no game artwork or logo is used.").pack(anchor="w", pady=(px(8), px(18)))

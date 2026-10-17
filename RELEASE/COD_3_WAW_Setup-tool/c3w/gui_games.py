"""The game tabs: the strip of tabs across the top, the header drawn for
each game, each tab's own sidebar, and the pages of the World at War, Call
of Duty 2 and United Offensive tabs.

- World at War: Content (its sources, map packs and "Rip content", moved
  here from Call of Duty 3's Set up page), Mods & maps (when switched on)
  and Port status.
- Call of Duty 2 and United Offensive: New copy (the Rust tool newcopy:
  find the install, make and check the data-only copy), LAN server (the
  Rust iw-server, started and stopped from here) and Port status.

A mixin for gui.App, like gui_pages.Pages: it uses the App's variables and
helpers (v_out, _row, _open, _info, _start_task ...).
"""
from __future__ import annotations

import threading
import tkinter as tk
import tkinter.font as tkfont
from pathlib import Path
from tkinter import ttk
from typing import Dict, List, Optional

from . import common, games, gui_art, pc_games, theme, waw, widgets
from .common import SetupError
from .widgets import hint, px, section, tip

DISC_TYPES = [("Disc images", "*.iso *.ISO")]
HERO_H = games.HERO_H                   # the header's usual height, for the first window size
# The main buttons in the bottom bar: (key, label). Upper case, like a launcher's.
ACTION_LABELS = {"build": "BUILD", "play": "PLAY", "rip": "RIP CONTENT", "copy": "MAKE A NEW COPY",
                 "server": "START LAN SERVER", "stop": "STOP SERVER"}
STATE_STYLES = {games.READY: "ChipGood.TLabel", games.TESTING: "ChipWarn.TLabel", games.NEXT: "ChipInfo.TLabel",
                games.LATER: "ChipDim.TLabel"}


def show_text(label, text: str) -> None:
    """A gridded label that takes no room while it has nothing to say."""
    label.configure(text=text)
    if text:
        label.grid()
    else:
        label.grid_remove()


def language_label(lang: str) -> str:
    return lang.title() if lang else "Automatic"


def language_value(label: str) -> str:
    return "" if label in ("", "Automatic") else label.lower()


class Games:
    # ------------------------------------------------------------------ tab bar and header
    def _build_tabbar(self, parent) -> ttk.Frame:
        """The game tabs across the top: one per game, each with a colored
        line under it (the game's own accent; bright when selected)."""
        bar = ttk.Frame(parent, style="TabBar.TFrame")
        box = self.tab_badge_box = ttk.Frame(bar, style="TabBar.TFrame", width=px(200))
        box.pack(side="left", fill="y")
        box.pack_propagate(False)
        self.tab_badge = ttk.Label(box, text="SETUP TOOL", style="TabBar.TLabel")
        self.tab_badge.pack(side="left", padx=(px(18), 0))
        self.tab_items: Dict[str, dict] = {}
        for g in games.GAMES:
            cell = ttk.Frame(bar, style="TabBar.TFrame")
            cell.pack(side="left", fill="y")
            btn = ttk.Button(cell, text=g.tab.upper(), style="GameTab.TButton", cursor="hand2",
                             command=lambda gid=g.id: self.show_game(gid))
            btn.pack(side="top", fill="both", expand=True)
            line = tk.Frame(cell, height=px(3))
            line.c3w_role = "tabline"
            line.pack(side="bottom", fill="x")
            tip(btn, f"{g.name}  (Ctrl+Tab: next tab)")
            self.tab_items[g.id] = {"button": btn, "line": line}
        return bar

    def _build_hero(self, parent) -> tk.Canvas:
        cv = tk.Canvas(parent, height=px(HERO_H), highlightthickness=0, borderwidth=0)
        cv.c3w_role = "hero"
        cv.bind("<Configure>", lambda e: self._redraw_hero_soon(), add="+")
        self._hero_pending = False
        self._hero_size = (0, 0, "")
        self._hero_h = px(HERO_H)
        return cv

    def _fit_hero(self, window_h: int) -> None:
        """The header's height for the window's (hidden in a low window)."""
        want = games.hero_height(window_h, widgets.ui_scale())
        if want == self._hero_h:
            return
        self._hero_h = want
        if want:
            self.hero.configure(height=want)
            self.hero.grid()
            self._redraw_hero_soon()
        else:
            self.hero.grid_remove()

    def _redraw_hero_soon(self) -> None:
        if not self._hero_pending:
            self._hero_pending = True
            self.root.after_idle(self._redraw_hero)

    def _redraw_hero(self, force: bool = False) -> None:
        self._hero_pending = False
        cv = self.hero
        if not self._hero_h:
            return    # hidden in a low window
        g = games.game(self.tabs.game)
        strong = self.tabs.theme_for(g.id) == "contrast"
        key = (cv.winfo_width(), cv.winfo_height(), g.id + theme.current["accent"] + theme.current["bg"] + str(strong))
        if key == self._hero_size and not force:
            return
        self._hero_size = key
        ui = tkfont.nametofont("TkDefaultFont", root=self.root).actual("family")
        small = self._chrome_font[0] if self._chrome_font else ui
        gui_art.draw_hero(cv, g.id, g.wordmark, g.tagline, theme.current, self._display_font, small, ui,
                          strong=strong)

    def _color_tabs(self, c: Dict[str, str]) -> None:
        """Each tab's line in its own game's accent (dim unless selected)."""
        for gid, item in getattr(self, "tab_items", {}).items():
            selected = gid == self.tabs.game
            item["button"].configure(style="GameTabSelected.TButton" if selected else "GameTab.TButton")
            if selected:
                color = c["accent"]
            else:
                own = theme.colors(self.tabs.theme_for(gid), self.tabs.accent_for(gid))["accent"]
                color = theme.mix(c["tabbar"], own, 0.55)
            try:
                item["line"].configure(background=color)
            except tk.TclError:
                pass

    def show_game(self, game_id: str, page: Optional[str] = None) -> None:
        """Switches to a game's tab: its look, its sidebar, its main buttons
        and its last page (or `page`)."""
        if game_id not in self.page_lists:
            return
        changed = game_id != self.tabs.game or not getattr(self, "_shown_game", None)
        _, page = self.tabs.show(game_id, page)
        if changed:
            self._shown_game = game_id
            for gid, nav in self.navs.items():
                if gid == game_id:
                    nav.pack(side="top", fill="x")
                else:
                    nav.pack_forget()
            for gid, frame in self.action_frames.items():
                if gid == game_id:
                    frame.pack(side="left")
                else:
                    frame.pack_forget()
            g = games.game(game_id)
            self.cb_theme.configure(values=[theme.picker_label(t, g.palette) for t in theme.palettes_for(g.palette)])
            self._apply_look()
            self._update_build_state()
        self.show_page(page)

    def _step_game(self, delta: int) -> str:
        self.show_game(self.tabs.step(delta))
        return "break"

    def _apply_look(self) -> None:
        """The current tab's colors, accent and title type."""
        gid = self.tabs.game
        self.accent = self.tabs.accent_for(gid)
        self.v_theme.set(theme.picker_label(self.tabs.theme_for(gid), games.game(gid).palette))
        self._display_font = theme.set_game_fonts(self.root, gid)
        theme.apply(self.root, self.tabs.theme_for(gid), self.accent, px)
        self._redraw_hero(force=True)

    def _build_navs(self) -> None:
        """Each tab's sidebar: a button and marker stripe per page."""
        self.navs: Dict[str, ttk.Frame] = {}
        self.nav_items: Dict[tuple, tuple] = {}
        for g in games.GAMES:
            nav = ttk.Frame(self.nav_host, style="Sidebar.TFrame")
            self.navs[g.id] = nav
            for i, key in enumerate(self.page_lists[g.id]):
                label = games.PAGE_LABELS[key]
                row = ttk.Frame(nav, style="Sidebar.TFrame")
                row.pack(fill="x")
                stripe = tk.Frame(row, width=px(4))
                stripe.c3w_role = "stripe"
                stripe.pack(side="left", fill="y")
                btn = ttk.Button(row, text=label, style="Nav.TButton", command=lambda k=key: self.show_page(k))
                btn.pack(side="left", fill="x", expand=True)
                tip(btn, f"{label}  (Ctrl+{i + 1})" if i < 9 else label)
                self.nav_items[(g.id, key)] = (btn, stripe)

    def _show_nth_page(self, n: int) -> None:
        pages = self.page_lists[self.tabs.game]
        if n < len(pages):
            self.show_page(pages[n])

    # ------------------------------------------------------------------ the bottom bar's buttons
    def _build_actions(self, host) -> None:
        """Each tab's main buttons for the bottom bar (only the current tab's
        are shown)."""
        self.action_frames: Dict[str, ttk.Frame] = {}
        self.pc_actions: Dict[str, dict] = {}
        for g in games.GAMES:
            f = ttk.Frame(host)
            self.action_frames[g.id] = f
            if g.id == games.COD3:
                self.btn_build = ttk.Button(f, text=ACTION_LABELS["build"], style="Big.Accent.TButton",
                                            command=self._start_build)
                self.btn_build.pack(side="left")
                tip(self.btn_build, "Rip the game and build what's ticked on Set up (Ctrl+B). Nothing is "
                    "downloaded before you approve the list.", reason=self.v_build_reason.get)
                self.btn_play_bar = ttk.Button(f, text=ACTION_LABELS["play"], style="Big.TButton",
                                               command=self._play_cod3)
                self.btn_play_bar.pack(side="left", padx=(px(6), 0))
                tip(self.btn_play_bar, "Start Call of Duty 3.", reason=lambda: self._play_exe()[1]
                    if self._play_exe()[0] is None else "")
            elif g.id == games.WAW:
                self.btn_rip = ttk.Button(f, text=ACTION_LABELS["rip"], style="Big.Accent.TButton",
                                          command=self._start_rip)
                self.btn_rip.pack(side="left")
                tip(self.btn_rip, "Rip World at War's files for the new engine (Ctrl+B).",
                    reason=self.v_build_reason.get)
            else:
                copy = ttk.Button(f, text=ACTION_LABELS["copy"], style="Big.Accent.TButton",
                                  command=lambda gid=g.id: self._pc_make_copy(gid))
                copy.pack(side="left")
                tip(copy, "Copy the game's data for the Rust engine (Ctrl+B).",
                    reason=lambda gid=g.id: self._pc_copy_reason(gid))
                server = ttk.Button(f, text=ACTION_LABELS["server"], style="Big.TButton",
                                    command=lambda gid=g.id: self._pc_server_toggle(gid))
                server.pack(side="left", padx=(px(6), 0))
                tip(server, "Start or stop the Rust LAN server.", reason=lambda gid=g.id: self._pc_server_reason(gid))
                self.pc_actions[g.id] = {"copy": copy, "server": server}

    def _compact_actions(self, compact: bool) -> None:
        """In a narrow or low window the bottom bar's buttons are the usual
        size (all of them, so they stay one height)."""
        self._actions_compact = compact
        for b in self._action_buttons():
            style = str(b.cget("style"))
            if compact and style.startswith("Big."):
                b.configure(style=style[4:])
            elif not compact and not style.startswith("Big."):
                b.configure(style="Big." + style)

    def _action_buttons(self) -> List[ttk.Button]:
        out = [self.btn_build, self.btn_play_bar, self.btn_rip, self.btn_cancel, self.btn_open_out]
        for b in self.pc_actions.values():
            out += [b["copy"], b["server"]]
        return out

    def _action_style(self, accent: bool) -> str:
        style = "Accent.TButton" if accent else "TButton"
        return style if getattr(self, "_actions_compact", False) else "Big." + style

    def _main_action(self) -> None:
        """Ctrl+B: the current tab's main button."""
        gid = self.tabs.game
        if gid == games.COD3:
            self._shortcut_build()
        elif gid == games.WAW:
            if str(self.btn_rip.cget("state")) != "disabled":
                self._start_rip()
            elif self.v_build_reason.get():
                self.v_status.set(self.v_build_reason.get())
        else:
            why = self._pc_copy_reason(gid)
            if why:
                self.v_status.set(why)
            else:
                self._pc_make_copy(gid)

    def _update_actions(self) -> None:
        """Every tab's main buttons on or off, and the current tab's reason
        under them."""
        if not hasattr(self, "btn_rip"):
            return
        busy = bool(self.worker and self.worker.is_alive())
        exe, _ = self._play_exe()
        self.btn_play_bar.configure(state="normal" if exe else "disabled")
        rip_why = "" if busy else self._rip_blocker()
        self.btn_rip.configure(state="disabled" if busy or rip_why else "normal")
        for gid, b in self.pc_actions.items():
            copy_why = self._pc_copy_reason(gid)
            b["copy"].configure(state="disabled" if copy_why else "normal")
            running = self._pc_server_running(gid)
            b["server"].configure(text=ACTION_LABELS["stop" if running else "server"],
                                  style=self._action_style(accent=running),
                                  state="disabled" if self._pc_server_reason(gid) else "normal")
        gid = self.tabs.game
        if gid == games.WAW:
            reason = rip_why
        elif gid in self.pc_actions:
            reason = "" if busy else (self._pc_copy_reason(gid) or "")
        else:
            return   # Call of Duty 3's reason is set by _update_build_state
        self.v_build_reason.set(reason)
        if reason:
            self.lbl_reason.grid()
        else:
            self.lbl_reason.grid_remove()

    def _reason_clicked(self) -> None:
        """The reason under the main button was clicked: the page that fixes it."""
        gid = self.tabs.game
        self.show_page({games.COD3: "setup", games.WAW: "waw_content"}.get(gid, f"{gid}_copy"))

    # ------------------------------------------------------------------ port status pages
    def _page_status(self, t, game_id: str) -> None:
        g = games.game(game_id)
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Port status", f"How far {g.name} is, step by step.", columns=1)
        s = section(t, "Steps", 1, columns=1)
        s.columnconfigure(2, weight=1)
        for r, m in enumerate(games.PORT_STATUS.get(game_id, ())):
            ttk.Label(s, text=games.STATE_LABELS[m.state], style=STATE_STYLES[m.state], width=8).grid(
                row=r, column=0, sticky="nw", pady=px(4))
            ttk.Label(s, text=m.what, style="Bold.TLabel").grid(row=r, column=1, sticky="nw", padx=(px(12), px(12)),
                                                                pady=px(4))
            hint(s, m.note).grid(row=r, column=2, sticky="nw", pady=px(4))
        docs = [(rel, label) for rel, label in games.STATUS_DOCS.get(game_id, ()) if self._doc_path(rel)]
        if docs:
            d = section(t, "Read more", 3, columns=1)
            row = ttk.Frame(d)
            row.pack(anchor="w", pady=(0, px(18)))
            for rel, label in docs:
                path = self._doc_path(rel)
                widgets.link(row, label, lambda p=path: self._open_file(p)).pack(side="left", padx=(0, px(14)))

    # ------------------------------------------------------------------ World at War: Content
    def _page_waw_content(self, t) -> None:
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "Content", "Rip World at War's files from your own copy. It is only read, "
                                          "never run or changed.", columns=1)
        self.pages["waw_content"]["on_show"] = self._validate_waw
        ww = section(t, "Your copy", 1, columns=1)
        self._grid_cols(ww)
        self.steam_widgets: list = []
        self._row(ww, 0, "Steam folder", self.v_steam, [
            ("Find", self._find_steam, "Look in your Steam libraries."),
            ("Browse...", lambda: self._pick_dir(self.v_steam, "World at War folder in your Steam library"), "")],
            widgets_out=self.steam_widgets, tooltip="When set, Steam is the only source used.")
        hint(ww, "On Steam? That's all you need. Otherwise, use a disc:").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(px(2), px(6)))
        rb1 = ttk.Radiobutton(ww, text="Xbox 360 disc image", value=waw.XBOX360, variable=self.v_alt)
        rb1.grid(row=2, column=0, sticky="w", pady=px(3))
        e1 = ttk.Entry(ww, textvariable=self.v_xbox, width=8)   # (stretches; narrow at least)
        e1.grid(row=2, column=1, sticky="ew", pady=px(3))
        bf1 = ttk.Frame(ww)
        bf1.grid(row=2, column=2, sticky="e", padx=(px(6), 0))
        b1 = ttk.Button(bf1, text="Browse...", command=lambda: self._pick_file(
            self.v_xbox, "World at War Xbox 360 disc image", DISC_TYPES))
        b1.pack(side="left", padx=(px(4), 0))
        rb2 = ttk.Radiobutton(ww, text="PC disc", value=waw.PC_DISC, variable=self.v_alt)
        rb2.grid(row=3, column=0, sticky="w", pady=px(3))
        e2 = ttk.Entry(ww, textvariable=self.v_pc, width=8)
        e2.grid(row=3, column=1, sticky="ew", pady=px(3))
        pf = ttk.Frame(ww)
        pf.grid(row=3, column=2, sticky="e", padx=(px(6), 0))
        b2 = ttk.Button(pf, text="Image...", command=lambda: self._pick_file(
            self.v_pc, "World at War PC disc image", DISC_TYPES))
        b2.pack(side="left", padx=(px(4), 0))
        b3 = ttk.Button(pf, text="Drive/folder...", command=lambda: self._pick_dir(
            self.v_pc, "The World at War PC disc (drive), or a folder installed from it"))
        b3.pack(side="left", padx=(px(4), 0))
        tip(b3, "The disc drive, or a folder installed from the PC disc.")
        self.lbl_waw_status = hint(ww, var=self.v_waw_status, style="Accent.TLabel")
        self.lbl_waw_status.grid(row=4, column=0, columnspan=3, sticky="w", pady=(px(8), 0))

        mp = section(t, "Map packs", 3, columns=1)
        self._grid_cols(mp)
        ttk.Label(mp, text="Downloads").grid(row=0, column=0, sticky="nw", pady=px(3))
        hint(mp, var=self.v_pack, style="TLabel").grid(row=0, column=1, columnspan=2, sticky="w", pady=px(3))
        mf = ttk.Frame(mp)
        mf.grid(row=1, column=1, columnspan=2, sticky="w", pady=(px(2), px(4)))
        b4 = ttk.Button(mf, text="Add...", command=self._add_packs)
        b4.pack(side="left")
        tip(b4, "Add the official PC patch downloads (1.4, 1.5, 1.6).")
        b5 = ttk.Button(mf, text="Clear", command=lambda: self._set_packs([]))
        b5.pack(side="left", padx=(px(4), 0))
        b6 = ttk.Button(mf, text="Download pages", command=self._open_pack_pages)
        b6.pack(side="left", padx=(px(4), 0))
        tip(b6, "Open the patches' download pages in your browser.")
        hint(mp, "Only for discs (Steam has them all). The patches are unpacked, never run.").grid(
            row=2, column=0, columnspan=3, sticky="w")
        self.alt_widgets = [rb1, e1, b1, rb2, e2, b2, b3, b4, b5, b6]
        for wdg in self.alt_widgets:
            tip(wdg, reason=lambda: "Not used: a Steam folder is set." if self.v_steam.get().strip() else "")

        o = section(t, "Options", 5, columns=1)
        ttk.Checkbutton(o, text="Also rip it when Call of Duty 3 is built", variable=self.v_use_waw,
                        command=self._update_build_state).pack(anchor="w")
        bf = ttk.Frame(o)
        bf.pack(anchor="w", pady=(px(10), px(18)))
        b = ttk.Button(bf, text="Open ripped files", command=self._open_waw_data)
        b.pack(side="left")
        tip(b, "data/waw in the output folder.")

    def _open_waw_data(self) -> None:
        out = self.v_out.get().strip()
        self._open(str(Path(out) / waw.OUTPUT_DATA) if out else "")

    def _waw_source(self) -> waw.Source:
        return waw.choose_source(self.v_steam.get(), self.v_xbox.get(), self.v_pc.get(), self.v_alt.get())[0]

    def _rip_blocker(self) -> str:
        """Why "Rip content" can't start ("" when it can)."""
        if self._waw_source().kind == waw.NONE:
            return "Rip needs your World at War: a Steam folder or a disc (Content page)."
        if not self.v_out.get().strip():
            return "Rip needs an output folder: choose one on Call of Duty 3's Set up page."
        return ""

    def _start_rip(self) -> None:
        """Rips only World at War (the pipeline's "waw" step, with the
        ripper it needs), whatever "Also rip it" says."""
        why = self._rip_blocker()
        if why:
            self.v_status.set(why)
            return
        packs = list(self.packs) if not self.v_steam.get().strip() else []
        self._start_build({"only_steps": ["waw"], "build_recomp": False, "waw_steam": self.v_steam.get().strip(),
                           "waw_xbox360": self.v_xbox.get().strip(), "waw_pc_disc": self.v_pc.get().strip(),
                           "waw_map_packs": packs},
                          what="Rip", blocker=lambda opts, what: self._rip_blocker(),
                          done=("Open ripped files", self._open_waw_data))

    # ------------------------------------------------------------------ Call of Duty 2 / United Offensive: settings
    def _init_pc_settings(self, s: dict) -> None:
        """Each PC game's choices, from settings.json (first start: the
        install found in Steam, the copy in the output folder)."""
        self.pc: Dict[str, Dict[str, tk.Variable]] = {}
        self.pc_ui: Dict[str, dict] = {}
        self.pc_servers: Dict[str, pc_games.ServerProcess] = {}
        self._pc_install_ok: Dict[str, str] = {}
        self._pc_gen: Dict[str, int] = {}
        self._pc_searching: set = set()   # first start: Steam is searched off the UI thread
        for gid, pg in pc_games.PC_GAMES.items():
            install = s.get(f"{gid}_install")
            if not isinstance(install, str):
                install = ""
                self._pc_searching.add(gid)
            port = s.get(f"{gid}_port")
            clients = s.get(f"{gid}_max_clients")
            self.pc[gid] = {
                "install": tk.StringVar(value=install),
                "copy_dir": tk.StringVar(value=s.get(f"{gid}_copy_dir") or
                                         pc_games.default_copy_dir(self.v_out.get())),
                "full_copy": tk.BooleanVar(value=bool(s.get(f"{gid}_full_copy", False))),
                "language": tk.StringVar(value=language_label(s.get(f"{gid}_language") or "")
                                         if (s.get(f"{gid}_language") or "") in pc_games.LANGUAGES else "Automatic"),
                "map": tk.StringVar(value=s.get(f"{gid}_map") or pg.default_map),
                "port": tk.StringVar(value=str(port if isinstance(port, int) else pc_games.DEFAULT_PORT)),
                "hostname": tk.StringVar(value=s.get(f"{gid}_hostname") or pc_games.DEFAULT_HOSTNAME),
                "max_clients": tk.StringVar(value=str(clients if isinstance(clients, int)
                                                      else pc_games.DEFAULT_MAX_CLIENTS)),
            }
            self._pc_install_ok[gid] = ""
            self._pc_gen[gid] = 0
            v = self.pc[gid]
            v["install"].trace_add("write", lambda *a, g=gid: self._pc_check_install(g))
            for name in ("copy_dir", "map", "port", "hostname", "max_clients"):
                v[name].trace_add("write", lambda *a, g=gid: self._pc_refresh(g))
            self.pc_servers[gid] = pc_games.ServerProcess(
                on_line=lambda line, g=gid: self.q.put(("call", lambda: self._pc_server_line(g, line))),
                on_exit=lambda code, g=gid: self.q.put(("call", lambda: self._pc_server_exited(g, code))))
        for gid in list(self._pc_searching):
            self._pc_find(gid, first_start=True)

    def _pc_settings(self) -> dict:
        out: dict = {}
        for gid, v in self.pc.items():
            if gid not in self._pc_searching or v["install"].get().strip():
                out[f"{gid}_install"] = v["install"].get()   # (still searching: looked for again next time)
            out[f"{gid}_copy_dir"] = v["copy_dir"].get()
            out[f"{gid}_full_copy"] = bool(v["full_copy"].get())
            out[f"{gid}_language"] = language_value(v["language"].get())
            out[f"{gid}_map"] = v["map"].get().strip()
            out[f"{gid}_hostname"] = v["hostname"].get()
            for key, rng in (("port", pc_games.PORT_RANGE), ("max_clients", pc_games.MAX_CLIENTS_RANGE)):
                value = pc_games.parse_int(v[key].get(), *rng)
                if value is not None:
                    out[f"{gid}_{key}"] = value
        return out

    def _pc_apply_settings(self, s: dict) -> None:
        for gid, v in self.pc.items():
            for key in ("install", "copy_dir", "map", "hostname"):
                if isinstance(s.get(f"{gid}_{key}"), str):
                    v[key].set(s[f"{gid}_{key}"])
            if isinstance(s.get(f"{gid}_full_copy"), bool):
                v["full_copy"].set(s[f"{gid}_full_copy"])
            if isinstance(s.get(f"{gid}_language"), str) and s[f"{gid}_language"] in pc_games.LANGUAGES:
                v["language"].set(language_label(s[f"{gid}_language"]))
            for key in ("port", "max_clients"):
                value = s.get(f"{gid}_{key}")
                if isinstance(value, int) and not isinstance(value, bool):
                    v[key].set(str(value))

    # ------------------------------------------------------------------ Call of Duty 2 / United Offensive: New copy
    def _page_pc_copy(self, t, gid: str) -> None:
        pg = pc_games.PC_GAMES[gid]
        v = self.pc[gid]
        ui = self.pc_ui.setdefault(gid, {})
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "New copy", f"Copy {pg.name}'s data for the Rust engine. Program files never come "
                                           "along, so neither does any DRM.", columns=1)
        self.pages[f"{gid}_copy"]["on_show"] = lambda: self._pc_check_install(gid)
        s = section(t, "Your game", 1, columns=1)
        self._grid_cols(s)
        self._row(s, 0, "Game folder", v["install"], [
            ("Find", lambda: self._pc_find(gid), "Look in your Steam libraries."),
            ("Browse...", lambda: self._pick_dir(v["install"], f"{pg.name} folder (Steam or CB copy)"), "")],
            tooltip="Your Steam or CB install. Empty: newcopy looks in Steam.")
        ui["install_status"] = hint(s, "")
        ui["install_status"].grid(row=1, column=1, columnspan=2, sticky="w")

        c = section(t, "The copy", 3, columns=1)
        self._grid_cols(c)
        self._row(c, 0, "Copy folder", v["copy_dir"], [
            ("Browse...", lambda: self._pick_dir(v["copy_dir"], "Where the new copy goes"), ""),
            ("Open", lambda: self._open(self._pc_copy_path(gid)), "")],
            tooltip=f"The copy goes into {gid}/ in this folder.")
        ttk.Label(c, text="Language").grid(row=1, column=0, sticky="w", pady=px(3))
        lang = ttk.Combobox(c, textvariable=v["language"], state="readonly", width=14,
                            values=[language_label(x) for x in pc_games.LANGUAGES])
        lang.grid(row=1, column=1, sticky="w", pady=px(3))
        tip(lang, "Only matters when your install has several.")
        full = ttk.Checkbutton(c, text="Full copy (no hard links)", variable=v["full_copy"])
        full.grid(row=2, column=0, columnspan=3, sticky="w", pady=(px(4), 0))
        tip(full, "Copies every file. Otherwise files on the same drive are hard-linked: instant, no extra space.")
        ui["copy_reason"] = hint(c, "", style="Warn.TLabel")
        ui["copy_reason"].grid(row=3, column=0, columnspan=3, sticky="w", pady=(px(4), 0))

        k = section(t, "Check", 5, columns=1)
        bf = ttk.Frame(k)
        bf.pack(anchor="w")
        widgets.button_row(bf, [("Check game", lambda: self._pc_run_newcopy(gid, "find"),
                                 "List what newcopy finds: version, language, missing files."),
                                ("Check copy", lambda: self._pc_run_newcopy(gid, "check"),
                                 "Read every file of the copy and compare it with its list.")])
        hint(k, "Results go to the Log page.").pack(anchor="w", pady=(px(6), px(18)))
        self._pc_check_install(gid)

    def _pc_copy_path(self, gid: str) -> str:
        """The copy itself: <copy folder>/<game>."""
        base = self.pc[gid]["copy_dir"].get().strip()
        return str(Path(base) / gid) if base else ""

    def _pc_find(self, gid: str, first_start: bool = False) -> None:
        """Looks for the game in the Steam libraries, off the UI thread (it
        lists folders, maybe on slow drives). At first start, quietly: a
        folder found fills the empty box."""
        pg = pc_games.PC_GAMES[gid]
        if not first_start:
            self.v_status.set(f"Looking for {pg.name} in your Steam libraries...")

        def work():
            found = pc_games.find_steam(pg)

            def done():
                var = self.pc[gid]["install"]
                if first_start:
                    self._pc_searching.discard(gid)
                    if found and not var.get().strip():
                        var.set(str(found[0]))
                elif found:
                    var.set(str(found[0]))
                    self.v_status.set(f"Found: {found[0]}" +
                                      (f" (and {len(found) - 1} more)" if len(found) > 1 else ""))
                else:
                    self.v_status.set("Ready.")
                    self._info("Not found", f"{pg.name} isn't in your Steam libraries.\n"
                                            "Use Browse to pick its folder (a CB copy works too).")
            self.q.put(("call", done))
        threading.Thread(target=work, daemon=True).start()

    def _pc_check_install(self, gid: str) -> None:
        """Checks the game folder off the UI thread (it lists folders)."""
        if gid not in self.pc:
            return
        self._pc_gen[gid] += 1
        gen = self._pc_gen[gid]
        text = self.pc[gid]["install"].get()
        pg = pc_games.PC_GAMES[gid]

        def work():
            if not text.strip():
                found = pc_games.find_steam(pg)
                msg = (f"Empty: newcopy uses Steam ({found[0]})." if found else
                       "Empty: newcopy looks in Steam, but it isn't there. Pick the folder.")
                result = ("", msg, "Hint.TLabel" if found else "Warn.TLabel")
            else:
                why = pc_games.check_install(text, pg)
                ok_msg = pc_games.recognize(Path(text.strip()), pg)[1] if not why else ""
                result = (why, ok_msg or why, "Warn.TLabel" if why else "Good.TLabel")

            def done():
                if gen != self._pc_gen.get(gid):
                    return   # a newer check started meanwhile
                self._pc_install_ok[gid] = result[0]
                lbl = self.pc_ui.get(gid, {}).get("install_status")
                if lbl is not None:
                    lbl.configure(text=result[1], style=result[2])
                self._pc_refresh(gid)
            self.q.put(("call", done))
        threading.Thread(target=work, daemon=True).start()

    def _pc_copy_reason(self, gid: str) -> str:
        """Why "Make a new copy" can't start ("" when it can)."""
        if self.worker and self.worker.is_alive():
            return "Busy: wait for the job running now, or cancel it."
        v = self.pc[gid]
        return (self._pc_install_ok.get(gid) or
                pc_games.check_copy_folder(v["copy_dir"].get(), v["install"].get()))

    def _pc_refresh(self, gid: str) -> None:
        """The page's reason lines and the main buttons."""
        ui = self.pc_ui.get(gid, {})
        v = self.pc[gid]
        if "copy_reason" in ui:
            show_text(ui["copy_reason"], pc_games.check_copy_folder(v["copy_dir"].get(), v["install"].get()))
        if "server_reason" in ui:
            show_text(ui["server_reason"], self._pc_server_reason(gid, inputs_only=True))
        self._update_actions()

    def _pc_tool_maker(self, tool: str):
        """A function for the worker thread that returns newcopy or iw-server,
        built first when needed. The form is read here, on the UI thread."""
        out = Path(self.v_out.get().strip() or common.default_output_dir())
        work = Path(self.v_work.get().strip() or common.default_work_dir())
        jobs, allow_install = self._jobs(), bool(self.v_install.get())
        tool_root = Path(__file__).resolve().parent.parent

        def make(runner: common.Runner) -> Path:
            common.claim_work_dir(work)
            return pc_games.ensure_tool(tool, tool_root, out, work, jobs, runner, self.log, self.cancel,
                                        allow_install)
        return make

    def _pc_make_copy(self, gid: str) -> None:
        why = self._pc_copy_reason(gid)
        if why:
            self.v_status.set(why)
            return
        pg = pc_games.PC_GAMES[gid]
        v = self.pc[gid]
        install, out = v["install"].get(), v["copy_dir"].get()
        full, lang = bool(v["full_copy"].get()), language_value(v["language"].get())
        tool = self._pc_tool_maker("newcopy")
        self._save_quietly()

        def work(runner: common.Runner) -> str:
            exe = tool(runner)
            Path(out.strip()).mkdir(parents=True, exist_ok=True)
            runner.run(pc_games.newcopy_make(exe, pg, out, install, full, lang),
                       line_filter=pc_games.copy_progress(self.log))
            return f"The new copy is in {Path(out.strip()) / gid}."
        self._start_task("New copy", work, [("Open copy", lambda: self._open(self._pc_copy_path(gid)))])

    def _pc_run_newcopy(self, gid: str, command: str) -> None:
        """Check game (newcopy find) or Check copy (newcopy check); the report
        goes to the log, and its gist to a message."""
        pg = pc_games.PC_GAMES[gid]
        v = self.pc[gid]
        install = v["install"].get()
        copy_dir = self._pc_copy_path(gid)
        if command == "check" and not Path(copy_dir or ".").joinpath("newcopy.toml").is_file():
            self._info("No copy yet", f"There is no new copy in {copy_dir or 'the copy folder'} yet.\n"
                                      "Make one first.", kind="warning")
            return
        if command == "find" and self._pc_install_ok.get(gid):
            self._info("Check the folder", self._pc_install_ok[gid], kind="warning")
            return
        tool = self._pc_tool_maker("newcopy")

        def work(runner: common.Runner) -> str:
            exe = tool(runner)
            lines: List[str] = []

            def keep(line: str) -> None:
                lines.append(line)
            if command == "find":
                runner.run(pc_games.newcopy_find(exe, install), line_filter=keep)
                # The chosen folder's entry (newcopy lists Steam's installs first).
                f = pc_games.pick_found(pc_games.parse_find("\n".join(lines)), gid, install)
                if f is None:
                    raise SetupError(f"newcopy found no {pg.name} install" +
                                     (" in the game folder." if install.strip() else ". Pick its folder."))
                return pc_games.find_summary(f)
            rc = runner.run(pc_games.newcopy_check(exe, copy_dir), check=False, line_filter=keep)
            if rc != 0:
                raise SetupError("The copy doesn't match its list; Make a new copy repairs it. See the Log page.")
            return "Every file of the copy matches."
        self._start_task("Check" if command == "check" else "Check game", work,
                         [("Show log", lambda: self.show_page("log"))])

    # ------------------------------------------------------------------ Call of Duty 2 / United Offensive: LAN server
    def _page_pc_server(self, t, gid: str) -> None:
        pg = pc_games.PC_GAMES[gid]
        v = self.pc[gid]
        ui = self.pc_ui.setdefault(gid, {})
        t.columnconfigure(0, weight=1)
        widgets.page_header(t, "LAN server", f"Host a game your {pg.name} can join. Our own Rust server: your "
                                             "game's program is only read, never run.", columns=1)
        # The form two fields a row (map and port, name and players), so the
        # server's output is in view without scrolling.
        s = section(t, "Server", 1, columns=1)
        self._grid_cols(s)
        s.columnconfigure(1, weight=0)
        s.columnconfigure(4, weight=1)
        fields = (("Map", ttk.Combobox(s, textvariable=v["map"], values=list(pg.maps), width=20),
                   "Pick one, or type the name of another map you have."),
                  ("Port", ttk.Spinbox(s, from_=pc_games.PORT_RANGE[0], to=pc_games.PORT_RANGE[1],
                                       textvariable=v["port"], width=7),
                   f"UDP. The games use {pc_games.DEFAULT_PORT}."),
                  ("Server name", ttk.Entry(s, textvariable=v["hostname"], width=22), "What the LAN list shows."),
                  ("Players", ttk.Spinbox(s, from_=pc_games.MAX_CLIENTS_RANGE[0], to=pc_games.MAX_CLIENTS_RANGE[1],
                                          textvariable=v["max_clients"], width=7), "Most players at once."))
        for i, (label, field, help_text) in enumerate(fields):
            r, c = divmod(i, 2)
            ttk.Label(s, text=label).grid(row=r, column=c * 2, sticky="w", pady=px(3),
                                          padx=(px(24) if c else 0, px(10)))
            field.grid(row=r, column=c * 2 + 1, sticky="w", pady=px(3))
            tip(field, help_text)
        ui["server_reason"] = hint(s, "", style="Warn.TLabel")
        ui["server_reason"].grid(row=2, column=0, columnspan=5, sticky="w", pady=(px(4), 0))
        line = ttk.Frame(s)
        line.grid(row=3, column=0, columnspan=5, sticky="w", pady=(px(8), 0))
        ui["server_state"] = ttk.Label(line, text="Stopped.", style="Hint.TLabel")
        ui["server_state"].pack(side="left", padx=(0, px(18)))
        ui["join"] = ttk.Label(line, text="", style="Bold.TLabel")
        ui["join"].pack(side="left")
        b = ttk.Button(line, text="Copy", command=lambda: self._copy_text(self._pc_join_command(gid)))
        b.pack(side="left", padx=(px(10), 0))
        tip(b, "Copy the console command.")
        hint(s, "To join: Multiplayer > LAN in the game, or that line in its console (~). Players spectate "
                "for now.").grid(row=4, column=0, columnspan=5, sticky="w", pady=(px(4), 0))

        o = section(t, "Server output", 3, columns=1)
        o.columnconfigure(0, weight=1)
        box = tk.Text(o, height=10, width=40, wrap="word", font="C3WMono", borderwidth=0, highlightthickness=1,
                      padx=px(8), pady=px(6), state="disabled")
        box.grid(row=0, column=0, sticky="ew", pady=(0, px(18)))
        theme.recolor(box)
        ui["output"] = box
        self._pc_update_join(gid)
        for name in ("port",):
            v[name].trace_add("write", lambda *a, g=gid: self._pc_update_join(g))

    def _pc_join_command(self, gid: str) -> str:
        port = pc_games.parse_int(self.pc[gid]["port"].get(), *pc_games.PORT_RANGE) or pc_games.DEFAULT_PORT
        return f"connect {pc_games.local_address() or '127.0.0.1'}:{port}"

    def _pc_update_join(self, gid: str) -> None:
        lbl = self.pc_ui.get(gid, {}).get("join")
        if lbl is not None:
            lbl.configure(text=self._pc_join_command(gid))

    def _pc_server_settings(self, gid: str) -> pc_games.ServerSettings:
        v = self.pc[gid]
        return pc_games.ServerSettings(map=v["map"].get(), port=v["port"].get(), hostname=v["hostname"].get(),
                                       max_clients=v["max_clients"].get())

    def _pc_server_running(self, gid: str) -> bool:
        server = getattr(self, "pc_servers", {}).get(gid)
        return bool(server and server.running)

    def _pc_server_reason(self, gid: str, inputs_only: bool = False) -> str:
        """Why the server can't start ("" when it can, or while it runs)."""
        if not inputs_only and self._pc_server_running(gid):
            return ""
        problems = self._pc_server_settings(gid).problems()
        if problems:
            return problems[0]
        port = pc_games.parse_int(self.pc[gid]["port"].get(), *pc_games.PORT_RANGE)
        for other in self.pc:
            if other != gid and self._pc_server_running(other) and \
                    pc_games.parse_int(self.pc[other]["port"].get(), *pc_games.PORT_RANGE) == port:
                return f"Port {port} is in use by the {pc_games.PC_GAMES[other].name} server."
        if not inputs_only and self.worker and self.worker.is_alive():
            return "Busy: wait for the job running now, or cancel it."
        return ""

    def _pc_server_toggle(self, gid: str) -> None:
        if self._pc_server_running(gid):
            self._pc_stop_server(gid)
        else:
            self._pc_start_server(gid)

    def _pc_start_server(self, gid: str) -> None:
        why = self._pc_server_reason(gid)
        if why:
            self.v_status.set(why)
            return
        pg = pc_games.PC_GAMES[gid]
        settings = self._pc_server_settings(gid)
        install = self.pc[gid]["install"].get()
        tool = self._pc_tool_maker("iw-server")
        self._save_quietly()

        def work(runner: common.Runner) -> str:
            exe = tool(runner)
            # The game's program, for its tables (only read, never run).
            game_exe = pc_games.game_executable(install, pg)
            cmd = pc_games.server_command(exe, pg, settings, game_exe)
            env = pc_games.server_env(None, install, game_exe)
            self.q.put(("call", lambda: self._pc_launch_server(gid, cmd, env, exe.parent)))
            return ""
        self._start_task("Starting the LAN server", work, None)

    def _pc_launch_server(self, gid: str, cmd: List[str], env: dict, cwd: Path) -> None:
        self._pc_output(gid, None)
        self.log.info("Starting: " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
        try:
            self.pc_servers[gid].start(cmd, env=env, cwd=cwd)
        except SetupError as e:
            self._info("Server not started", str(e), kind="error")
            return
        port = cmd[cmd.index("--port") + 1]
        self._pc_set_state(gid, f"Running on UDP port {port}.", "Good.TLabel")
        self.v_status.set(f"{pc_games.PC_GAMES[gid].name} LAN server started.")
        self._update_actions()

    def _pc_stop_server(self, gid: str) -> None:
        server = self.pc_servers[gid]
        self._pc_set_state(gid, "Stopping...", "Hint.TLabel")
        threading.Thread(target=server.stop, daemon=True).start()

    def _pc_server_line(self, gid: str, line: str) -> None:
        self.log.output(f"[{games.game(gid).short} server] {line}")
        self._pc_output(gid, line)

    def _pc_server_exited(self, gid: str, code: int) -> None:
        stopped = self.pc_servers[gid].stopping
        name = pc_games.PC_GAMES[gid].name
        if stopped:
            self._pc_set_state(gid, "Stopped.", "Hint.TLabel")
            self.v_status.set(f"{name} LAN server stopped.")
        else:
            self._pc_set_state(gid, f"Stopped by itself (exit code {code}): see the output.", "Warn.TLabel")
            self.v_status.set(f"The {name} LAN server stopped (exit code {code}).")
            self.log.warn(f"The {name} LAN server stopped (exit code {code}).")
        self._update_actions()

    def _pc_set_state(self, gid: str, text: str, style: str) -> None:
        lbl = self.pc_ui.get(gid, {}).get("server_state")
        if lbl is not None:
            lbl.configure(text=text, style=style)

    def _pc_output(self, gid: str, line: Optional[str]) -> None:
        """Adds a line to the page's server output (None empties it)."""
        box = self.pc_ui.get(gid, {}).get("output")
        if box is None:
            return
        box.configure(state="normal")
        if line is None:
            box.delete("1.0", "end")
        else:
            box.insert("end", line + "\n")
            if int(box.index("end-1c").split(".")[0]) > 500:
                box.delete("1.0", "100.0")
            box.see("end")
        box.configure(state="disabled")

    def _pc_stop_all_servers(self) -> None:
        """When the tool quits: every LAN server it started ends with it."""
        for server in self.pc_servers.values():
            server.stop(wait=False)

    def _pc_servers_running(self) -> List[str]:
        return [pc_games.PC_GAMES[g].name for g in self.pc if self._pc_server_running(g)]

    def _theme_games(self, c: Dict[str, str]) -> None:
        """What styles can't reach on these pages: the output boxes."""
        for ui in getattr(self, "pc_ui", {}).values():
            if "output" in ui:
                theme.recolor(ui["output"], c)
        self._color_tabs(c)

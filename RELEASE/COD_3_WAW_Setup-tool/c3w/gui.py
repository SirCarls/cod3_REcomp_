"""Tkinter front end: a sidebar with the pages Set up, Play, Textures, Tools,
Log and About (and Mods & maps when waw.MODS_ENABLED), and a bar at the
bottom with Build, Cancel, progress and status that stays on every page.

All long-running work happens on worker threads; they talk to the UI only
through a queue that the Tk main loop drains.

Where things are: this file has the window, the sidebar, the bottom bar,
building, settings and the Set up page; gui_pages.py has Play, Textures,
Mods & maps, Tools and About; gui_log.py the Log page. Colors and styles:
theme.py. Shared building blocks (scrolling, tooltips, dialogs): widgets.py.
"""
from __future__ import annotations

import dataclasses
import hashlib
import os
import queue
import subprocess
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
import webbrowser
from pathlib import Path
from tkinter import colorchooser, filedialog, ttk
from typing import Callable, Dict, List, Optional

from . import common, inputs, map_packs, theme, upscale, waw, widgets
from .common import IS_LINUX, IS_WINDOWS, Log, SetupError
from .gui_log import LogPage
from .gui_pages import Pages
from .pipeline import KNOWN_XEX, TOOL_VERSION, BuildOptions, Pipeline, fidelityfx_available
from .widgets import ScrollFrame, px, section, hint, tip  # noqa: F401 - ScrollFrame/px kept importable from gui
from .xiso import XisoImage

SETTINGS_FILE = common.config_dir() / "settings.json"
PAD = {"padx": 6, "pady": 3}
DISC_TYPES = [("Disc images", "*.iso *.ISO")]
PATCH_TYPES = [("Patch downloads", "*.zip *.ZIP *.exe *.EXE")]
# Where earlier versions put the work folder by default; such a saved setting
# is moved to the new default (the project's own work/ folder).
_USER = os.environ.get("USER") or os.environ.get("LOGNAME") or "user"
OLD_WORK_DEFAULTS = {common.cache_dir() / "work", Path("/var/tmp") / f"{common.APP_ID}-{_USER}"}
# The window's own settings, saved with the others but not part of a profile.
UI_KEYS = ("ui_theme", "ui_accent", "ui_page", "ui_geometry", "ui_zoomed")
# Pages in the sidebar: (key, label). Ctrl+1... picks them in this order.
PAGES = [("setup", "Set up"), ("play", "Play"), ("textures", "Textures"), ("maps", "Mods & maps"),
         ("tools", "Tools"), ("log", "Log"), ("about", "About")]
SIDEBAR_W, SIDEBAR_COMPACT_W = 200, 116
COMPACT_BELOW = 760   # window width (at 96 DPI) under which the sidebar gets narrow


def desktop_scale(root: tk.Tk) -> float:
    """How much bigger than 96 DPI the desktop draws text: Windows' display
    scaling, or on Linux the Xft.dpi the desktop sets (GNOME, KDE, Xfce all
    do; Tk itself doesn't read it), else GDK_SCALE, else the X server's DPI."""
    if not IS_WINDOWS:
        try:
            out = subprocess.run(["xrdb", "-query"], capture_output=True, text=True, timeout=3).stdout
            for line in out.splitlines():
                if line.startswith("Xft.dpi:"):
                    return max(1.0, min(4.0, float(line.split(":", 1)[1]) / 96.0))
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
        try:
            gdk = float(os.environ.get("GDK_SCALE", "0")) * float(os.environ.get("GDK_DPI_SCALE", "1"))
            if gdk > 0:
                return max(1.0, min(4.0, gdk))
        except ValueError:
            pass
    try:
        return max(1.0, min(4.0, root.winfo_fpixels("1i") / 96.0))
    except tk.TclError:
        return 1.0


def apply_desktop_scale(root: tk.Tk) -> float:
    """Makes the whole window follow the desktop's scaling: Tk's own fonts on
    Linux are sized in pixels, which no DPI setting changes, so they are
    scaled here, and Tk's point-to-pixel factor is set to match."""
    scale = desktop_scale(root)
    if scale < 1.15:
        scale = 1.0    # a monitor that reports 100 DPI or so: leave it as Tk draws it
    widgets.set_scale(scale)
    if IS_WINDOWS or scale == 1.0:
        return scale   # Windows: Tk already sizes its point-sized fonts for the DPI
    root.tk.call("tk", "scaling", scale * 96.0 / 72.0)
    for name in tkfont.names(root):
        f = tkfont.nametofont(name, root=root)
        size = int(f.cget("size"))
        if size < 0:   # pixels
            f.configure(size=-max(1, round(-size * scale)))
    return scale


def format_duration(seconds: float) -> str:
    """'45 s', '12 min 5 s', '1 h 4 min'."""
    s = int(round(max(0.0, seconds)))
    if s < 60:
        return f"{s} s"
    if s < 3600:
        return f"{s // 60} min {s % 60} s"
    return f"{s // 3600} h {(s % 3600) // 60} min"


class App(Pages, LogPage):
    def __init__(self, root: tk.Tk, launcher: bool):
        self.root = root
        self.launcher = launcher
        self.q: "queue.Queue[tuple]" = queue.Queue()
        self.cancel = threading.Event()
        self.worker: Optional[threading.Thread] = None
        self.log = Log(sinks=[lambda k, t, f: self.q.put(("log", k, t, f))])
        self._in_poll = False
        self._build_started = 0.0
        self.config_dir = SETTINGS_FILE.parent     # settings.json and profiles/
        s = common.read_json(SETTINGS_FILE)
        # A settings file copied from another system (Linux to Windows or
        # back) has paths that mean something else here: use the defaults.
        from .profiles import PATH_KEYS
        s, foreign = common.drop_foreign_paths(s, PATH_KEYS)
        if foreign:
            self.log.warn(f"{SETTINGS_FILE} was saved on another kind of system; its folders were "
                          f"not used here ({', '.join(foreign)}). Pick them again, or keep the defaults.")
        self._saved_ui = {k: s.get(k) for k in UI_KEYS}

        # Launched from <output>/launcher: that output folder is the default.
        here = Path(__file__).resolve().parent.parent
        default_out = here.parent if (here.parent / "cod3_waw.json").exists() else common.default_output_dir()
        # Discs and downloads put in the input folder are found by themselves.
        self.input_dir = common.default_input_dir()
        in_input = inputs.scan(self.input_dir, KNOWN_XEX)

        self.v_iso = tk.StringVar(value=s.get("cod3_source") or (str(in_input.cod3_iso) if in_input.cod3_iso else ""))
        if "waw_steam" not in s and s.get("waw_source"):
            # Settings from tool 1.x had one "waw_source" for every kind.
            old = s["waw_source"]
            s[{waw.STEAM: "waw_steam", waw.XBOX360: "waw_xbox360"}.get(waw.guess_kind(Path(old)), "waw_pc_disc")] = old
            s.setdefault("waw_steam", "")
        steam_guess = s.get("waw_steam")
        if steam_guess is None:  # first start: look for World at War in Steam
            found = waw.find_steam_waw()
            steam_guess = str(found) if found else ""
        self.v_steam = tk.StringVar(value=steam_guess)
        self.v_xbox = tk.StringVar(value=s.get("waw_xbox360") or
                                   (str(in_input.waw_xbox360) if in_input.waw_xbox360 else ""))
        self.v_pc = tk.StringVar(value=s.get("waw_pc_disc", ""))
        self.v_alt = tk.StringVar(value=s.get("waw_alternative", waw.XBOX360))
        # World at War is optional (Call of Duty 3 doesn't need it). The box is
        # ticked on first start only when a copy was found.
        use_waw = s.get("use_waw")
        if use_waw is None:
            use_waw = bool(steam_guess or self.v_xbox.get() or self.v_pc.get())
        self.v_use_waw = tk.BooleanVar(value=use_waw)
        packs = s.get("waw_map_packs")
        if packs is None:  # first start (or settings from before map packs): look for downloaded patches
            packs = [s["waw_map_pack"]] if s.get("waw_map_pack") else [str(p) for p in map_packs.find_downloads()]
            packs += [str(p) for p in in_input.map_packs if str(p) not in packs]
        self.packs: List[str] = [p for p in packs if p]
        self.v_pack = tk.StringVar()
        self.v_out = tk.StringVar(value=s.get("output_dir") if s.get("output_dir") and not launcher
                                  else str(default_out))
        work = s.get("work_dir") or ""
        if not work or Path(work) in OLD_WORK_DEFAULTS:
            work = str(common.default_work_dir())   # the work folder now defaults to the project folder
        self.v_work = tk.StringVar(value=work)
        self.v_jobs = tk.IntVar(value=int(s.get("jobs", common.recommended_jobs())))
        self.v_recomp = tk.BooleanVar(value=s.get("build_recomp", True))
        self.v_appimage = tk.BooleanVar(value=s.get("appimage", True))
        self.v_install = tk.BooleanVar(value=s.get("install_deps", True))
        self.v_ffx = tk.BooleanVar(value=s.get("fidelityfx", True))
        self.v_upscale = tk.BooleanVar(value=s.get("upscale_textures", False))
        self.v_upscale_model = tk.StringVar()
        self.v_upscale_noise = tk.StringVar()
        self.v_upscale_scale = tk.StringVar()
        self._set_upscale_vars(s.get("upscale_model", upscale.DEFAULT_MODEL),
                               s.get("upscale_noise", upscale.DEFAULT_NOISE),
                               s.get("upscale_scale", upscale.DEFAULT_SCALE))
        # Which versions to build (either system can build both). Settings
        # from before these boxes: this system's own, plus Windows when the
        # old "also make Windows .exe files" box was ticked on Linux.
        self.v_linux = tk.BooleanVar(value=s.get("build_linux", IS_LINUX))
        self.v_windows = tk.BooleanVar(value=s.get("build_windows", IS_WINDOWS or bool(s.get("windows_exe"))))
        self.v_cleanup = tk.BooleanVar(value=s.get("cleanup", True))
        self.v_keep_sdk = tk.BooleanVar(value=s.get("keep_sdk", False))
        self.v_iso_status = tk.StringVar()
        self.v_waw_status = tk.StringVar()
        self.v_status = tk.StringVar(value="Ready.")
        self.v_elapsed = tk.StringVar()
        self.v_build_reason = tk.StringVar()
        self.v_progress = tk.DoubleVar(value=0)
        self.v_cod3_windowed = tk.BooleanVar(value=s.get("cod3_windowed", False))
        self.v_cod3_args = tk.StringVar(value=s.get("cod3_args", ""))
        # The window's look (theme.py): the game's F2 themes and an accent.
        theme_id = s.get("ui_theme") if s.get("ui_theme") in theme.THEMES else theme.DEFAULT_THEME
        self.accent = theme.normalize_accent(s.get("ui_accent"))
        self.v_theme = tk.StringVar(value=theme.theme_label(theme_id))

        # The mouse wheel scrolls the page, never the value of a number box or
        # list under the pointer (that changed "Build threads" while scrolling).
        for cls in ("TSpinbox", "TCombobox"):
            for ev in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
                root.bind_class(cls, ev, lambda e: None)
        root.title(common.APP_NAME)
        # Small windows (and small screens) work: the pages scroll.
        root.minsize(px(420), px(320))
        theme.apply(root, theme_id, self.accent, px)
        theme.listeners.append(self._on_theme)

        self._build_shell()
        self.page_order = [k for k, _ in PAGES if k != "maps" or waw.MODS_ENABLED]
        self.pages: Dict[str, dict] = {}
        builders = {"setup": self._page_setup, "play": self._page_play, "textures": self._page_textures,
                    "maps": self._page_maps, "tools": self._page_tools, "log": self._page_log,
                    "about": self._page_about}
        labels = dict(PAGES)
        for i, key in enumerate(self.page_order):
            self._add_page(key, labels[key], builders[key], i)
        self._initial_size()

        self.v_iso.trace_add("write", lambda *a: (self._validate_iso(), self._update_build_state()))
        self.v_out.trace_add("write", lambda *a: self._on_output_changed())
        for v in (self.v_steam, self.v_xbox, self.v_pc, self.v_alt):
            v.trace_add("write", lambda *a: self._validate_waw())
        self._validate_iso()
        self._set_packs(self.packs)
        self._update_build_state()
        self._bind_keys()
        page = self._saved_ui.get("ui_page")
        self.show_page("play" if launcher else page if page in self.pages else "setup")
        root.protocol("WM_DELETE_WINDOW", self._on_close)
        root.after(100, self._poll)
        root.after(50, lambda: widgets.title_bar_dark(root, not theme.current["light"]))

    # ------------------------------------------------------------------ window
    def _build_shell(self) -> None:
        """Sidebar | divider | (page, banner, bottom bar)."""
        root = self.root
        shell = self.shell = ttk.Frame(root)
        shell.pack(fill="both", expand=True)
        shell.rowconfigure(0, weight=1)
        shell.columnconfigure(2, weight=1)

        sb = self.sidebar = ttk.Frame(shell, style="Sidebar.TFrame", width=px(SIDEBAR_W))
        sb.grid(row=0, column=0, sticky="ns")
        sb.pack_propagate(False)
        divider = tk.Frame(shell, width=1)
        divider.c3w_role = "divider"
        divider.grid(row=0, column=1, sticky="ns")
        theme.recolor(divider)

        logo = ttk.Frame(sb, style="Sidebar.TFrame", padding=(px(18), px(20), px(10), px(18)))
        logo.pack(side="top", fill="x")
        self.lbl_logo = ttk.Label(logo, text="CALL OF DUTY 3", style="Logo.TLabel")
        self.lbl_logo.pack(anchor="w")
        self.lbl_logo_sub = ttk.Label(logo, text="Setup tool", style="SidebarDim.TLabel")
        self.lbl_logo_sub.pack(anchor="w")
        self.nav = ttk.Frame(sb, style="Sidebar.TFrame")
        self.nav.pack(side="top", fill="x")

        # The look, at the sidebar's foot (also on Tools > Appearance).
        foot = self.sidebar_foot = ttk.Frame(sb, style="Sidebar.TFrame", padding=(px(16), px(8), px(12), px(14)))
        foot.pack(side="bottom", fill="x")
        ttk.Label(foot, text="Theme", style="SidebarDim.TLabel").pack(anchor="w")
        cb = ttk.Combobox(foot, textvariable=self.v_theme, state="readonly", width=16,
                          values=[theme.THEMES[t].label for t in theme.THEME_ORDER])
        cb.pack(fill="x", pady=(px(2), px(6)))
        cb.bind("<<ComboboxSelected>>", lambda e: self._set_theme(theme.theme_id_for_label(self.v_theme.get())))
        tip(cb, lambda: theme.THEMES[theme.theme_id_for_label(self.v_theme.get())].description +
            " The same themes as the game's F2 window.")
        arow = ttk.Frame(foot, style="Sidebar.TFrame")
        arow.pack(fill="x")
        self.swatches: List[tk.Frame] = []
        sw = tk.Frame(arow, width=px(14), height=px(14), cursor="hand2")
        sw.c3w_role = "accent"
        sw.pack(side="left", padx=(0, px(6)))
        sw.bind("<Button-1>", lambda e: self._pick_accent())
        theme.recolor(sw)
        self.swatches.append(sw)
        tip(ttk.Button(arow, text="Accent color...", style="SidebarLink.TButton", cursor="hand2",
                       command=self._pick_accent), "Pick the highlight color.").pack(side="left")
        self.lbl_version = ttk.Label(foot, text=f"Version {TOOL_VERSION}", style="SidebarDim.TLabel")
        self.lbl_version.pack(anchor="w", pady=(px(10), 0))

        main = self.main = ttk.Frame(shell)
        main.grid(row=0, column=2, sticky="nsew")
        main.rowconfigure(0, weight=1)
        main.columnconfigure(0, weight=1)
        self.content = ttk.Frame(main)
        self.content.grid(row=0, column=0, sticky="nsew")
        self.content.rowconfigure(0, weight=1)
        self.content.columnconfigure(0, weight=1)
        self._build_banner(main)
        self._build_bottom_bar(main)
        self._compact = False
        root.bind("<Configure>", self._on_root_resize, add="+")

    def _add_page(self, key: str, label: str, builder: Callable, index: int) -> None:
        """A page (it scrolls when the window is too small for it) and its
        entry in the sidebar: a marker stripe and a button."""
        frame = ScrollFrame(self.content)
        row = ttk.Frame(self.nav, style="Sidebar.TFrame")
        row.pack(fill="x")
        stripe = tk.Frame(row, width=px(4))
        stripe.c3w_role = "stripe"
        stripe.pack(side="left", fill="y")
        btn = ttk.Button(row, text=label, style="Nav.TButton", command=lambda k=key: self.show_page(k))
        btn.pack(side="left", fill="x", expand=True)
        tip(btn, f"{label}  (Ctrl+{index + 1})" if index < 9 else label)
        self.pages[key] = {"frame": frame, "button": btn, "stripe": stripe, "label": label,
                           "scroll": frame, "on_show": None}
        builder(frame.inner)
        self._color_stripe(key)

    def show_page(self, key: str) -> None:
        if key not in self.pages:
            return
        current = getattr(self, "page", None)
        if current and current != key:
            self.pages[current]["frame"].grid_remove()
        self.page = key
        p = self.pages[key]
        p["frame"].grid(row=0, column=0, sticky="nsew")
        for k in self.pages:
            self.pages[k]["button"].configure(style="NavSelected.TButton" if k == key else "Nav.TButton")
            self._color_stripe(k)
        if p.get("on_show"):
            p["on_show"]()

    def _color_stripe(self, key: str) -> None:
        c = theme.current
        selected = getattr(self, "page", None) == key
        try:
            self.pages[key]["stripe"].configure(background=c["accent"] if selected else c["sidebar"])
        except (KeyError, tk.TclError):
            pass

    def _on_root_resize(self, event) -> None:
        if event.widget is not self.root:
            return
        compact = event.width < px(COMPACT_BELOW)
        if compact == self._compact:
            return
        self._compact = compact
        self.sidebar.configure(width=px(SIDEBAR_COMPACT_W if compact else SIDEBAR_W))
        self.lbl_logo.configure(text="COD3" if compact else "CALL OF DUTY 3")
        if compact:
            self.sidebar_foot.pack_forget()
        else:
            self.sidebar_foot.pack(side="bottom", fill="x", after=self.nav)

    def _initial_size(self) -> None:
        """Open at the size the window had last time; the first time, big
        enough for the Set up page without scrolling. Never bigger than the
        screen (then the scrollbars take over)."""
        self.root.update_idletasks()
        sw, sh = self.root.winfo_screenwidth(), self.root.winfo_screenheight()
        saved = str(self._saved_ui.get("ui_geometry") or "")
        try:
            w, h = (int(v) for v in saved.lower().split("x", 1))
        except ValueError:
            w = h = 0
        if w < px(420) or h < px(320):
            pages = [p["scroll"] for p in self.pages.values()]
            setup = self.pages["setup"]["scroll"]
            w = (px(SIDEBAR_W) + 1 + max(p.inner.winfo_reqwidth() for p in pages) +
                 setup.vbar.winfo_reqwidth() + px(8))
            h = setup.inner.winfo_reqheight() + self.bottom.winfo_reqheight() + px(8)
        self.root.geometry(f"{min(w, int(sw * 0.95))}x{min(h, int(sh * 0.88))}")
        if self._saved_ui.get("ui_zoomed"):
            try:
                if IS_WINDOWS:
                    self.root.state("zoomed")
                else:
                    self.root.attributes("-zoomed", True)
            except tk.TclError:
                pass

    def _bind_keys(self) -> None:
        """Ctrl+B builds, Ctrl+L shows the log, Ctrl+1... picks a page, F5
        checks again. Bound to the main window only, so a dialog's keys stay
        its own."""
        r = self.root
        for seq in ("<Control-b>", "<Control-B>"):
            r.bind(seq, lambda e: self._shortcut_build())
        for seq in ("<Control-l>", "<Control-L>"):
            r.bind(seq, lambda e: self.show_page("log"))
        r.bind("<F5>", lambda e: self._refresh_checks())
        for i, key in enumerate(self.page_order[:9]):
            r.bind(f"<Control-Key-{i + 1}>", lambda e, k=key: self.show_page(k))

    def _shortcut_build(self) -> None:
        if str(self.btn_build.cget("state")) == "disabled":
            if self.v_build_reason.get():
                self.v_status.set(self.v_build_reason.get())
            return
        self._start_build()

    def _refresh_checks(self) -> None:
        """F5: checks the disc, World at War, the build and the current page
        again."""
        self._validate_iso()
        self._validate_waw()
        self._update_build_state()
        self._refresh_page(self.page)
        self.v_status.set("Checked again.")

    # ------------------------------------------------------------------ theme
    def _set_theme(self, theme_id: str) -> None:
        self.v_theme.set(theme.theme_label(theme_id))
        theme.apply(self.root, theme_id, self.accent, px)
        self._save_quietly()

    def _pick_accent(self) -> None:
        start = theme.current["accent"]
        try:
            picked = colorchooser.askcolor(color=start, title="Accent color", parent=self.root)
        except tk.TclError:
            return
        if picked and picked[1]:
            self.accent = theme.normalize_accent(picked[1])
            self._set_theme(theme.theme_id_for_label(self.v_theme.get()))

    def _reset_accent(self) -> None:
        self.accent = ""
        self._set_theme(theme.theme_id_for_label(self.v_theme.get()))

    def _on_theme(self, c: Dict[str, str]) -> None:
        """What styles can't reach: the page markers, swatches, the log's colors."""
        for key in getattr(self, "pages", {}):
            self._color_stripe(key)
        for sw in getattr(self, "swatches", []):
            try:
                sw.configure(background=c["accent"])
            except tk.TclError:
                pass
        if hasattr(self, "banner_stripe"):
            self.banner_stripe.configure(background=c.get(self._banner_kind, c["accent"]))
        widgets.title_bar_dark(self.root, not c["light"])
        self._theme_log(c)
        self._theme_pages(c)

    # ------------------------------------------------------------------ bottom bar and banner
    def _build_bottom_bar(self, main) -> None:
        """Build, Cancel, the reason Build is off, progress and status: in
        view on every page, however small the window is."""
        bar = self.bottom = ttk.Frame(main)
        bar.grid(row=2, column=0, sticky="ew")
        line = tk.Frame(bar, height=1)
        line.c3w_role = "divider"
        line.pack(fill="x")
        theme.recolor(line)
        inner = ttk.Frame(bar, padding=(px(24), px(10), px(24), px(8)))
        inner.pack(fill="x")
        inner.columnconfigure(2, weight=1)
        self.btn_build = ttk.Button(inner, text="Build", style="Accent.TButton", command=self._start_build)
        self.btn_build.grid(row=0, column=0, sticky="w")
        tip(self.btn_build, "Rip the games and build what's ticked on Set up (Ctrl+B). Nothing is downloaded before "
            "you approve the list.", reason=self.v_build_reason.get)
        self.btn_cancel = ttk.Button(inner, text="Cancel", command=self._cancel, state="disabled")
        self.btn_cancel.grid(row=0, column=1, sticky="w", padx=(px(6), 0))
        tip(self.btn_cancel, "Stop the build. Press Build later to continue where it stopped.")
        b = ttk.Button(inner, text="Open output folder", command=lambda: self._open(self.v_out.get()))
        b.grid(row=0, column=3, sticky="e")
        tip(b, "The built game, the ripped files, saves and logs.")
        # Why Build is off (only shown then); a click goes to Set up.
        self.lbl_reason = ttk.Label(inner, textvariable=self.v_build_reason, style="Warn.TLabel", cursor="hand2",
                                    justify="left")
        self.lbl_reason.grid(row=1, column=0, columnspan=4, sticky="w", pady=(px(6), 0))
        self.lbl_reason.bind("<Button-1>", lambda e: self.show_page("setup"))
        tip(self.lbl_reason, "Click to go to Set up.")
        ttk.Progressbar(inner, variable=self.v_progress, maximum=1.0).grid(
            row=2, column=0, columnspan=4, sticky="ew", pady=(px(8), px(4)))
        # Status (cut at the window's edge, never pushing the buttons) and the time taken.
        ttk.Label(inner, textvariable=self.v_status, width=1).grid(row=3, column=0, columnspan=3, sticky="ew")
        ttk.Label(inner, textvariable=self.v_elapsed, style="Hint.TLabel").grid(row=3, column=3, sticky="e")
        inner.bind("<Configure>", lambda e: self.lbl_reason.configure(wraplength=max(px(200), e.width - px(52))),
                   add="+")

    def _build_banner(self, main) -> None:
        """The "Done" note at the end of a build (no desktop notification):
        how it went and how long it took, until dismissed."""
        self._banner_kind = "good"
        b = self.banner = ttk.Frame(main, style="Surface.TFrame")
        self.banner_stripe = tk.Frame(b, width=px(5))
        self.banner_stripe.c3w_role = "stripe"
        self.banner_stripe.pack(side="left", fill="y")
        body = ttk.Frame(b, style="Surface.TFrame", padding=(px(16), px(10), px(16), px(10)))
        body.pack(side="left", fill="both", expand=True)
        body.columnconfigure(0, weight=1)
        self.banner_title = ttk.Label(body, style="SurfaceGood.TLabel")
        self.banner_title.grid(row=0, column=0, sticky="w")
        self.banner_text = ttk.Label(body, style="Surface.TLabel", justify="left", wraplength=px(560))
        self.banner_text.grid(row=1, column=0, sticky="w", pady=(px(2), 0))
        self.banner_buttons = ttk.Frame(body, style="Surface.TFrame")
        self.banner_buttons.grid(row=0, column=1, rowspan=2, sticky="e", padx=(px(12), 0))
        # The text wraps in the room the buttons leave.
        body.bind("<Configure>", lambda e: self.banner_text.configure(
            wraplength=max(px(160), e.width - self.banner_buttons.winfo_reqwidth() - px(44))), add="+")

    def _show_banner(self, kind: str, title: str, text: str, actions: List[tuple]) -> None:
        """kind: good, warn or bad. actions: (label, command)."""
        self._banner_kind = kind
        c = theme.current
        self.banner_stripe.configure(background=c[kind])
        self.banner_title.configure(text=title, style={"good": "SurfaceGood.TLabel", "warn": "SurfaceWarn.TLabel"}
                                    .get(kind, "SurfaceBad.TLabel"))
        self.banner_text.configure(text=text)
        for w in self.banner_buttons.winfo_children():
            w.destroy()
        for i, (label, cmd) in enumerate(actions + [("Dismiss", None)]):
            ttk.Button(self.banner_buttons, text=label, style="Accent.TButton" if i == 0 and cmd else "TButton",
                       command=lambda c=cmd: (self._hide_banner(), c and c())).pack(side="left", padx=(px(6), 0))
        self.banner.grid(row=1, column=0, sticky="ew")

    def _hide_banner(self) -> None:
        self.banner.grid_remove()

    def _update_build_state(self) -> None:
        """Build is on only when it can start; otherwise the reason is under
        it (and in its tooltip)."""
        if not hasattr(self, "btn_build"):
            return
        running = bool(self.worker and self.worker.is_alive())
        reason = "" if running else self._blocker(self._options(), "Build")
        self.v_build_reason.set(reason)
        self.btn_build.configure(state="disabled" if running or reason else "normal")
        self.btn_cancel.configure(state="normal" if running else "disabled")
        if reason:
            self.lbl_reason.grid()
        else:
            self.lbl_reason.grid_remove()
        self._update_page_states()

    def _blocker(self, opts: BuildOptions, what: str) -> str:
        """Why a build with these options can't start ("" when it can)."""
        src = opts.cod3_source
        if not src:
            return f"{what} needs your Call of Duty 3 disc: choose the .iso (or its extracted folder) on Set up."
        if not Path(src).exists():
            return f"{what} needs your Call of Duty 3 disc: {Path(src).name} isn't there any more (Set up)."
        if not opts.output_dir:
            return f"{what} needs an output folder: choose one on Set up."
        return ""

    def _options_with(self, overrides: Optional[dict]) -> BuildOptions:
        """The form's options, changed by `overrides` (BuildOptions fields)."""
        opts = self._options()
        if overrides:
            known = {f.name for f in dataclasses.fields(BuildOptions)}
            opts = dataclasses.replace(opts, **{k: v for k, v in overrides.items() if k in known})
        return opts

    def _on_output_changed(self) -> None:
        self._update_build_state()
        self._refresh_maps()
        self._output_changed()

    # ------------------------------------------------------------------ helpers
    def _row(self, parent, r: int, label: str, var: tk.StringVar, buttons: List[tuple],
             widgets_out: Optional[list] = None, tooltip: str = "") -> ttk.Entry:
        lbl = ttk.Label(parent, text=label)
        lbl.grid(row=r, column=0, sticky="w", padx=(0, px(10)), pady=px(3))
        entry = ttk.Entry(parent, textvariable=var)
        entry.grid(row=r, column=1, sticky="ew", pady=px(3))
        if tooltip:
            tip(entry, tooltip)
        f = ttk.Frame(parent)
        f.grid(row=r, column=2, sticky="e", padx=(px(6), 0))
        made = [entry]
        for item in buttons:
            text, cmd = item[0], item[1]
            b = ttk.Button(f, text=text, command=cmd)
            b.pack(side="left", padx=(px(4), 0))
            if len(item) > 2 and item[2]:
                tip(b, item[2])
            made.append(b)
        if widgets_out is not None:
            widgets_out += made
        return entry

    def _pick_file(self, var: tk.StringVar, title: str, types: List[tuple]) -> None:
        start = Path(var.get()).parent if var.get() else Path.home()
        p = filedialog.askopenfilename(title=title, initialdir=str(start) if start.exists() else None,
                                       filetypes=types + [("All files", "*")], parent=self.root)
        if p:
            var.set(str(Path(p)))

    def _pick_dir(self, var: tk.StringVar, title: str) -> None:
        start = Path(var.get()) if var.get() else Path.home()
        p = filedialog.askdirectory(title=title, initialdir=str(start) if start.exists() else None, parent=self.root)
        if p:
            var.set(str(Path(p)))

    def _bg(self, func: Callable[[], None], busy_text: str = "") -> None:
        """Run a short task off the UI thread; errors become messages."""
        def wrapper():
            try:
                func()
            except SetupError as e:
                self.q.put(("error", str(e)))
            except Exception as e:  # pragma: no cover - unexpected
                self.q.put(("error", f"{type(e).__name__}: {e}"))
        if busy_text:
            self.v_status.set(busy_text)
        threading.Thread(target=wrapper, daemon=True).start()

    def _manifest(self) -> dict:
        return common.read_json(Path(self.v_out.get()) / "cod3_waw.json") if self.v_out.get() else {}

    def _info(self, title: str, text: str, kind: str = "info") -> None:
        widgets.message(self.root, title, text, kind=kind)

    def _bold_font(self) -> str:
        return "C3WBold"

    def _copy_text(self, text: str) -> None:
        self.root.clipboard_clear()
        self.root.clipboard_append(text)
        self.v_status.set("Copied.")

    def _open(self, path: str) -> None:
        if path and Path(path).exists():
            common.open_in_file_manager(Path(path))
        elif path:
            self.v_status.set(f"Not there yet: {path}")

    def _open_input(self) -> None:
        self.input_dir.mkdir(parents=True, exist_ok=True)
        self._open(str(self.input_dir))

    def _open_logs(self) -> None:
        out = self.v_out.get().strip()
        logs = Path(out) / "logs" if out else None
        if logs and logs.is_dir():
            self._open(str(logs))
        else:
            self._info("No logs yet", "No build logs yet.\nThey are kept in the logs folder of the output folder, "
                                      "after the first build.")

    # ------------------------------------------------------------------ Set up page
    def _page_setup(self, t) -> None:
        t.columnconfigure(0, weight=1)
        head = widgets.page_header(t, "Set up", "Make Call of Duty 3 from your own disc, then press Build. Your "
                                   "discs and game folders are only read: never changed, never deleted.", columns=1)
        b = ttk.Button(head, text="Open input folder", command=self._open_input)
        b.pack(side="right", anchor="n", padx=(px(12), 0))
        tip(b, "Put your disc images and downloads here: they're found by themselves.")

        d = section(t, "Your disc", 1, columns=1)
        self._grid_cols(d)
        self._row(d, 0, "Call of Duty 3 (Xbox 360)", self.v_iso, [
            ("ISO...", lambda: self._pick_file(self.v_iso, "Call of Duty 3 disc image", DISC_TYPES),
             "Pick the disc image (.iso)."),
            ("Folder...", lambda: self._pick_dir(self.v_iso, "Folder extracted from the CoD3 disc"),
             "Pick a folder you already extracted the disc to.")],
            tooltip="Your Call of Duty 3 (USA, Europe) Xbox 360 disc image.")
        self.lbl_iso_status = hint(d, var=self.v_iso_status)
        self.lbl_iso_status.grid(row=1, column=1, columnspan=2, sticky="w")
        hint(d, "Tip: put your disc images and downloads in the input folder and they're found by "
                "themselves.").grid(row=2, column=0, columnspan=3, sticky="w", pady=(px(6), 0))

        w = section(t, "What to build", 3, columns=1)
        self._grid_cols(w)
        ttk.Checkbutton(w, text="Build the playable Call of Duty 3 (recompiled; 30-120 minutes per version the "
                                "first time)", variable=self.v_recomp,
                        command=self._update_versions).grid(row=0, column=0, columnspan=3, sticky="w")
        vf = ttk.Frame(w)
        vf.grid(row=1, column=0, columnspan=3, sticky="w", padx=(px(26), 0), pady=(px(2), 0))
        ttk.Label(vf, text="Versions:").pack(side="left", padx=(0, px(8)))
        self.cb_linux = ttk.Checkbutton(vf, text="Linux (AppImage)", variable=self.v_linux)
        self.cb_linux.pack(side="left")
        self.cb_windows = ttk.Checkbutton(vf, text="Windows (.exe)", variable=self.v_windows)
        self.cb_windows.pack(side="left", padx=(px(14), 0))
        for cb in (self.cb_linux, self.cb_windows):
            tip(cb, reason=lambda: "" if self.v_recomp.get() else "Tick \"Build the playable Call of Duty 3\" first.")
        other = ("The Windows version is cross-compiled here with llvm-mingw (downloaded automatically)."
                 if IS_LINUX else
                 "The Linux version is built inside WSL 2 (Windows Subsystem for Linux), in a private Ubuntu "
                 "the tool sets up and removes again. WSL itself must be turned on once: in an administrator "
                 "PowerShell, `wsl --install --no-distribution`, then restart.")
        hint(w, other).grid(row=2, column=0, columnspan=3, sticky="w", padx=(px(26), 0))
        self._update_versions()
        self._row(w, 3, "Output folder", self.v_out, [
            ("Browse...", lambda: self._pick_dir(self.v_out, "Where everything goes"), "")],
            tooltip="Where the game, the ripped files, saves and logs go.")
        ttk.Checkbutton(w, text="Install anything that's missing automatically (recommended)",
                        variable=self.v_install).grid(row=4, column=0, columnspan=3, sticky="w", pady=(px(4), 0))

        ww = section(t, "World at War (optional)", 5, columns=1)
        self._grid_cols(ww)
        ttk.Checkbutton(ww, text="Also rip World at War (not needed to play Call of Duty 3)",
                        variable=self.v_use_waw, command=self._validate_waw).grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, px(4)))
        self.steam_widgets: list = []
        self._row(ww, 1, "Steam folder", self.v_steam, [
            ("Find", self._find_steam, "Look for World at War in your Steam libraries."),
            ("Browse...", lambda: self._pick_dir(self.v_steam, "World at War folder in your Steam library"), "")],
            widgets_out=self.steam_widgets)
        hint(ww, "Have it on Steam? That's all you need: when a Steam folder is set, it is the only World at "
                 "War source used.").grid(row=2, column=0, columnspan=3, sticky="w")
        ttk.Label(ww, text="Without Steam, use one of these:").grid(row=3, column=0, columnspan=3, sticky="w",
                                                                   pady=(px(10), px(2)))
        rb1 = ttk.Radiobutton(ww, text="Xbox 360 disc image", value=waw.XBOX360, variable=self.v_alt)
        rb1.grid(row=4, column=0, sticky="w", pady=px(3))
        e1 = ttk.Entry(ww, textvariable=self.v_xbox)
        e1.grid(row=4, column=1, sticky="ew", pady=px(3))
        bf1 = ttk.Frame(ww)
        bf1.grid(row=4, column=2, sticky="e", padx=(px(6), 0))
        b1 = ttk.Button(bf1, text="Browse...", command=lambda: self._pick_file(
            self.v_xbox, "World at War Xbox 360 disc image", DISC_TYPES))
        b1.pack(side="left", padx=(px(4), 0))
        rb2 = ttk.Radiobutton(ww, text="PC disc", value=waw.PC_DISC, variable=self.v_alt)
        rb2.grid(row=5, column=0, sticky="w", pady=px(3))
        e2 = ttk.Entry(ww, textvariable=self.v_pc)
        e2.grid(row=5, column=1, sticky="ew", pady=px(3))
        pf = ttk.Frame(ww)
        pf.grid(row=5, column=2, sticky="e", padx=(px(6), 0))
        b2 = ttk.Button(pf, text="Image...", command=lambda: self._pick_file(
            self.v_pc, "World at War PC disc image", DISC_TYPES))
        b2.pack(side="left", padx=(px(4), 0))
        b3 = ttk.Button(pf, text="Drive/folder...", command=lambda: self._pick_dir(
            self.v_pc, "The World at War PC disc (drive), or a folder installed from it"))
        b3.pack(side="left", padx=(px(4), 0))
        tip(b3, "The disc drive, or a folder installed from the PC disc.")
        ttk.Label(ww, text="Map packs").grid(row=6, column=0, sticky="nw", pady=(px(8), px(2)))
        ttk.Label(ww, textvariable=self.v_pack, wraplength=px(460), justify="left").grid(
            row=6, column=1, columnspan=2, sticky="w", pady=(px(8), px(2)))
        mf = ttk.Frame(ww)
        mf.grid(row=7, column=1, columnspan=2, sticky="w", pady=(0, px(4)))
        b4 = ttk.Button(mf, text="Add...", command=self._add_packs)
        b4.pack(side="left")
        tip(b4, "Add the official PC patch downloads (1.4, 1.5, 1.6).")
        b5 = ttk.Button(mf, text="Clear", command=lambda: self._set_packs([]))
        b5.pack(side="left", padx=(px(4), 0))
        b6 = ttk.Button(mf, text="Download pages", command=self._open_pack_pages)
        b6.pack(side="left", padx=(px(4), 0))
        tip(b6, "Open the patches' download pages in your browser.")
        hint(ww, "Optional. The discs don't have the map packs; the official PC patches 1.4, 1.5 and 1.6 add "
                 "them (Map Packs 1-3). Download them from the download pages and add them here, or put them in "
                 "the input folder. Their installers are never run.").grid(
            row=8, column=0, columnspan=3, sticky="w")
        self.alt_widgets = [rb1, e1, b1, rb2, e2, b2, b3, b4, b5, b6]
        # Off rows say why when the pointer rests on them.
        for wdg in self.steam_widgets:
            tip(wdg, reason=lambda: "" if self.v_use_waw.get() else "Tick \"Also rip World at War\" above first.")
        for wdg in self.alt_widgets:
            tip(wdg, reason=lambda: "Tick \"Also rip World at War\" above first." if not self.v_use_waw.get() else
                "Not used: a Steam folder is set, and then it is the only source." if self.v_steam.get().strip()
                else "")
        self.lbl_waw_status = hint(ww, var=self.v_waw_status, style="Accent.TLabel")
        self.lbl_waw_status.grid(row=9, column=0, columnspan=3, sticky="w", pady=(px(6), 0))

        # Rarely changed: under "More options" (closed by default).
        more = ttk.Frame(t)
        more.grid(row=7, column=0, sticky="w", padx=px(24), pady=(px(14), 0))
        self.btn_more = widgets.link(more, "Show more options", self._toggle_options)
        self.btn_more.pack(side="left")
        self.adv = section(t, "More options", 8, columns=1, top=8)
        self._grid_cols(self.adv)
        self._row(self.adv, 0, "Work folder (temporary)", self.v_work, [
            ("Browse...", lambda: self._pick_dir(self.v_work, "Temporary build folder"), "")],
            tooltip="Temporary build files; emptied after a build unless you keep the SDK.")
        ttk.Label(self.adv, text="Build threads").grid(row=1, column=0, sticky="w", pady=px(3))
        jf = ttk.Frame(self.adv)
        jf.grid(row=1, column=1, columnspan=2, sticky="w", pady=px(3))
        ttk.Spinbox(jf, from_=1, to=max(1, common.cpu_count()), textvariable=self.v_jobs, width=5).pack(side="left")
        ttk.Label(jf, text=f"{common.cpu_count()} CPU threads, {common.total_ram_gb():.0f} GB RAM "
                           f"(about 2 GB per build thread; {common.recommended_jobs()} recommended)",
                  style="Hint.TLabel").pack(side="left", padx=(px(10), 0))
        checks = [("Linux version as an AppImage (otherwise a plain bin/ folder)", self.v_appimage)]
        if fidelityfx_available():
            checks.append(("AMD FidelityFX: FSR / CAS output scaling in game", self.v_ffx))
        checks += [("Delete temporary files when done", self.v_cleanup),
                   ("Keep the built SDK for faster rebuilds (downloaded tools are always kept)", self.v_keep_sdk)]
        for i, (text, var) in enumerate(checks):
            ttk.Checkbutton(self.adv, text=text, variable=var).grid(row=2 + i, column=0, columnspan=3, sticky="w",
                                                                   pady=px(1))
        self._toggle_options(show=False)

        hint(t, "Before anything is downloaded or installed, Build lists what, from where and into which "
                "folder, and waits for your approval. Everything that can stays in this project folder "
                "(Tools > Where things go shows each place). You may be asked for your password for system "
                "packages. You can cancel at any time and press Build again later to continue where it "
                "stopped.").grid(row=10, column=0, sticky="w", padx=px(24), pady=(px(18), px(18)))

    def _grid_cols(self, frame) -> None:
        """Labels in a column of the same width on every page."""
        frame.columnconfigure(0, minsize=px(190))
        frame.columnconfigure(1, weight=1)

    def _update_versions(self) -> None:
        state = "normal" if self.v_recomp.get() else "disabled"
        for cb in (self.cb_linux, self.cb_windows):
            cb.configure(state=state)

    def _toggle_options(self, show: Optional[bool] = None) -> None:
        if show is None:
            show = not self.adv.winfo_ismapped()
        for w in (self.adv, self.adv.head):
            if show:
                w.grid()
            else:
                w.grid_remove()
        self.btn_more.configure(text="Hide more options" if show else "Show more options")
        if "setup" in getattr(self, "pages", {}):
            self.pages["setup"]["scroll"].relayout()

    def _set_packs(self, packs: List[str]) -> None:
        self.packs = [str(p) for p in map_packs.patch_order(packs)]
        names = [Path(p).name for p in self.packs]
        self.v_pack.set(", ".join(names) if names else "none")
        self._validate_waw()

    def _add_packs(self) -> None:
        start = Path(self.packs[-1]).parent if self.packs else Path.home() / "Downloads"
        chosen = filedialog.askopenfilenames(title="Downloaded World at War patches with map packs",
                                             initialdir=str(start) if start.exists() else None,
                                             filetypes=PATCH_TYPES + [("All files", "*")], parent=self.root)
        if chosen:
            self._set_packs(list(dict.fromkeys(self.packs + [str(Path(c)) for c in chosen])))

    def _open_pack_pages(self) -> None:
        have = {Path(p).name.lower() for p in self.packs}
        pages = [k.page for k in map_packs.KNOWN_PATCHES if k.archive.lower() not in have]
        for page in pages or [k.page for k in map_packs.KNOWN_PATCHES]:
            webbrowser.open(page)

    def _find_steam(self) -> None:
        found = waw.find_steam_waw()
        if found:
            self.v_steam.set(str(found))
        else:
            self._info("Not found", "World at War wasn't found in your Steam libraries.\n"
                                    "Use Browse to pick its folder, or use the Xbox 360 or PC disc.")

    def _validate_iso(self) -> None:
        p = self.v_iso.get().strip()
        if not p:
            self.v_iso_status.set("Choose the .iso, or a folder you already extracted it to.")
            self.lbl_iso_status.configure(style="Hint.TLabel") if hasattr(self, "lbl_iso_status") else None
            return

        def work():
            path = Path(p)
            ok = False
            try:
                if path.is_dir():
                    xex = path / "default.xex"
                    if not xex.exists():
                        msg = "No default.xex in this folder."
                    else:
                        ok = common.sha1_file(xex) in KNOWN_XEX
                        msg = KNOWN_XEX.get(common.sha1_file(xex), "Unknown CoD3 version (default.xex differs)")
                elif path.is_file():
                    with XisoImage(path) as img:
                        sha = hashlib.sha1(img.read("default.xex")).hexdigest()
                        ok = sha in KNOWN_XEX
                        msg = KNOWN_XEX.get(sha, "Unknown game / version") + \
                            f" - {len(img.files)} files, {common.human(img.total_size)}"
                else:
                    msg = "Not found."
            except (SetupError, OSError) as e:
                msg = str(e)
            if self.v_iso.get().strip() == p:   # still the same disc
                self.q.put(("iso_status", msg, ok))
        threading.Thread(target=work, daemon=True).start()

    def _validate_waw(self) -> None:
        use = self.v_use_waw.get()
        steam_set = bool(self.v_steam.get().strip())
        for wdg in getattr(self, "steam_widgets", []):
            try:
                wdg.configure(state="normal" if use else "disabled")
            except tk.TclError:
                pass
        for wdg in getattr(self, "alt_widgets", []):
            try:
                wdg.configure(state="normal" if use and not steam_set else "disabled")
            except tk.TclError:
                pass
        self._waw_gen = getattr(self, "_waw_gen", 0) + 1
        gen = self._waw_gen
        if not use:
            self.v_waw_status.set("World at War is left out. Call of Duty 3 builds and plays without it; tick the "
                                  "box above to rip World at War's files too (for the new engine).")
            if hasattr(self, "lbl_waw_status"):
                self.lbl_waw_status.configure(style="Hint.TLabel")
            return
        source, notes = waw.choose_source(self.v_steam.get(), self.v_xbox.get(), self.v_pc.get(), self.v_alt.get())

        packs = list(self.packs)

        def work():
            ok, msg = waw.check_source(source)
            text = ("Using: " if ok else "Problem with the ") + waw.KIND_LABELS[source.kind] + \
                (f" - {msg}" if ok else f": {msg}")
            if source.kind == waw.NONE:
                text = msg
            extra = []
            if packs and source.kind not in (waw.STEAM, waw.NONE):
                missing = [Path(p).name for p in packs if not Path(p).is_file()]
                extra.append(f"Map packs: {len(packs)} download(s)" +
                             (f"; not found: {', '.join(missing)}." if missing else "."))
            elif packs and source.kind == waw.STEAM:
                extra.append("The Steam version already has every map pack, so the map pack downloads aren't used.")
            if gen == self._waw_gen:   # a newer check started meanwhile: its result wins
                self.q.put(("waw_status", " ".join([text] + notes + extra), ok and source.kind != waw.NONE))
        threading.Thread(target=work, daemon=True).start()

    # ------------------------------------------------------------------ building
    def _set_upscale_vars(self, model: str, noise: int, scale: int) -> None:
        self.v_upscale_model.set(upscale.MODELS.get(model, upscale.MODELS[upscale.DEFAULT_MODEL])[1])
        self.v_upscale_noise.set(upscale.NOISE_LEVELS.get(noise, upscale.NOISE_LEVELS[upscale.DEFAULT_NOISE]))
        self.v_upscale_scale.set(upscale.SCALES.get(scale, upscale.SCALES[upscale.DEFAULT_SCALE]))

    def _upscale_model_key(self) -> str:
        label = self.v_upscale_model.get()
        return next((k for k, (_, text) in upscale.MODELS.items() if text == label), upscale.DEFAULT_MODEL)

    def _upscale_noise_level(self) -> int:
        label = self.v_upscale_noise.get()
        return next((k for k, text in upscale.NOISE_LEVELS.items() if text == label), upscale.DEFAULT_NOISE)

    def _upscale_scale(self) -> int:
        label = self.v_upscale_scale.get()
        return next((k for k, text in upscale.SCALES.items() if text == label), upscale.DEFAULT_SCALE)

    def _options(self) -> BuildOptions:
        use = self.v_use_waw.get()   # unticked: no World at War at all, whatever is filled in
        return BuildOptions(
            cod3_source=self.v_iso.get().strip(),
            waw_steam=self.v_steam.get().strip() if use else "",
            waw_xbox360=self.v_xbox.get().strip() if use else "",
            waw_pc_disc=self.v_pc.get().strip() if use else "", waw_alternative=self.v_alt.get(),
            waw_map_packs=list(self.packs) if use and not self.v_steam.get().strip() else [],
            build_linux=self.v_linux.get(), build_windows=self.v_windows.get(),
            output_dir=self.v_out.get().strip(), work_dir=self.v_work.get().strip() or str(common.default_work_dir()),
            jobs=self._jobs(), build_recomp=self.v_recomp.get(),
            appimage=self.v_appimage.get(), cleanup=self.v_cleanup.get(), keep_sdk=self.v_keep_sdk.get(),
            install_deps=self.v_install.get(), fidelityfx=self.v_ffx.get(),
            upscale_textures=self.v_upscale.get(), upscale_model=self._upscale_model_key(),
            upscale_noise=self._upscale_noise_level(), upscale_scale=self._upscale_scale())

    def _start_build(self, overrides: Optional[dict] = None, what: str = "Build") -> None:
        """Starts a build; `overrides` (BuildOptions fields) make it a
        repair that redoes only what's broken."""
        if self.worker and self.worker.is_alive():
            return
        opts = self._options_with(overrides)
        blocker = self._blocker(opts, what)
        if blocker:
            self.v_status.set(blocker)
            return
        self._save()
        self.cancel.clear()
        common.APPROVAL.reset()
        common.APPROVAL.ask = self._ask_approval
        self._hide_banner()
        self._build_started = time.monotonic()
        self._build_what = what
        self.show_page("log")
        self._log_show_live()

        def work():
            try:
                Pipeline(opts, self.log, self.cancel,
                         on_step=lambda i, n, t: self.q.put(("step", i, n, t))).run()
                self.q.put(("done", "good", self._where_summary(opts)))
            except common.Cancelled:
                self.q.put(("done", "warn", "Press Build to continue later."))
            except SetupError as e:
                self.log.error(str(e))
                self.q.put(("done", "bad", str(e)))
            except Exception as e:  # pragma: no cover
                self.log.error(f"{type(e).__name__}: {e}")
                self.q.put(("done", "bad", f"Unexpected error: {e}"))
        self.worker = threading.Thread(target=work, daemon=True)
        self.worker.start()
        self._update_build_state()
        self._tick()

    def _tick(self) -> None:
        """The time the build has taken, next to the status."""
        if self.worker and self.worker.is_alive():
            self.v_elapsed.set(format_duration(time.monotonic() - self._build_started))
            self.root.after(1000, self._tick)

    def _build_done(self, kind: str, text: str) -> None:
        took = format_duration(time.monotonic() - self._build_started)
        what = getattr(self, "_build_what", "Build")
        self.root.title(common.APP_NAME)
        self.v_elapsed.set("")
        self.v_progress.set(1.0 if kind == "good" else self.v_progress.get())
        self._update_build_state()
        self._refresh_maps()
        if kind == "good":
            self.v_status.set(f"{what} finished in {took}.")
            self.log.info(text)
            self._show_banner("good", f"Done: {what.lower()} finished in {took}.",
                              "Everything is in the output folder. Play starts the game; Where things go lists "
                              "every folder.",
                              [("Play", lambda: self.show_page("play")), ("Where things go", self._show_locations)])
            self._output_changed()
        elif kind == "warn":
            self.v_status.set(f"Canceled after {took}.")
            self._show_banner("warn", f"Canceled after {took}.", text, [])
        else:
            self.v_status.set(text.splitlines()[0] if text else "Stopped.")
            self._show_banner("bad", f"{what} stopped after {took}.", text,
                              [("Show log", lambda: self.show_page("log"))])

    def _cancel(self) -> None:
        self.cancel.set()
        self.v_status.set("Canceling...")

    def _tools_dir(self) -> Path:
        return Path(self.v_work.get() or common.default_work_dir()) / "tools"

    def _jobs(self) -> int:
        """Build threads; an empty or garbled box means the recommended number."""
        try:
            return max(1, int(self.v_jobs.get()))
        except (tk.TclError, ValueError):
            return common.recommended_jobs()

    # ------------------------------------------------------------------ approval
    def _ask_approval(self, items: List[common.Install]) -> bool:
        """Called from the build thread: shows the list in the window and
        waits for the answer (no answer, a closed window or Cancel = no)."""
        done = threading.Event()
        answer: dict = {}
        self.q.put(("approve", items, done, answer))
        while not done.wait(0.2):
            if self.cancel.is_set():
                return False
        return bool(answer.get("ok"))

    def _show_approval(self, items: List[common.Install], done: threading.Event, answer: dict) -> None:
        """What will be downloaded and installed, from where, into which
        folder; nothing happens until "Download and install" (owner,
        2026-10-10)."""
        rows = [(i.what + (f"  ({i.size})" if i.size else ""), i.source, i.where) for i in items]

        def finish(ok: bool) -> None:
            answer["ok"] = ok
            done.set()
        self._table_window("Approve downloads",
                           "These will be downloaded and installed. Nothing happens until you approve.",
                           ["What", "From", "Goes into"], rows,
                           "Downloads are pinned releases checked against their SHA-256 (see docs/SOURCES.md). "
                           "\"Where things go\" lists every folder the tool uses.",
                           [("Download and install", lambda: finish(True)), ("Cancel", lambda: finish(False))],
                           on_close=lambda: finish(False), shares=(0.44, 0.18, 0.38))

    def _where_summary(self, opts: BuildOptions) -> str:
        out = Path(opts.output_dir)
        return (f"The game and the ripped files: {out}\n"
                f"Saves, game settings and logs: {out / 'userdata'}\n"
                f"Downloaded tools (kept for the next build): {Path(opts.work_dir) / 'tools'}")

    def _table_window(self, title: str, heading: str, columns: List[str], rows: List[tuple], note: str,
                      buttons: List[tuple], on_close: Optional[Callable[[], None]] = None,
                      open_column: Optional[int] = None, shares: tuple = (0.26, 0.32, 0.42)) -> None:
        """A table that follows the font size and the screen: long text wraps
        at the width in view, the rows scroll, the buttons stay in view. The
        first button is the main one."""
        d = widgets.dialog(self.root, title)
        ttk.Label(d, text=heading, style="Heading.TLabel", wraplength=px(640), justify="left").pack(
            side="top", anchor="w", padx=px(16), pady=(px(14), px(8)))
        bar = ttk.Frame(d)
        bar.pack(side="bottom", fill="x", padx=px(16), pady=px(14))
        if note:
            hint(d, note).pack(side="bottom", anchor="w", padx=px(16))
        body = ScrollFrame(d)
        body.pack(side="top", fill="both", expand=True, padx=px(16))
        t = body.inner
        for c, name in enumerate(columns):
            ttk.Label(t, text=name, style="Bold.TLabel").grid(row=0, column=c, sticky="w", padx=(0, px(12)),
                                                             pady=(0, px(4)))
            t.columnconfigure(c, weight=1 if c else 0)
        wrapped = []
        for r, row in enumerate(rows, start=1):
            for c, text in enumerate(row):
                lbl = ttk.Label(t, text=text, justify="left", style="Bold.TLabel" if c == 0 else "Hint.TLabel")
                lbl.grid(row=r, column=c, sticky="nw", padx=(0, px(12)), pady=px(3))
                wrapped.append((c, lbl))
            if open_column is not None and Path(row[open_column]).is_dir():
                folder = row[open_column]
                ttk.Button(t, text="Open", command=lambda f=folder: self._open(f)).grid(
                    row=r, column=len(row), sticky="ne", pady=px(3))

        def rewrap(_=None) -> None:
            width = max(300, body.winfo_width() - (body.vbar.winfo_width() if body.vbar.winfo_ismapped() else 0))
            for c, lbl in wrapped:
                want = int(width * (shares[c] if c < len(shares) else 0.3)) - 14
                if int(lbl.cget("wraplength") or 0) != want:
                    lbl.configure(wraplength=want)
            body.relayout()
        body.bind("<Configure>", rewrap, add="+")

        def close(action: Optional[Callable[[], None]] = None) -> None:
            try:
                d.grab_release()
            except tk.TclError:
                pass
            d.destroy()
            if action:
                action()
        for i, (text, action) in enumerate(reversed(buttons)):
            ttk.Button(bar, text=text, command=lambda a=action: close(a),
                       style="Accent.TButton" if i == len(buttons) - 1 else "TButton").pack(
                side="right", padx=(px(6), 0))
        if on_close:
            d.protocol("WM_DELETE_WINDOW", lambda: close(on_close))
            d.bind("<Escape>", lambda e: close(on_close))
        else:
            d.protocol("WM_DELETE_WINDOW", close)
            d.bind("<Escape>", lambda e: close())
        sw, sh = d.winfo_screenwidth(), d.winfo_screenheight()
        w, h = min(px(860), int(sw * 0.85)), min(px(560), int(sh * 0.8))
        d.update_idletasks()
        x = self.root.winfo_rootx() + max(0, (self.root.winfo_width() - w) // 2)
        y = self.root.winfo_rooty() + max(0, (self.root.winfo_height() - h) // 3)
        d.geometry(f"{w}x{h}+{max(0, min(x, sw - w))}+{max(0, min(y, sh - h))}")
        d.minsize(min(w, px(420)), min(h, px(240)))
        d.deiconify()
        try:
            d.grab_set()
        except tk.TclError:
            pass
        d.focus_set()

    # ------------------------------------------------------------------ plumbing
    def _poll(self) -> None:
        # Rescheduled first, and never run inside itself: a dialog opened
        # from here can't start a second chain of polls.
        self.root.after(100, self._poll)
        if self._in_poll:
            return
        self._in_poll = True
        try:
            for _ in range(500):
                msg = self.q.get_nowait()
                kind = msg[0]
                if kind == "log":
                    _, k, text, frac = msg
                    if k == "progress":
                        if frac is not None:
                            self.v_progress.set(frac)
                        if text:
                            self.v_status.set(text)
                    else:
                        self._append(k, text)
                        if k in ("step", "warn", "error"):
                            self.v_status.set(text.strip())
                elif kind == "step":
                    _, i, n, title = msg
                    self.root.title(f"{common.APP_NAME} - [{i + 1}/{n}] {title}")
                    self.v_progress.set(0)
                elif kind == "done":
                    self._build_done(msg[1], msg[2])
                elif kind == "set":
                    msg[1].set(msg[2])
                elif kind == "waw_status":
                    self.v_waw_status.set(msg[1])
                    self.lbl_waw_status.configure(style="Good.TLabel" if msg[2] else "Warn.TLabel")
                elif kind == "iso_status":
                    self.v_iso_status.set(msg[1])
                    self.lbl_iso_status.configure(style="Good.TLabel" if msg[2] else "Warn.TLabel")
                elif kind == "approve":
                    self._show_approval(msg[1], msg[2], msg[3])
                elif kind == "checked":
                    self._show_check(*msg[1:])
                elif kind == "info":
                    self.v_status.set("Ready.")
                    self._info(msg[1], msg[2])
                elif kind == "error":
                    self.v_status.set("Ready.")
                    self._info("Error", msg[1], kind="error")
                elif kind == "refresh":
                    self._refresh_maps()
                elif kind == "call":
                    msg[1]()
        except queue.Empty:
            pass
        finally:
            self._in_poll = False

    # ------------------------------------------------------------------ settings
    def _settings(self) -> dict:
        """The build and play settings: what config/settings.json holds and
        what a profile keeps."""
        return {
            "cod3_source": self.v_iso.get(), "waw_steam": self.v_steam.get(), "waw_xbox360": self.v_xbox.get(),
            "waw_pc_disc": self.v_pc.get(), "waw_alternative": self.v_alt.get(), "waw_map_packs": self.packs,
            "use_waw": self.v_use_waw.get(),
            "build_linux": self.v_linux.get(), "build_windows": self.v_windows.get(),
            "output_dir": self.v_out.get(),
            "work_dir": self.v_work.get(), "jobs": self._jobs(), "build_recomp": self.v_recomp.get(),
            "appimage": self.v_appimage.get(), "install_deps": self.v_install.get(), "fidelityfx": self.v_ffx.get(),
            "cleanup": self.v_cleanup.get(), "keep_sdk": self.v_keep_sdk.get(),
            "upscale_textures": self.v_upscale.get(), "upscale_model": self._upscale_model_key(),
            "upscale_noise": self._upscale_noise_level(), "upscale_scale": self._upscale_scale(),
            "cod3_windowed": self.v_cod3_windowed.get(), "cod3_args": self.v_cod3_args.get(),
        }

    def _apply_settings(self, s: dict) -> None:
        """Fills the form from a settings dict (a loaded profile); keys it
        doesn't have stay as they are."""
        strings = {"cod3_source": self.v_iso, "waw_steam": self.v_steam, "waw_xbox360": self.v_xbox,
                   "waw_pc_disc": self.v_pc, "waw_alternative": self.v_alt, "output_dir": self.v_out,
                   "work_dir": self.v_work, "cod3_args": self.v_cod3_args}
        bools = {"use_waw": self.v_use_waw, "build_linux": self.v_linux, "build_windows": self.v_windows,
                 "build_recomp": self.v_recomp, "appimage": self.v_appimage, "install_deps": self.v_install,
                 "fidelityfx": self.v_ffx, "cleanup": self.v_cleanup, "keep_sdk": self.v_keep_sdk,
                 "upscale_textures": self.v_upscale, "cod3_windowed": self.v_cod3_windowed}
        for k, var in strings.items():
            if isinstance(s.get(k), str):
                var.set(s[k])
        for k, var in bools.items():
            if isinstance(s.get(k), bool):
                var.set(s[k])
        if isinstance(s.get("jobs"), int):
            self.v_jobs.set(s["jobs"])
        self._set_upscale_vars(s.get("upscale_model", self._upscale_model_key()),
                               s.get("upscale_noise", self._upscale_noise_level()),
                               s.get("upscale_scale", self._upscale_scale()))
        if isinstance(s.get("waw_map_packs"), list):
            self._set_packs([p for p in s["waw_map_packs"] if isinstance(p, str) and p])
        self._update_versions()
        self._update_upscale()
        self._validate_waw()
        self._update_build_state()

    def _ui_settings(self) -> dict:
        ui = {"ui_theme": theme.theme_id_for_label(self.v_theme.get()), "ui_accent": self.accent,
              "ui_page": getattr(self, "page", "setup")}
        try:
            zoomed = (self.root.state() == "zoomed") if IS_WINDOWS else bool(int(self.root.attributes("-zoomed")))
        except (tk.TclError, ValueError):
            zoomed = False
        ui["ui_zoomed"] = zoomed
        if zoomed or self.root.state() != "normal":
            ui["ui_geometry"] = self._saved_ui.get("ui_geometry") or ""
        else:
            ui["ui_geometry"] = f"{self.root.winfo_width()}x{self.root.winfo_height()}"
        return ui

    def _save(self) -> None:
        common.write_json(SETTINGS_FILE, {**self._settings(), **self._ui_settings()})

    def _save_quietly(self) -> None:
        try:
            self._save()
        except OSError:
            pass

    def _on_close(self) -> None:
        if self.worker and self.worker.is_alive():
            if not widgets.ask_yes_no(self.root, "Build running", "A build is running. Cancel it and quit?\n"
                                                                   "Press Build again later to continue.",
                                      yes="Cancel and quit", no="Keep building", danger=True):
                return
            self.cancel.set()
        try:
            self._save()
        except Exception:  # noqa: BLE001 - never keep the window from closing
            pass
        self.root.destroy()


def run(launcher: bool = False) -> int:
    if IS_WINDOWS:
        # Sharp text at Windows' display scaling instead of a stretched
        # bitmap (Tk then sizes its fonts for the real DPI).
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    root = tk.Tk()
    apply_desktop_scale(root)
    theme.setup_fonts(root, windows=IS_WINDOWS)
    App(root, launcher)
    root.mainloop()
    return 0

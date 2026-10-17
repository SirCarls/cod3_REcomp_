"""Small Tk building blocks shared by the setup tool's pages: desktop
scaling (px), a scrolling frame, tooltips, section headings and themed
dialogs (message, yes/no, a name prompt). Colors come from c3w/theme.py.
"""
from __future__ import annotations

import os
import tkinter as tk
from tkinter import ttk
from typing import Callable, List, Optional, Sequence, Tuple, Union

from . import theme

# The desktop's text scale (1.0 = 96 DPI), set before the window is made.
# Pixel sizes in the layout (wrap widths, paddings) go through px().
_UI_SCALE = 1.0


def set_scale(scale: float) -> None:
    global _UI_SCALE
    _UI_SCALE = scale


def ui_scale() -> float:
    return _UI_SCALE


def px(n: float) -> int:
    return int(round(n * _UI_SCALE))


# Help texts wrap at this width at most; on a narrower page, at the width
# the page has (ScrollFrame re-wraps them when its width changes).
WRAP = 640
MIN_WRAP = 160
# Labels that wrap to the page: (label, their widest wrap at 96 DPI).
_AUTOWRAP: List[Tuple[object, int]] = []


def autowrap(label, wrap: int = WRAP):
    """Makes a label's text wrap at the room its page has, never wider than
    `wrap`. Returns the label."""
    _AUTOWRAP.append((label, wrap))
    label.configure(wraplength=px(wrap))
    return label


class ScrollFrame(ttk.Frame):
    """A frame whose content (`.inner`) scrolls when the window is too small
    for it: a vertical and a horizontal scrollbar, each shown only when
    needed. While the window is big enough, the content stretches to fill it
    as a plain frame would. The mouse wheel scrolls up/down, Shift + wheel
    sideways."""

    def __init__(self, parent, **kw):
        super().__init__(parent, **kw)
        self.canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0, background=theme.current["bg"])
        self.vbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.hbar = ttk.Scrollbar(self, orient="horizontal", command=self.canvas.xview)
        self.canvas.configure(yscrollcommand=self.vbar.set, xscrollcommand=self.hbar.set)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.vbar.grid(row=0, column=1, sticky="ns")
        self.hbar.grid(row=1, column=0, sticky="ew")
        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)
        self.inner = ttk.Frame(self.canvas)
        self._win = self.canvas.create_window(0, 0, window=self.inner, anchor="nw")
        self.inner.bind("<Configure>", lambda e: self._layout())
        self.bind("<Configure>", lambda e: self._layout())
        self.bind("<Enter>", lambda e: self._wheel(True))
        self.bind("<Leave>", lambda e: self._wheel(False))
        self._busy = False
        self._rewrap_width = 0
        self._last_need = (0, 0)
        self.after(500, self._watch)

    def _watch(self) -> None:
        """The content can come to want another size without Tk telling
        this frame (a label's new text: the canvas fixes the content's
        actual size, so no <Configure> comes). Looked at twice a second
        while in view: two numbers compared."""
        try:
            if self.winfo_ismapped() and \
                    (self.inner.winfo_reqwidth(), self.inner.winfo_reqheight()) != self._last_need:
                self._layout()
            self.after(500, self._watch)
        except tk.TclError:
            pass   # the window is gone

    def _layout(self) -> None:
        if self._busy:
            return
        self._busy = True
        try:
            need_w, need_h = self.inner.winfo_reqwidth(), self.inner.winfo_reqheight()
            self._last_need = (need_w, need_h)
            full_w, full_h = self.winfo_width(), self.winfo_height()
            if full_w <= 1 or full_h <= 1:
                return  # not drawn yet
            if self._rewrap(full_w):
                # Once the wrapped texts have their new size, lay out again:
                # a moment later, when Tk has passed the new sizes up from
                # the labels through their frames (one idle pass per level;
                # the page frame itself gets no <Configure> for it, as the
                # canvas sets its size). Not update_idletasks() here: Tk
                # would deliver this frame's own <Configure> inside it, while
                # busy, and lose it.
                self.after(15, self._layout)
                self.after(150, self._layout)    # (and once more, on a busy or slow display)
            # Both bars are decided from the frame's whole size, in one go:
            # deciding each from the canvas (which shrinks when the other bar
            # shows) can flip them back and forth forever.
            vbw, hbh = self.vbar.winfo_reqwidth(), self.hbar.winfo_reqheight()
            need_v = need_h > full_h
            need_h_bar = need_w > full_w - (vbw if need_v else 0)
            if need_h_bar and not need_v:
                need_v = need_h > full_h - hbh
                need_h_bar = need_w > full_w - (vbw if need_v else 0)
            have_w = full_w - (vbw if need_v else 0)
            have_h = full_h - (hbh if need_h_bar else 0)
            # Stretch to the window when it's big enough; otherwise keep the
            # content's own size and scroll.
            self.canvas.itemconfigure(self._win, width=max(need_w, have_w), height=max(need_h, have_h))
            self.canvas.configure(scrollregion=(0, 0, max(need_w, have_w), max(need_h, have_h)))
            for bar, needed in ((self.vbar, need_v), (self.hbar, need_h_bar)):
                if needed and not bar.winfo_ismapped():
                    bar.grid()
                elif not needed and bar.winfo_ismapped():
                    bar.grid_remove()
            if not need_v:
                self.canvas.yview_moveto(0)
            if not need_h_bar:
                self.canvas.xview_moveto(0)
        finally:
            self._busy = False

    def _rewrap(self, width: int) -> bool:
        """Wraps this page's help texts at the room they have: the page's
        width (less the scrollbar's, always kept free so the wrap doesn't
        flip with it) from where each text starts, less a margin. Returns
        whether any changed."""
        room_w = width - self.vbar.winfo_reqwidth() - px(30)
        prefix = str(self.inner) + "."
        changed, alive = False, []
        for label, most in _AUTOWRAP:
            try:
                if not label.winfo_exists():
                    continue
            except tk.TclError:
                continue
            alive.append((label, most))
            if not str(label).startswith(prefix):
                continue
            x, w = 0, label
            while w is not None and w is not self.inner:   # where it starts, in the page
                x += w.winfo_x()
                w = w.master
            # Room kept free on its right (a page title's buttons beside its text).
            reserve = getattr(label, "c3w_reserve", None)
            want = max(px(MIN_WRAP), min(px(most), room_w - x - (reserve() if reserve else 0)))
            if int(str(label.cget("wraplength")) or 0) != want:
                label.configure(wraplength=want)
                changed = True
        _AUTOWRAP[:] = alive
        return changed

    def relayout(self) -> None:
        """After content changed its requested size (wrapped text): the
        content's own frame doesn't report that, since the canvas sets its
        size."""
        self.after_idle(self._layout)

    def to_top(self) -> None:
        self.canvas.yview_moveto(0)

    def _wheel(self, on: bool) -> None:
        events = ("<MouseWheel>", "<Shift-MouseWheel>", "<Button-4>", "<Button-5>",
                  "<Shift-Button-4>", "<Shift-Button-5>")
        if not on:
            for ev in events:
                self.unbind_all(ev)
            return
        self.bind_all("<MouseWheel>", lambda e: self._scroll(e, "y"))
        self.bind_all("<Shift-MouseWheel>", lambda e: self._scroll(e, "x"))
        self.bind_all("<Button-4>", lambda e: self._scroll(e, "y", -1))
        self.bind_all("<Button-5>", lambda e: self._scroll(e, "y", 1))
        self.bind_all("<Shift-Button-4>", lambda e: self._scroll(e, "x", -1))
        self.bind_all("<Shift-Button-5>", lambda e: self._scroll(e, "x", 1))

    def _scroll(self, event, axis: str, step: int = 0) -> None:
        if not step:
            step = -1 if event.delta > 0 else 1
        bar = self.vbar if axis == "y" else self.hbar
        if bar.winfo_ismapped():   # only when there is something to scroll
            (self.canvas.yview_scroll if axis == "y" else self.canvas.xview_scroll)(step * 3, "units")


class Tooltip:
    """A short help text that shows when the pointer rests on a widget.
    `text` can be a function, for text that changes (why a button is off)."""

    DELAY_MS = 550

    def __init__(self, widget, text: Union[str, Callable[[], str]],
                 reason: Optional[Callable[[], str]] = None):
        self.widget = widget
        self.text = text
        self.reason = reason     # why the widget is off; shown instead of the text when not ""
        self._after: Optional[str] = None
        self._tip: Optional[tk.Toplevel] = None
        widget.bind("<Enter>", self._schedule, add="+")
        widget.bind("<Leave>", self._hide, add="+")
        widget.bind("<ButtonPress>", self._hide, add="+")

    def _schedule(self, _=None) -> None:
        self._cancel()
        self._after = self.widget.after(self.DELAY_MS, self._show)

    def _cancel(self) -> None:
        if self._after:
            try:
                self.widget.after_cancel(self._after)
            except tk.TclError:
                pass
            self._after = None

    def _show(self) -> None:
        self._after = None
        text = (self.reason() if self.reason else "") or (self.text() if callable(self.text) else self.text)
        if not text or self._tip is not None:
            return
        try:
            x = self.widget.winfo_rootx() + px(8)
            y = self.widget.winfo_rooty() + self.widget.winfo_height() + px(4)
        except tk.TclError:
            return
        c = theme.current
        tip = self._tip = tk.Toplevel(self.widget)
        tip.wm_overrideredirect(True)
        try:
            tip.wm_attributes("-topmost", True)
        except tk.TclError:
            pass
        tip.configure(background=c["border"])
        lbl = tk.Label(tip, text=text, justify="left", background=c["surface"], foreground=c["text"],
                       font="TkTooltipFont", wraplength=px(360), padx=px(8), pady=px(5), borderwidth=0)
        lbl.c3w_role = "tooltip"
        lbl.pack(padx=1, pady=1)
        tip.update_idletasks()
        sw, sh = tip.winfo_screenwidth(), tip.winfo_screenheight()
        w, h = tip.winfo_reqwidth(), tip.winfo_reqheight()
        if y + h > sh - 4:   # no room below: above the widget
            y = max(0, self.widget.winfo_rooty() - h - px(4))
        tip.wm_geometry(f"+{max(0, min(x, sw - w - 4))}+{y}")

    def _hide(self, _=None) -> None:
        self._cancel()
        if self._tip is not None:
            try:
                self._tip.destroy()
            except tk.TclError:
                pass
            self._tip = None


def tip(widget, text: Union[str, Callable[[], str], None] = None, reason: Optional[Callable[[], str]] = None):
    """Gives a widget its tooltip (a second call changes the same one).
    `reason` says why the widget is off; while it returns text, that shows
    instead. Returns the widget (for chaining)."""
    t = getattr(widget, "_c3w_tip", None)
    if t is None:
        t = widget._c3w_tip = Tooltip(widget, text or "", reason)
    else:
        if text is not None:
            t.text = text
        if reason is not None:
            t.reason = reason
    return widget


def page_header(parent, title: str, subtitle: str = "", row: int = 0, columns: int = 3) -> ttk.Frame:
    """A page's big title and one-line description. Returns the header frame
    (buttons can be packed on its right)."""
    head = ttk.Frame(parent)
    head.grid(row=row, column=0, columnspan=columns, sticky="ew", padx=px(24), pady=(px(18), px(4)))
    text = ttk.Frame(head)
    text.pack(side="left", fill="x", expand=True)
    ttk.Label(text, text=title, style="Title.TLabel").pack(anchor="w")
    if subtitle:
        sub = autowrap(ttk.Label(text, text=subtitle, style="Hint.TLabel", justify="left"))
        sub.pack(anchor="w", pady=(px(2), 0))
        # Buttons packed on the header's right later take their room from the text's.
        sub.c3w_reserve = lambda: sum(k.winfo_reqwidth() + px(12) for k in head.winfo_children() if k is not text)
    return head


def section(parent, title: str, row: int, columns: int = 3, top: int = 18) -> ttk.Frame:
    """A section heading like the game's F2 (bold, accent color, a rule to
    the right), and the frame under it for the section's rows."""
    head = ttk.Frame(parent)
    head.grid(row=row, column=0, columnspan=columns, sticky="ew", padx=px(24), pady=(px(top), px(6)))
    ttk.Label(head, text=title, style="Heading.TLabel").pack(side="left")
    ttk.Separator(head, orient="horizontal").pack(side="left", fill="x", expand=True, padx=(px(10), 0), pady=(px(2), 0))
    body = ttk.Frame(parent)
    body.grid(row=row + 1, column=0, columnspan=columns, sticky="ew", padx=(px(30), px(24)))
    body.head = head  # type: ignore[attr-defined]
    return body


def hint(parent, text="", var: Optional[tk.Variable] = None, style: str = "Hint.TLabel",
         wrap: int = WRAP, **kw) -> ttk.Label:
    """A help text (dim, wrapped at the room its page has, `wrap` at most)."""
    if var is not None:
        return autowrap(ttk.Label(parent, textvariable=var, style=style, justify="left", **kw), wrap)
    return autowrap(ttk.Label(parent, text=text, style=style, justify="left", **kw), wrap)


class LineProgress(tk.Frame):
    """A progress bar drawn as a thin line: the divider's color, filled from
    the left in the accent color (theme.recolor colors both by their
    role). ttk's own bar can't be made this thin in every theme."""

    def __init__(self, parent, variable: tk.Variable, height: int):
        super().__init__(parent, height=height, borderwidth=0, highlightthickness=0)
        self.c3w_role = "divider"
        self.fill = tk.Frame(self, borderwidth=0, highlightthickness=0)
        self.fill.c3w_role = "accent"
        self.var = variable
        variable.trace_add("write", lambda *a: self._update())
        theme.recolor(self)
        theme.recolor(self.fill)
        self._update()

    def _update(self) -> None:
        try:
            value = max(0.0, min(1.0, float(self.var.get())))
        except (tk.TclError, ValueError):
            value = 0.0
        if value <= 0:
            self.fill.place_forget()
        else:
            self.fill.place(x=0, y=0, relheight=1, relwidth=value)


def fit_list_width(combo, labels: Sequence[str], most_chars: int = 34) -> None:
    """A list box as wide as its longest choice, but no wider than
    `most_chars` characters, so a small window doesn't scroll sideways for
    it; its drop-down list then still opens wide enough to show every
    choice whole (ttk's -postoffset)."""
    want = max([12] + [len(x) + 2 for x in labels])
    combo.configure(width=min(most_chars, want))
    if want <= most_chars:
        return
    import tkinter.font as tkfont
    try:
        f = tkfont.nametofont("TkDefaultFont", root=combo)
        extra = max(f.measure(x) for x in labels) + px(44) - f.measure("0") * most_chars
    except tk.TclError:
        return
    if extra > 0:
        style = f"Wide{extra}.TCombobox"
        ttk.Style(combo).configure(style, postoffset=(0, 0, extra, 0))
        combo.configure(style=style)


def link(parent, text: str, command: Callable[[], None], **kw) -> ttk.Button:
    """A button that looks like a link (accent text, no box)."""
    return ttk.Button(parent, text=text, command=command, style="Link.TButton", cursor="hand2",
                      takefocus=True, **kw)


def button_row(parent, buttons: Sequence[Tuple[str, Callable[[], None], str]], style: str = "TButton",
               side: str = "left") -> List[ttk.Button]:
    """Buttons in a row, each with its tooltip: (text, command, tooltip)."""
    made = []
    for i, (text, cmd, tooltip) in enumerate(buttons):
        b = ttk.Button(parent, text=text, command=cmd, style=style)
        b.pack(side=side, padx=(0 if i == 0 else px(6), 0) if side == "left" else (px(6), 0))
        if tooltip:
            tip(b, tooltip)
        made.append(b)
    return made


def title_bar_dark(win, dark: bool) -> None:
    """Windows 10 and 11: a dark title bar for the dark themes (the window
    manager draws it, so ttk styles can't). Elsewhere, and where Windows
    doesn't have it (older versions, Wine), nothing happens."""
    if os.name != "nt":
        return
    try:
        import ctypes
        win.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(win.winfo_id())
        value = ctypes.c_int(1 if dark else 0)
        # DWMWA_USE_IMMERSIVE_DARK_MODE: 20, or 19 on Windows 10 before 20H1.
        for attr in (20, 19):
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(value),
                                                          ctypes.sizeof(value)) == 0:
                break
    except (AttributeError, OSError, ValueError, tk.TclError):
        pass


# ---------------------------------------------------------------------------
# Dialogs: themed Toplevels (Esc closes, Enter picks the default)
# ---------------------------------------------------------------------------
def _center(win: tk.Toplevel, parent, w: int, h: int) -> None:
    sw, sh = win.winfo_screenwidth(), win.winfo_screenheight()
    w, h = min(w, int(sw * 0.9)), min(h, int(sh * 0.85))
    try:
        top = parent.winfo_toplevel()
        x = top.winfo_rootx() + max(0, (top.winfo_width() - w) // 2)
        y = top.winfo_rooty() + max(0, (top.winfo_height() - h) // 3)
    except tk.TclError:
        x, y = (sw - w) // 2, (sh - h) // 3
    win.geometry(f"{w}x{h}+{max(0, min(x, sw - w))}+{max(0, min(y, sh - h))}")


def dialog(parent, title: str) -> tk.Toplevel:
    """An empty themed dialog window, on top of its parent."""
    d = tk.Toplevel(parent)
    d.withdraw()
    d.title(title)
    d.transient(parent.winfo_toplevel())
    # A thin border in the theme's color: dialogs stand out from the window
    # behind them, whatever the window manager draws.
    d.configure(background=theme.current["bg"], highlightthickness=1,
                highlightbackground=theme.current["border"], highlightcolor=theme.current["border"])
    title_bar_dark(d, not theme.current["light"])
    return d


def show_dialog(d: tk.Toplevel, parent, modal: bool = True, min_w: int = 360) -> None:
    """Sizes a dialog to its content, centers it on the parent window and
    shows it (grabbing input when modal)."""
    d.update_idletasks()
    w = max(d.winfo_reqwidth(), px(min_w))
    h = d.winfo_reqheight()
    _center(d, parent, w, h)
    d.deiconify()
    d.minsize(min(w, px(320)), min(h, px(160)))
    if modal:
        try:
            d.grab_set()
        except tk.TclError:   # another window holds the grab
            pass
    d.lift()
    d.focus_set()


def message(parent, title: str, text: str, kind: str = "info", detail: str = "",
            buttons: Sequence[Tuple[str, Optional[Callable[[], None]]]] = (("OK", None),),
            wait: bool = False) -> None:
    """A message in the theme's colors. kind: info, warning, error, done.
    The first button is the default (Enter); Esc closes."""
    d = dialog(parent, title)
    body = ttk.Frame(d, padding=(px(20), px(16), px(20), px(8)))
    body.pack(fill="both", expand=True)
    style = {"error": "BadBold.TLabel", "warning": "WarnBold.TLabel", "done": "GoodBold.TLabel"}.get(
        kind, "Heading.TLabel")
    first, _, rest = text.partition("\n")
    ttk.Label(body, text=first, style=style, wraplength=px(460), justify="left").pack(anchor="w")
    more = (rest.strip("\n") + ("\n\n" + detail if detail else "")).strip("\n")
    if more:
        ttk.Label(body, text=more, wraplength=px(460), justify="left").pack(anchor="w", pady=(px(8), 0))
    bar = ttk.Frame(d, padding=(px(20), px(8), px(20), px(16)))
    bar.pack(fill="x")

    def close(action=None) -> None:
        try:
            d.grab_release()
        except tk.TclError:
            pass
        d.destroy()
        if action:
            action()
    made = []
    for i, (label, action) in enumerate(reversed(list(buttons))):
        b = ttk.Button(bar, text=label, command=lambda a=action: close(a),
                       style="Accent.TButton" if i == len(buttons) - 1 else "TButton")
        b.pack(side="right", padx=(px(6), 0))
        made.append(b)
    d.protocol("WM_DELETE_WINDOW", close)
    d.bind("<Escape>", lambda e: close())
    d.bind("<Return>", lambda e: close(buttons[0][1]))
    show_dialog(d, parent)
    if made:
        made[-1].focus_set()
    if wait:
        parent.wait_window(d)


def ask_yes_no(parent, title: str, text: str, yes: str = "Yes", no: str = "No", danger: bool = False) -> bool:
    """Waits for an answer; Esc or closing the window means no."""
    answer = {"yes": False}

    def said_yes() -> None:
        answer["yes"] = True
    d = dialog(parent, title)
    body = ttk.Frame(d, padding=(px(20), px(16), px(20), px(8)))
    body.pack(fill="both", expand=True)
    first, _, rest = text.partition("\n")
    ttk.Label(body, text=first, style="WarnBold.TLabel" if danger else "Heading.TLabel", wraplength=px(460),
              justify="left").pack(anchor="w")
    if rest.strip():
        ttk.Label(body, text=rest.strip("\n"), wraplength=px(460), justify="left").pack(anchor="w", pady=(px(8), 0))
    bar = ttk.Frame(d, padding=(px(20), px(8), px(20), px(16)))
    bar.pack(fill="x")

    def close(ok: bool) -> None:
        if ok:
            said_yes()
        try:
            d.grab_release()
        except tk.TclError:
            pass
        d.destroy()
    b_yes = ttk.Button(bar, text=yes, style="Accent.TButton", command=lambda: close(True))
    b_no = ttk.Button(bar, text=no, command=lambda: close(False))
    b_yes.pack(side="right", padx=(px(6), 0))
    b_no.pack(side="right")
    d.protocol("WM_DELETE_WINDOW", lambda: close(False))
    d.bind("<Escape>", lambda e: close(False))
    d.bind("<Return>", lambda e: close(True))
    show_dialog(d, parent)
    (b_no if danger else b_yes).focus_set()
    parent.wait_window(d)
    return answer["yes"]


def ask_string(parent, title: str, text: str, initial: str = "", ok: str = "OK",
               check: Optional[Callable[[str], str]] = None) -> Optional[str]:
    """Asks for a short text (a profile's name). `check` returns why the
    text can't be used ("" when it can); OK stays off until it can.
    None when canceled."""
    result: dict = {}
    d = dialog(parent, title)
    body = ttk.Frame(d, padding=(px(20), px(16), px(20), px(8)))
    body.pack(fill="both", expand=True)
    ttk.Label(body, text=text, wraplength=px(420), justify="left").pack(anchor="w")
    var = tk.StringVar(value=initial)
    entry = ttk.Entry(body, textvariable=var, width=40)
    entry.pack(fill="x", pady=(px(8), px(2)))
    why = tk.StringVar()
    ttk.Label(body, textvariable=why, style="Warn.TLabel", wraplength=px(420), justify="left").pack(anchor="w")
    bar = ttk.Frame(d, padding=(px(20), px(8), px(20), px(16)))
    bar.pack(fill="x")

    def close(accept: bool) -> None:
        if accept:
            value = var.get().strip()
            if not value or (check and check(value)):
                return
            result["value"] = value
        try:
            d.grab_release()
        except tk.TclError:
            pass
        d.destroy()
    b_ok = ttk.Button(bar, text=ok, style="Accent.TButton", command=lambda: close(True))
    b_ok.pack(side="right", padx=(px(6), 0))
    ttk.Button(bar, text="Cancel", command=lambda: close(False)).pack(side="right")

    def validate(*_):
        value = var.get().strip()
        reason = "Type a name." if not value else (check(value) if check else "")
        why.set(reason if value else "")
        b_ok.configure(state="disabled" if reason else "normal")
    var.trace_add("write", validate)
    validate()
    d.protocol("WM_DELETE_WINDOW", lambda: close(False))
    d.bind("<Escape>", lambda e: close(False))
    d.bind("<Return>", lambda e: close(True))
    show_dialog(d, parent)
    entry.focus_set()
    entry.select_range(0, "end")
    parent.wait_window(d)
    return result.get("value")

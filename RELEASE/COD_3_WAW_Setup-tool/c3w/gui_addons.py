"""[acct2-decomp] Set up's "Map packs" row (in "Your disc", under the disc):
add the Xbox 360 map packs, see what's in, remove one. The work is in
addons.py; this is only the buttons. Mixed into App like Updates."""
from __future__ import annotations

import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk
from typing import Optional

from . import addons, common, disc_needs, theme
from .widgets import hint, px, tip

PACK_TYPES = [("Map pack downloads", "*.zip *.ZIP"), ("Xbox 360 packages", "*")]


class Addons:
    def _addons_row(self, parent, r: int) -> None:
        """Row r: the label, what's installed, Add... / Folder... / Remove...;
        row r + 1: a hint."""
        self.v_addons = tk.StringVar(value="none")
        ttk.Label(parent, text="Map packs (Xbox 360)").grid(row=r, column=0, sticky="w", padx=(0, px(10)),
                                                             pady=px(3))
        e = ttk.Entry(parent, textvariable=self.v_addons, width=8, state="readonly")
        e.grid(row=r, column=1, sticky="ew", pady=px(3))
        tip(e, "The map packs in the output folder's userdata: multiplayer's map list has them from the game's "
           "next start.")  # [acct2-decomp] the campaign doesn't use them
        f = ttk.Frame(parent)
        f.grid(row=r, column=2, sticky="e", padx=(px(6), 0))
        for text, cmd, help_text in (
                ("Add...", self._add_addons, "Pick the map pack downloads (.zip) or the packages themselves."),
                ("Folder...", self._add_addons_folder, "Pick a folder holding map pack downloads or packages."),
                ("Remove...", self._remove_addon, "Remove a map pack you added.")):
            b = ttk.Button(f, text=text, command=cmd)
            b.pack(side="left", padx=(px(4), 0))
            tip(b, help_text)
        hint(parent, "Your Call of Duty 3 map pack downloads (Champs, Valor, Bravo), for multiplayer: only a game "
                     "built from the USA/Europe disc has it. They're only read: copied into the game's userdata, "
                     "never run. Only the known packs are added.").grid(
            row=r + 1, column=1, columnspan=2, sticky="w")
        self._refresh_addons()

    def _addons_output(self) -> Optional[Path]:
        out = self.v_out.get().strip() if hasattr(self, "v_out") else ""
        return Path(out) if out else None

    def _refresh_addons(self) -> None:
        if hasattr(self, "v_addons"):
            self.v_addons.set(addons.summary(self._addons_output()))

    def _add_addons(self) -> None:
        chosen = filedialog.askopenfilenames(title="Call of Duty 3 map pack downloads", parent=self.root,
                                             filetypes=PACK_TYPES)
        if chosen:
            self._start_addons([Path(c) for c in chosen])

    def _add_addons_folder(self) -> None:
        p = filedialog.askdirectory(title="Folder with Call of Duty 3 map pack downloads", parent=self.root)
        if p:
            self._start_addons([Path(p)])

    def _start_addons(self, sources) -> None:
        out = self._addons_output()
        if out is None:
            self._info("No output folder", "Choose the output folder on Set up first.", kind="warning")
            return
        if self.worker and self.worker.is_alive():
            self._info("Busy", "Wait for the build to finish first.", kind="warning")
            return
        scratch = Path(self.v_work.get().strip() or common.default_work_dir()) / "scratch" / "addons"
        cancel = threading.Event()          # its own: the build's Cancel doesn't stop it
        # [acct2-decomp] map packs are multiplayer's: say so when the game has none (they're added all the same)
        built = disc_needs.game_disc(out)
        if built is not None and not disc_needs.RELEASES[built.key].multiplayer:
            self._info("Map packs", f"Your game was built from {disc_needs.RELEASES[built.key].short}, which has no "
                                    "multiplayer the recomp knows: map packs are used by multiplayer, in a game "
                                    "built from the USA/Europe disc. They're added all the same, for such a build.")

        def work():
            added = addons.add(sources, out, scratch, cancel, self.log)
            self.q.put(("call", self._refresh_addons))
            self.q.put(("info", "Map packs added", "\n".join(
                f"{a.name}: {', '.join(a.maps)}" for a in added) + "\n\nThey're in multiplayer's map list from "
                "the game's next start."))
        self._bg(work, "Adding map packs...")

    def _remove_addon(self) -> None:
        out = self._addons_output()
        items = addons.read_registry(out) if out else []
        if not items:
            self._info("No map packs", "No map packs have been added to this output folder.")
            return
        win = tk.Toplevel(self.root)
        win.title("Remove a map pack")
        win.transient(self.root)
        lb = tk.Listbox(win, height=min(len(items), 8), width=60, exportselection=False)
        for a in items:
            lb.insert("end", f"{a.name}  ({', '.join(a.maps)})")
        theme.recolor(lb)
        lb.pack(fill="both", expand=True, padx=px(12), pady=(px(12), px(6)))
        bf = ttk.Frame(win)
        bf.pack(fill="x", padx=px(12), pady=(0, px(12)))

        def go():
            sel = lb.curselection()
            if not sel:
                return
            a = items[sel[0]]
            win.destroy()

            def work():
                addons.remove(out, a.id, self.log)
                self.q.put(("call", self._refresh_addons))
                self.q.put(("info", "Removed", f"Removed the {a.name}."))
            self._bg(work, f"Removing the {a.name}...")
        ttk.Button(bf, text="Remove", command=go).pack(side="right")
        ttk.Button(bf, text="Cancel", command=win.destroy).pack(side="right", padx=(0, px(6)))

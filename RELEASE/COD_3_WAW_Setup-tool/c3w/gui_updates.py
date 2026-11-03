"""The setup tool's update checks in the window (c3w/update.py does the
work): the question at the first start, the check at start (only when the
player allowed it), the "new version" note, the approval before anything is
downloaded, the restart that puts it in place, and the About page's Updates
section.

A mixin for gui.App, like gui_pages.Pages: it uses the App's root, worker,
banner and dialog helpers.
"""
from __future__ import annotations

import os
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import ttk
from typing import Callable, Optional

from . import update, widgets
from .widgets import hint, px, section

ASK_TEXT = ("Check for new versions of the setup tool?\n"
            "When you allow it, the setup tool looks at the project's page on GitHub each time it starts "
            "(raw.githubusercontent.com, which sees your IP address like any download). When there is a new "
            "version it tells you, and downloads it only if you say so.\n\n"
            "An update is used only if it carries the project's signature, and every file is checked before "
            "anything changes; the files it replaces are kept, so it can be undone. You can change this on the "
            "About page at any time.")


class Updates:
    """Mixin for gui.App."""

    # ------------------------------------------------------------- at start
    def _updates_on_start(self) -> None:
        done = os.environ.pop("C3W_UPDATED", "")
        if done:
            self._show_banner("good" if "updated" in done else "warn", "Setup tool update", done, [])
            self.log.info(done)
        if update.why_not_here():
            return   # a launcher copy, no release key yet, or not a signed release: nothing to ask
        allowed = update.checks_allowed()
        if allowed is None:
            allowed = widgets.ask_yes_no(self.root, "Update checks", ASK_TEXT, yes="Allow update checks",
                                         no="Don't check")
            update.set_checks_allowed(allowed)
            self.log.info("Update checks " + ("allowed." if allowed else "turned off (About page to change)."))
            if hasattr(self, "v_update_checks"):
                self.v_update_checks.set(allowed)
        if allowed and not update.pending_version():
            self._check_updates(manual=False)

    # ------------------------------------------------------------- checking
    def _in_thread(self, work: Callable[[], object], done: Callable[[object, Optional[Exception]], None]) -> None:
        """Runs `work` off the UI thread; `done(result, error)` runs on it."""
        box: dict = {}

        def run() -> None:
            try:
                box["result"] = work()
            except Exception as e:   # shown to the player, never a crash
                box["error"] = e
        t = threading.Thread(target=run, daemon=True)
        t.start()

        def poll() -> None:
            if t.is_alive():
                self.root.after(200, poll)
            else:
                done(box.get("result"), box.get("error"))
        self.root.after(200, poll)

    def _check_updates(self, manual: bool) -> None:
        if manual:
            self._set_update_status("Checking...")

        def done(result, error) -> None:
            if error is not None:
                text = f"No update was used: {error}." if isinstance(error, update.UpdateError) else \
                    f"The check didn't work: {error}"
                self.log.warn(f"Update check: {text}")
                self._set_update_status(text)
                if manual or (isinstance(error, update.UpdateError) and "signature" in str(error)):
                    self._info("Update check", text, kind="warning")
                return
            plan, message = result
            if plan is None:
                self._set_update_status(message)
                if manual:
                    self._info("Update check", message)
                return
            self._update_plan = plan
            self._set_update_status(f"Version {plan.release.version_text} is available: {plan.summary}.")
            notes = plan.release.notes.strip()
            self._show_banner("good", f"Setup tool {plan.release.version_text} is available",
                              (notes[:300] + ("..." if len(notes) > 300 else "")) if notes else plan.summary,
                              [("Update", self._update_now), ("What's new", self._show_release_notes)])
        self._in_thread(lambda: update.check(), done)

    def _show_release_notes(self) -> None:
        plan = getattr(self, "_update_plan", None)
        if plan:
            self._info(f"Setup tool {plan.release.version_text}",
                       f"What's new in {plan.release.version_text}\n{plan.release.notes or 'No notes.'}")

    # ------------------------------------------------------------- updating
    def _update_now(self) -> None:
        plan = getattr(self, "_update_plan", None)
        if plan is None:
            return
        if self.worker and self.worker.is_alive():
            self._info("Update", "A job is running. Update when it has finished.", kind="warning")
            return
        root = update.release_root()
        rows = [(f"Setup tool {plan.release.version_text} (now {plan.installed.version_text}): {plan.summary}",
                 f"github.com/{update.load_keys(root).repo}, tag {plan.release.ref}",
                 f"Downloaded into {update.update_dir() / 'pending'}, then into {root} at the next start")]
        if plan.edited:
            rows.append((f"{len(plan.edited)} of those are files you changed in this folder", "",
                         f"Your copies are kept in {update.update_dir() / 'previous'}"))
        self._table_window("Approve the update",
                           "This update will be downloaded. Nothing changes until you approve.",
                           ["What", "From", "Goes into"], rows,
                           "Each file is checked against the release's signed list before it's used; the files it "
                           "replaces are kept, so About > Undo the last update goes back. Your discs, games, "
                           "settings and work folder aren't touched.",
                           [("Download the update", self._download_update), ("Cancel", None)],
                           shares=(0.44, 0.22, 0.34))

    def _download_update(self) -> None:
        plan = self._update_plan
        self._set_update_status(f"Downloading {plan.release.version_text}...")
        self.v_status.set(f"Downloading the setup tool {plan.release.version_text}...")

        def done(result, error) -> None:
            self.v_status.set("Ready.")
            if error is not None:
                self._set_update_status(f"The update wasn't downloaded: {error}")
                self._info("Update", f"The update wasn't downloaded.\n{error}", kind="error")
                return
            self._set_update_status(f"{plan.release.version_text} is downloaded; it's put in place at the next start.")
            widgets.message(self.root, "Update",
                            f"Setup tool {plan.release.version_text} is downloaded.\n"
                            "It's put in place when the setup tool starts again.",
                            kind="done", buttons=(("Restart now", self._restart_for_update), ("Later", None)))
        self._in_thread(lambda: update.download(plan), done)

    def _restart_for_update(self) -> None:
        """Starts the setup tool again (it applies the update first) and closes this one."""
        if self.worker and self.worker.is_alive():
            self._info("Update", "A job is running. Restart when it has finished.", kind="warning")
            return
        try:
            self._save()
        except OSError:
            pass
        script = Path(__file__).resolve().parent.parent / "cod3_waw_setup.py"
        args = [sys.executable, str(script)] + [a for a in sys.argv[1:] if a == "--launcher"]
        try:
            subprocess.Popen(args, close_fds=True)
        except OSError as e:
            self._info("Update", f"Couldn't start the setup tool again ({e}). Close it and start it yourself.",
                       kind="error")
            return
        self.root.destroy()

    # ------------------------------------------------------------- About page
    def _set_update_status(self, text: str) -> None:
        if hasattr(self, "v_update_status"):
            self.v_update_status.set(text)

    def _about_updates(self, t, row: int) -> None:
        """The About page's Updates section."""
        u = section(t, "Updates", row, columns=1)
        u.columnconfigure(1, weight=1)
        reason = update.why_not_here()
        self.v_update_status = tk.StringVar(value=reason or (
            f"{update.pending_version()} is downloaded; it's put in place at the next start."
            if update.pending_version() else "Not checked yet."))
        if reason:
            hint(u, reason).grid(row=0, column=0, columnspan=2, sticky="w")
            if update.can_undo():   # an update that left this copy unable to check can still be undone
                ttk.Button(u, text="Undo the last update", width=20, command=self._undo_update).grid(
                    row=1, column=0, sticky="w", pady=px(2))
                hint(u, f"Goes back to {update.can_undo()}.").grid(row=1, column=1, sticky="w", padx=(px(12), 0))
            return
        self.v_update_checks = tk.BooleanVar(value=bool(update.checks_allowed()))

        def toggled() -> None:
            update.set_checks_allowed(self.v_update_checks.get())
        ttk.Checkbutton(u, text="Check for updates when the setup tool starts", variable=self.v_update_checks,
                        command=toggled).grid(row=0, column=0, columnspan=2, sticky="w")
        ttk.Button(u, text="Check now", width=20, command=lambda: self._check_updates(manual=True)).grid(
            row=1, column=0, sticky="w", pady=px(4))
        hint(u, var=self.v_update_status).grid(row=1, column=1, sticky="w", padx=(px(12), 0))
        undo_to = update.can_undo()
        b = ttk.Button(u, text="Undo the last update", width=20, command=self._undo_update)
        b.grid(row=2, column=0, sticky="w", pady=px(2))
        if not undo_to:
            b.configure(state="disabled")
        hint(u, f"Goes back to {undo_to}." if undo_to else "Nothing to undo.").grid(
            row=2, column=1, sticky="w", padx=(px(12), 0))
        hint(u, "Updates come only from the project's GitHub page and are used only with the project's signature "
                "(docs/UPDATES.md). Nothing is checked or downloaded unless you allow it.").grid(
            row=3, column=0, columnspan=2, sticky="w", pady=(px(4), 0))

    def _undo_update(self) -> None:
        if self.worker and self.worker.is_alive():
            self._info("Undo the update", "A job is running. Undo when it has finished.", kind="warning")
            return
        to = update.can_undo()
        if not to or not widgets.ask_yes_no(self.root, "Undo the last update",
                                            f"Go back to {to}?\nThe setup tool starts again afterwards.",
                                            yes="Undo the update", no="Keep it"):
            return
        try:
            text = update.undo_last()
        except (update.UpdateError, OSError, ValueError) as e:
            self._info("Undo the update", f"It couldn't be undone: {e}", kind="error")
            return
        self.log.info(text)
        os.environ["C3W_UPDATED"] = text
        self._restart_for_update()

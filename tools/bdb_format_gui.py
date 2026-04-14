#!/usr/bin/env python3
"""
VBOX BDB → verschlüsseltes .tbrl (GUI)
======================================
Liest VBOX Motorsport Tracks.BDB ein und exportiert alle Tracks als
ein einzelnes AES-256-CBC-verschlüsseltes .tbrl Bundle
(gleicher Schlüssel wie .brl-Fahrzeugprofile).

Starten:
    python tools/bdb_format_gui.py
"""

import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox

# bdb_format.py muss importierbar sein (auch als PyInstaller-EXE)
SCRIPT_DIR = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
sys.path.insert(0, str(SCRIPT_DIR))

from bdb_format import (  # noqa: E402
    database_to_tbrl_bundle,
    encrypt_tbrl,
    parse_bdb,
)

APP_VERSION = "1.0.0"

# BRL-Blau Farbschema
COL_BG     = "#000000"
COL_PANEL  = "#0D1117"
COL_BORDER = "#1C3A5C"
COL_BLUE   = "#0096FF"
COL_WHITE  = "#FFFFFF"
COL_GRAY   = "#7A8FA6"
COL_GREEN  = "#00CC66"
COL_RED    = "#FF3B30"


class BdbApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title(f"BRL VBOX-Track-Konverter v{APP_VERSION}")
        self.configure(bg=COL_BG)
        self.geometry("680x360")
        self.minsize(640, 320)
        self.resizable(True, True)

        self.db = None
        self.current_file: Path | None = None
        self.lang = tk.StringVar(value="de")

        self._build_ui()

    # ─────────────────────────────────────────────────────────────────────
    def _build_ui(self) -> None:
        header = tk.Label(
            self, text="VBOX-Track-Konverter",
            bg=COL_BG, fg=COL_BLUE,
            font=("Segoe UI", 20, "bold"))
        header.pack(side="top", pady=(18, 2))

        sub = tk.Label(
            self,
            text="Tracks.BDB öffnen → verschlüsseltes .tbrl-Bundle "
                 "für den Laptimer speichern",
            bg=COL_BG, fg=COL_GRAY, font=("Segoe UI", 10))
        sub.pack(side="top", pady=(0, 18))

        # Sprache
        lang_row = tk.Frame(self, bg=COL_BG)
        lang_row.pack(side="top", pady=(0, 12))
        tk.Label(lang_row, text="Sprache Ländernamen:",
                 bg=COL_BG, fg=COL_WHITE,
                 font=("Segoe UI", 10)).pack(side="left", padx=(0, 8))
        for val, lbl in (("de", "Deutsch"), ("en", "English")):
            tk.Radiobutton(
                lang_row, text=lbl, value=val, variable=self.lang,
                bg=COL_BG, fg=COL_WHITE, activebackground=COL_BG,
                selectcolor=COL_PANEL, font=("Segoe UI", 10)
            ).pack(side="left", padx=4)

        # Buttons
        btn_row = tk.Frame(self, bg=COL_BG)
        btn_row.pack(side="top", pady=6)

        self.btn_open = tk.Button(
            btn_row, text="1. Tracks.BDB öffnen…",
            bg=COL_PANEL, fg=COL_WHITE,
            activebackground=COL_BORDER, activeforeground=COL_WHITE,
            relief="flat", padx=18, pady=10,
            font=("Segoe UI", 11, "bold"),
            command=self._open_bdb)
        self.btn_open.pack(side="left", padx=6)

        self.btn_export = tk.Button(
            btn_row, text="2. Alle verschlüsselt exportieren…",
            bg=COL_BLUE, fg="#000000",
            activebackground=COL_BLUE, activeforeground="#000000",
            relief="flat", padx=18, pady=10,
            font=("Segoe UI", 11, "bold"),
            state="disabled",
            command=self._export_all_encrypted)
        self.btn_export.pack(side="left", padx=6)

        # Info-Panel
        self.info = tk.Label(
            self, text="Noch keine Datei geladen.",
            bg=COL_BG, fg=COL_GRAY,
            font=("Consolas", 10), justify="center")
        self.info.pack(side="top", pady=(20, 6))

        # Status bar
        self.status = tk.Label(
            self, text=f"Bereit  ·  v{APP_VERSION}",
            bg=COL_PANEL, fg=COL_GRAY, anchor="w",
            font=("Segoe UI", 9), padx=10, pady=4)
        self.status.pack(side="bottom", fill="x")

    # ─────────────────────────────────────────────────────────────────────
    def _set_status(self, text: str, color: str = COL_GRAY) -> None:
        self.status.configure(text=text, fg=color)

    def _open_bdb(self) -> None:
        path = filedialog.askopenfilename(
            title="Tracks.BDB öffnen",
            filetypes=[("VBOX-Datenbank", "*.BDB *.bdb"), ("Alle", "*.*")])
        if not path:
            return
        self.current_file = Path(path)
        self._set_status(f"Lade {self.current_file.name}…", COL_BLUE)
        self.btn_export.configure(state="disabled")
        self.update_idletasks()
        try:
            self.db = parse_bdb(self.current_file)
        except Exception as exc:
            messagebox.showerror("Parse-Fehler",
                                 f"{self.current_file}\n\n{exc}")
            self._set_status("Fehler beim Laden.", COL_RED)
            self.info.configure(text="Noch keine Datei geladen.",
                                fg=COL_GRAY)
            return

        self.info.configure(
            text=(f"{self.current_file.name}\n"
                  f"Datum: {self.db.date}   ·   "
                  f"{len(self.db.groups)} Länder   ·   "
                  f"{self.db.track_count} Tracks"),
            fg=COL_WHITE)
        self._set_status(
            f"Geladen: {self.db.track_count} Tracks aus "
            f"{len(self.db.groups)} Ländern. Bereit zum Export.",
            COL_GREEN)
        self.btn_export.configure(state="normal")

    # ─────────────────────────────────────────────────────────────────────
    def _export_all_encrypted(self) -> None:
        if not self.db:
            return
        default_name = "Tracks.tbrl"
        out = filedialog.asksaveasfilename(
            title="Verschlüsseltes Bundle speichern",
            defaultextension=".tbrl",
            initialfile=default_name,
            filetypes=[("BRL verschlüsselte Track-Datenbank", "*.tbrl"),
                       ("Alle", "*.*")])
        if not out:
            return

        self._set_status("Exportiere alle Tracks verschlüsselt…", COL_BLUE)
        self.btn_export.configure(state="disabled")
        self.btn_open.configure(state="disabled")
        self.update_idletasks()
        threading.Thread(
            target=self._do_export,
            args=(Path(out),),
            daemon=True).start()

    def _do_export(self, out_path: Path) -> None:
        try:
            plain = database_to_tbrl_bundle(
                self.db, filter_groups=None,
                language=self.lang.get())
            blob = encrypt_tbrl(plain)
            out_path.write_bytes(blob)
            msg = (f"Fertig!\n\n{out_path}\n"
                   f"{self.db.track_count} Tracks, "
                   f"{len(blob)} Byte verschlüsselt "
                   f"(AES-256-CBC)")
            self.after(0, lambda: messagebox.showinfo("Export fertig", msg))
            self.after(0, lambda: self._set_status(
                f"Export fertig: {self.db.track_count} Tracks → "
                f"{out_path.name}", COL_GREEN))
        except Exception as exc:
            m = str(exc)
            self.after(0, lambda: messagebox.showerror("Fehler", m))
            self.after(0, lambda: self._set_status(
                "Fehler beim Export.", COL_RED))
        finally:
            self.after(0, lambda: self.btn_export.configure(state="normal"))
            self.after(0, lambda: self.btn_open.configure(state="normal"))


def main() -> int:
    BdbApp().mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

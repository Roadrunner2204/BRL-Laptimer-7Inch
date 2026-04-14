#!/usr/bin/env python3
"""
VBOX BDB → BRL Track-Konverter (GUI)
====================================
Liest VBOX Motorsport Tracks.BDB ein, zeigt Länder + Tracks,
exportiert ausgewählte als .tbrl / .t-brl im BRL-Laptimer-Format.

Starten:
    python tools/bdb_format_gui.py
"""

import os
import sys
import json
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

# Sicherstellen, dass bdb_format.py importierbar ist (auch in PyInstaller-EXE)
SCRIPT_DIR = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
sys.path.insert(0, str(SCRIPT_DIR))

from bdb_format import (  # noqa: E402
    country_name,
    database_to_tbrl_bundle,
    parse_bdb,
    sanitize_filename,
)

APP_VERSION = "1.0.0"

# BRL-Blau Farbschema (passt zu den anderen Tools)
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
        self.geometry("960x640")
        self.minsize(820, 540)

        self.db = None
        self.current_file: Path | None = None
        self.lang = tk.StringVar(value="de")
        self.ext = tk.StringVar(value="tbrl")
        self.bundle = tk.BooleanVar(value=False)

        self._build_ui()

    # ─────────────────────────────────────────────────────────────────────
    def _build_ui(self) -> None:
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("Treeview",
                        background=COL_PANEL, foreground=COL_WHITE,
                        fieldbackground=COL_PANEL, bordercolor=COL_BORDER,
                        rowheight=22)
        style.map("Treeview",
                  background=[("selected", COL_BLUE)],
                  foreground=[("selected", "#000000")])
        style.configure("Treeview.Heading",
                        background=COL_BORDER, foreground=COL_WHITE,
                        relief="flat")

        # Top-bar
        bar = tk.Frame(self, bg=COL_BG)
        bar.pack(side="top", fill="x", padx=10, pady=(10, 4))

        tk.Button(bar, text="Tracks.BDB öffnen…",
                  bg=COL_BLUE, fg="#000000", activebackground=COL_BLUE,
                  relief="flat", padx=14, pady=6, font=("Segoe UI", 10, "bold"),
                  command=self._open_bdb).pack(side="left")

        tk.Label(bar, text="  Sprache:", bg=COL_BG, fg=COL_GRAY,
                 font=("Segoe UI", 9)).pack(side="left")
        for val, lbl in (("de", "DE"), ("en", "EN")):
            tk.Radiobutton(bar, text=lbl, value=val, variable=self.lang,
                           bg=COL_BG, fg=COL_WHITE, activebackground=COL_BG,
                           selectcolor=COL_PANEL,
                           command=self._refresh_tree).pack(side="left")

        tk.Label(bar, text="  Format:", bg=COL_BG, fg=COL_GRAY,
                 font=("Segoe UI", 9)).pack(side="left")
        for val in ("tbrl", "t-brl"):
            tk.Radiobutton(bar, text=f".{val}", value=val, variable=self.ext,
                           bg=COL_BG, fg=COL_WHITE, activebackground=COL_BG,
                           selectcolor=COL_PANEL).pack(side="left")

        tk.Checkbutton(bar, text="Als Bundle", variable=self.bundle,
                       bg=COL_BG, fg=COL_WHITE, activebackground=COL_BG,
                       selectcolor=COL_PANEL).pack(side="left", padx=(12, 0))

        tk.Button(bar, text="Auswahl exportieren…",
                  bg=COL_PANEL, fg=COL_WHITE, activebackground=COL_BORDER,
                  relief="flat", padx=14, pady=6, font=("Segoe UI", 10),
                  command=self._export_selection).pack(side="right")

        # Summary + tree
        self.summary = tk.Label(
            self, text="Keine Datei geladen.",
            bg=COL_BG, fg=COL_GRAY, anchor="w",
            font=("Consolas", 10))
        self.summary.pack(side="top", fill="x", padx=10, pady=(2, 6))

        body = tk.Frame(self, bg=COL_BG)
        body.pack(side="top", fill="both", expand=True, padx=10, pady=(0, 6))

        self.tree = ttk.Treeview(
            body, columns=("ntracks", "coord", "sectors"),
            show="tree headings", selectmode="extended")
        self.tree.heading("#0", text="Land / Track")
        self.tree.heading("ntracks", text="Tracks")
        self.tree.heading("coord", text="Lat, Lon")
        self.tree.heading("sectors", text="Sektoren")
        self.tree.column("#0", width=400)
        self.tree.column("ntracks", width=70, anchor="e")
        self.tree.column("coord", width=200, anchor="w")
        self.tree.column("sectors", width=70, anchor="e")

        vsb = ttk.Scrollbar(body, orient="vertical",
                            command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        vsb.pack(side="left", fill="y")

        # Status bar
        self.status = tk.Label(
            self, text=f"Bereit  ·  v{APP_VERSION}",
            bg=COL_PANEL, fg=COL_GRAY, anchor="w",
            font=("Segoe UI", 9), padx=10, pady=3)
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
        self.update_idletasks()
        try:
            self.db = parse_bdb(self.current_file)
        except Exception as exc:
            messagebox.showerror("Parse-Fehler",
                                 f"{self.current_file}\n\n{exc}")
            self._set_status("Fehler beim Laden.", COL_RED)
            return
        self._refresh_tree()

    def _refresh_tree(self) -> None:
        for item in self.tree.get_children():
            self.tree.delete(item)
        if not self.db:
            self.summary.configure(text="Keine Datei geladen.")
            return

        lang = self.lang.get()
        for g in self.db.groups:
            cn = country_name(g.index, lang)
            gid = self.tree.insert(
                "", "end",
                text=f"{cn}  (Gruppe {g.index})",
                values=(len(g.tracks), "", ""),
                open=False,
                iid=f"g{g.index}")
            for t in g.tracks:
                lat, lon = t.center_lat, t.center_lon
                self.tree.insert(
                    gid, "end",
                    text=t.name,
                    values=("", f"{lat:+9.5f}, {lon:+10.5f}",
                            str(len(t.sectors))),
                    iid=f"t{t.offset:x}")

        self.summary.configure(
            text=(f"{self.current_file.name}  ·  "
                  f"Datum {self.db.date}  ·  "
                  f"{len(self.db.groups)} Länder  ·  "
                  f"{self.db.track_count} Tracks gesamt"))
        self._set_status(
            f"Geladen: {self.db.track_count} Tracks aus "
            f"{len(self.db.groups)} Ländern.", COL_GREEN)

    # ─────────────────────────────────────────────────────────────────────
    def _export_selection(self) -> None:
        if not self.db:
            messagebox.showinfo("Hinweis", "Zuerst eine .BDB-Datei öffnen.")
            return

        # Selektion auswerten: Gruppen → alle Tracks, einzelne Tracks → diese
        sel_groups: set[str] = set()
        sel_track_offsets: set[int] = set()
        for iid in self.tree.selection():
            if iid.startswith("g"):
                sel_groups.add(iid[1:])
            elif iid.startswith("t"):
                sel_track_offsets.add(int(iid[1:], 16))

        if not sel_groups and not sel_track_offsets:
            if not messagebox.askyesno(
                    "Alles exportieren?",
                    "Keine Auswahl aktiv — möchtest du ALLE Tracks aus "
                    "ALLEN Ländern exportieren?"):
                return

        out_dir = filedialog.askdirectory(title="Zielverzeichnis wählen")
        if not out_dir:
            return
        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)

        self._set_status("Exportiere…", COL_BLUE)
        self.update_idletasks()
        threading.Thread(
            target=self._do_export,
            args=(out, sel_groups, sel_track_offsets),
            daemon=True).start()

    def _do_export(self, out_dir: Path,
                   sel_groups: set[str],
                   sel_track_offsets: set[int]) -> None:
        db = self.db
        lang = self.lang.get()
        ext = self.ext.get()

        def want(t) -> bool:
            if not sel_groups and not sel_track_offsets:
                return True
            if str(t.group) in sel_groups:
                return True
            return t.offset in sel_track_offsets

        selected = [t for t in db.tracks if want(t)]
        try:
            if self.bundle.get():
                fg = sel_groups if sel_groups and not sel_track_offsets else None
                blob = database_to_tbrl_bundle(
                    db, filter_groups=fg, language=lang)
                path = out_dir / f"Tracks.{ext}"
                path.write_bytes(blob)
                self.after(0, lambda: messagebox.showinfo(
                    "Fertig", f"Bundle gespeichert:\n{path}\n"
                              f"({len(selected)} Tracks)"))
            else:
                for t in selected:
                    name = sanitize_filename(t.name) or f"track_{t.offset:x}"
                    (out_dir / f"{name}.{ext}").write_bytes(json.dumps(
                        t.to_tbrl_dict(country_name=country_name(t.group, lang)),
                        ensure_ascii=False, indent=2).encode("utf-8"))
                self.after(0, lambda: messagebox.showinfo(
                    "Fertig",
                    f"{len(selected)} Dateien exportiert nach:\n{out_dir}"))
            self.after(0, lambda: self._set_status(
                f"Export fertig: {len(selected)} Tracks.", COL_GREEN))
        except Exception as exc:
            msg = str(exc)
            self.after(0, lambda: messagebox.showerror("Fehler", msg))
            self.after(0, lambda: self._set_status("Fehler beim Export.", COL_RED))


def main() -> int:
    app = BdbApp()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

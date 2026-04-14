#!/usr/bin/env python3
"""
BRL Fahrzeugprofil-Tool (GUI)
==============================
BMW-Display-Farbschema, Tkinter.
Konvertiert CAN Checked .TRX → verschlüsseltes .brl
und zeigt/dekodiert bestehende .brl Dateien.

Voraussetzungen:
  pip install pycryptodome

Starten:
  python3 brl_format_gui.py
"""

import sys, os, json, struct, zlib, threading, tempfile, subprocess
import webbrowser, urllib.request
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from pathlib import Path

# ── Version & Update ─────────────────────────────────────────────────────────
APP_VERSION = "1.0.0"
GITHUB_REPO = "roadrunner2204/bmw-data-display-v0.1"
SCRIPT_DIR  = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent))

# ── BMW Display Farbschema ───────────────────────────────────────────────────
COL_BG      = "#000000"
COL_PANEL   = "#1A1A1A"
COL_BORDER  = "#2C2C2E"
COL_BLUE    = "#1C69D4"
COL_BLUE_DK = "#0D2B5A"
COL_WHITE   = "#FFFFFF"
COL_GRAY    = "#AEAEB2"
COL_DARK    = "#636366"
COL_GREEN   = "#30D158"
COL_AMBER   = "#FF9F0A"
COL_RED     = "#FF453A"

# ── BRL Krypto-Konstanten ───────────────────────────────────────────────────
BRL_KEY     = bytes.fromhex(
    "4d358275a7ea02f81df7c5689c073f8edbd464066f563717e6cbaf60addb710a"
)
BRL_MAGIC   = b"BRL\x01"
HEADER_SIZE = 32

TRX_KEY_AES = bytes.fromhex(
    "3373357638792f423f4528482b4d6251655468576d5a7134743777397a244326"
)
RECORD_SIZE      = 192
SENSOR_TEXT_SIZE = 96
FIELD_NAMES = [
    "proto","can_id","fmt","start","len","unsigned","shift","mask",
    "decimals","name","scale","offset","mapper_type","mapper1","mapper2",
    "mapper3","mapper4","ain","min","max","ref_sensor","ref_val",
    "unused1","popup","unused2","type",
]


# ── BRL Krypto-Funktionen ────────────────────────────────────────────────────

def _pad(data: bytes) -> bytes:
    pad = 16 - (len(data) % 16)
    return data + bytes([pad] * pad)

def _unpad(data: bytes) -> bytes:
    pad = data[-1]
    if pad < 1 or pad > 16:
        raise ValueError(f"Ungültiges PKCS7-Padding: {pad}")
    return data[:-pad]

def _brl_encrypt(plaintext: bytes):
    from Crypto.Cipher import AES
    iv = os.urandom(16)
    cipher = AES.new(BRL_KEY, AES.MODE_CBC, iv)
    return iv, cipher.encrypt(_pad(plaintext))

def _brl_decrypt(iv: bytes, ciphertext: bytes) -> bytes:
    from Crypto.Cipher import AES
    cipher = AES.new(BRL_KEY, AES.MODE_CBC, iv)
    return _unpad(cipher.decrypt(ciphertext))

def create_brl(vehicle: dict, sensors: list) -> bytes:
    payload = {
        "v": 1,
        "make":      vehicle.get("make", ""),
        "model":     vehicle.get("model", ""),
        "engine":    vehicle.get("engine", ""),
        "name":      vehicle.get("name", ""),
        "year_from": int(vehicle.get("year_from", 0)),
        "year_to":   int(vehicle.get("year_to", 9999)),
        "can":       vehicle.get("can", "PT-CAN"),
        "bitrate":   int(vehicle.get("bitrate", 500)),
        "sensors":   sensors,
    }
    json_bytes = json.dumps(payload, ensure_ascii=False, separators=(",",":")).encode("utf-8")
    iv, encrypted = _brl_encrypt(json_bytes)
    crc = zlib.crc32(encrypted) & 0xFFFFFFFF
    header  = BRL_MAGIC
    header += bytes([1, 0, 0, 0])
    header += iv
    header += struct.pack("<I", len(encrypted))
    header += struct.pack("<I", crc)
    return header + encrypted

def parse_brl(data: bytes):
    if len(data) < HEADER_SIZE:
        raise ValueError("Datei zu kurz")
    if data[:4] != BRL_MAGIC:
        raise ValueError(f"Kein BRL-Magic: {data[:4]!r}")
    iv           = data[8:24]
    payload_size = struct.unpack_from("<I", data, 24)[0]
    expected_crc = struct.unpack_from("<I", data, 28)[0]
    encrypted    = data[HEADER_SIZE: HEADER_SIZE + payload_size]
    if len(encrypted) != payload_size:
        raise ValueError("Unvollständige Payload")
    actual_crc = zlib.crc32(encrypted) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise ValueError(f"CRC32-Fehler: {expected_crc:08X} erwartet, {actual_crc:08X} erhalten")
    json_bytes = _brl_decrypt(iv, encrypted)
    payload    = json.loads(json_bytes.decode("utf-8"))
    vehicle = {k: payload.get(k, "") for k in
               ["make","model","engine","name","year_from","year_to","can","bitrate"]}
    return vehicle, payload.get("sensors", [])

def decrypt_trx(trx_path: Path):
    from Crypto.Cipher import AES
    data    = trx_path.read_bytes()
    hdr_end = data.find(b"\r\n")
    if hdr_end != -1:
        hdr_end += 2
    else:
        hdr_end = data.find(b"\n") + 1
    body  = data[hdr_end:]
    if len(body) % 16 != 0:
        raise ValueError("TRX-Körperlänge nicht durch 16 teilbar")
    plain = AES.new(TRX_KEY_AES, AES.MODE_ECB).decrypt(body)
    sensors = []
    for i in range(len(plain) // RECORD_SIZE):
        block = plain[i * RECORD_SIZE : i * RECORD_SIZE + SENSOR_TEXT_SIZE]
        null  = block.find(b"\x00")
        text  = block[:null if null != -1 else SENSOR_TEXT_SIZE].decode("latin-1", errors="replace").rstrip(";")
        if not text or ";" not in text:
            continue
        parts = (text.split(";") + [""]*26)[:26]
        s = {FIELD_NAMES[j]: parts[j] for j in range(26)}
        if s.get("name","").lower() == "empty":
            continue
        s["slot"] = i
        sensors.append(s)
    return sensors


# ── Hauptanwendung ────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("BRL Fahrzeugprofil-Tool")
        self.configure(bg=COL_BG)
        self.resizable(True, True)
        self.minsize(720, 540)
        self._setup_style()

        # ── Kopfzeile ────────────────────────────────────────────────────────
        header = tk.Frame(self, bg=COL_PANEL,
                          highlightbackground=COL_BORDER, highlightthickness=1)
        header.pack(fill="x")

        logo_frame = tk.Frame(header, bg=COL_PANEL, padx=14, pady=8)
        logo_frame.pack(side="left")
        self._build_logo(logo_frame)

        title_frame = tk.Frame(header, bg=COL_PANEL)
        title_frame.pack(side="left", padx=(0, 20))
        tk.Label(title_frame, text="BRL Fahrzeugprofil-Tool",
                 bg=COL_PANEL, fg=COL_WHITE,
                 font=("Helvetica", 15, "bold")).pack(anchor="w")
        tk.Label(title_frame, text="TRX-Konvertierung & Profil-Viewer",
                 bg=COL_PANEL, fg=COL_DARK,
                 font=("Helvetica", 9)).pack(anchor="w")

        btn_upd = tk.Button(header, text="Update prüfen",
                            command=lambda: self._check_updates(silent=False),
                            bg=COL_PANEL, fg=COL_BLUE,
                            activebackground=COL_BORDER, activeforeground=COL_BLUE,
                            font=("Helvetica", 9), relief="flat",
                            padx=10, pady=6, cursor="hand2")
        btn_upd.pack(side="right", padx=10)

        # ── Tabs ──────────────────────────────────────────────────────────────
        nb = ttk.Notebook(self, style="BMW.TNotebook")
        nb.pack(fill="both", expand=True, padx=12, pady=(10, 0))

        tab_conv = tk.Frame(nb, bg=COL_BG)
        tab_view = tk.Frame(nb, bg=COL_BG)
        nb.add(tab_conv, text="  TRX → BRL Konvertieren  ")
        nb.add(tab_view, text="  BRL Anzeigen  ")

        self._build_convert_tab(tab_conv)
        self._build_view_tab(tab_view)

        # ── Fußzeile ─────────────────────────────────────────────────────────
        footer = tk.Frame(self, bg=COL_PANEL,
                          highlightbackground=COL_BORDER, highlightthickness=1)
        footer.pack(fill="x", pady=(8, 0))
        tk.Label(footer, text=f"v{APP_VERSION}",
                 bg=COL_PANEL, fg=COL_DARK,
                 font=("Helvetica", 8)).pack(side="right", padx=10, pady=4)

        self._check_updates(silent=True)
        self.after(100, self._check_deps)

    # ── Style ─────────────────────────────────────────────────────────────────
    def _setup_style(self):
        s = ttk.Style(self)
        s.theme_use("default")
        s.configure("BMW.TNotebook",
                    background=COL_BG, borderwidth=0)
        s.configure("BMW.TNotebook.Tab",
                    background=COL_PANEL, foreground=COL_GRAY,
                    padding=[12, 6], borderwidth=0,
                    font=("Helvetica", 10))
        s.map("BMW.TNotebook.Tab",
              background=[("selected", COL_BG), ("active", COL_BORDER)],
              foreground=[("selected", COL_WHITE)])
        s.configure("BMW.Horizontal.TProgressbar",
                    troughcolor=COL_BORDER, background=COL_BLUE,
                    borderwidth=0, thickness=6)

    # ── Logo ──────────────────────────────────────────────────────────────────
    def _build_logo(self, parent):
        logo_path = SCRIPT_DIR / "logo.png"
        if logo_path.exists():
            try:
                img = tk.PhotoImage(file=str(logo_path))
                w, h = img.width(), img.height()
                factor = max(1, max(w, h) // 56)
                if factor > 1:
                    img = img.subsample(factor, factor)
                self._logo_img = img
                tk.Label(parent, image=img, bg=COL_PANEL).pack()
                return
            except Exception:
                pass
        size = 52; r = size // 2; m = 2; ri = r - 5; cx = cy = r
        cv = tk.Canvas(parent, width=size, height=size,
                       bg=COL_PANEL, highlightthickness=0)
        cv.pack()
        cv.create_oval(m, m, size-m, size-m,
                       fill="#0D1B2A", outline=COL_BLUE, width=2)
        for start, col in [(90, COL_BLUE), (0, COL_WHITE),
                           (270, COL_BLUE), (180, COL_WHITE)]:
            cv.create_arc(cx-ri, cy-ri, cx+ri, cy+ri,
                          start=start, extent=90,
                          fill=col, outline="", style="pieslice")
        cv.create_line(cx, cy-ri, cx, cy+ri, fill="#0D1B2A", width=2)
        cv.create_line(cx-ri, cy, cx+ri, cy, fill="#0D1B2A", width=2)
        tk.Label(parent, text="BMW", bg=COL_PANEL, fg=COL_BLUE,
                 font=("Helvetica", 7, "bold")).pack()

    # ── Abhängigkeiten ────────────────────────────────────────────────────────
    def _check_deps(self):
        try:
            from Crypto.Cipher import AES  # noqa
        except ImportError:
            messagebox.showerror("Fehlende Pakete",
                "Bitte installiere:\n\n    pip install pycryptodome\n\nDann neu starten.")
            self.destroy()


    # ── Tab 1: TRX → BRL ─────────────────────────────────────────────────────
    def _build_convert_tab(self, parent):
        # TRX Eingabe
        row0 = tk.Frame(parent, bg=COL_BG)
        row0.pack(fill="x", padx=16, pady=(14, 0))
        tk.Label(row0, text="TRX-Datei:", bg=COL_BG, fg=COL_GRAY,
                 font=("Helvetica", 10), width=14, anchor="w").pack(side="left")
        self.var_trx = tk.StringVar()
        tk.Entry(row0, textvariable=self.var_trx,
                 bg=COL_PANEL, fg=COL_WHITE, insertbackground=COL_WHITE,
                 relief="flat", font=("Helvetica", 10),
                 highlightbackground=COL_BORDER, highlightthickness=1
                 ).pack(side="left", fill="x", expand=True, padx=(0, 8))
        tk.Button(row0, text="Durchsuchen…",
                  command=self._browse_trx,
                  bg=COL_BORDER, fg=COL_WHITE,
                  activebackground=COL_DARK, activeforeground=COL_WHITE,
                  font=("Helvetica", 9), relief="flat",
                  padx=10, pady=4, cursor="hand2").pack(side="left")

        # Trennlinie
        tk.Frame(parent, bg=COL_BORDER, height=1).pack(fill="x", padx=16, pady=10)

        # Fahrzeugdaten-Formular
        form = tk.Frame(parent, bg=COL_BG)
        form.pack(fill="x", padx=16)

        def lbl(row, col, text):
            tk.Label(form, text=text, bg=COL_BG, fg=COL_GRAY,
                     font=("Helvetica", 10), anchor="e", width=13
                     ).grid(row=row, column=col*2, sticky="e", padx=(0,6), pady=3)

        def entry(row, col, var, width=22):
            e = tk.Entry(form, textvariable=var,
                         bg=COL_PANEL, fg=COL_WHITE,
                         insertbackground=COL_WHITE,
                         relief="flat", font=("Helvetica", 10), width=width,
                         highlightbackground=COL_BORDER, highlightthickness=1)
            e.grid(row=row, column=col*2+1, sticky="w", padx=(0,20), pady=3)
            return e

        self.var_make     = tk.StringVar(value="BMW")
        self.var_model    = tk.StringVar()
        self.var_engine   = tk.StringVar()
        self.var_name     = tk.StringVar()
        self.var_year_f   = tk.StringVar(value="2000")
        self.var_year_t   = tk.StringVar(value="9999")
        self.var_can      = tk.StringVar(value="PT-CAN")
        self.var_bitrate  = tk.StringVar(value="500")

        lbl(0,0,"Hersteller:");    entry(0,0,self.var_make)
        lbl(0,1,"Modell:");        entry(0,1,self.var_model)
        lbl(1,0,"Motorcode *:");   entry(1,0,self.var_engine)
        lbl(1,1,"Bezeichnung *:"); entry(1,1,self.var_name, width=28)
        lbl(2,0,"Baujahr von:");   entry(2,0,self.var_year_f, width=8)
        lbl(2,1,"Baujahr bis:");   entry(2,1,self.var_year_t, width=8)

        lbl(3,0,"CAN-Bus:")
        om_can = ttk.OptionMenu(form, self.var_can,
                                "PT-CAN", "PT-CAN","K-CAN","F-CAN","LIN","MOST")
        om_can.configure(style="BMW.TMenubutton" if "BMW.TMenubutton" in ttk.Style().element_names() else "")
        om_can.grid(row=3, column=1, sticky="w", pady=3)

        lbl(3,1,"Bitrate (kbps):")
        om_br = ttk.OptionMenu(form, self.var_bitrate,
                               "500", "100","250","500","1000")
        om_br.grid(row=3, column=3, sticky="w", pady=3)

        # Ausgabe
        tk.Frame(parent, bg=COL_BORDER, height=1).pack(fill="x", padx=16, pady=10)
        row_out = tk.Frame(parent, bg=COL_BG)
        row_out.pack(fill="x", padx=16)
        tk.Label(row_out, text="Ausgabe (.brl):", bg=COL_BG, fg=COL_GRAY,
                 font=("Helvetica", 10), width=14, anchor="w").pack(side="left")
        self.var_out_brl = tk.StringVar()
        tk.Entry(row_out, textvariable=self.var_out_brl,
                 bg=COL_PANEL, fg=COL_WHITE, insertbackground=COL_WHITE,
                 relief="flat", font=("Helvetica", 10),
                 highlightbackground=COL_BORDER, highlightthickness=1
                 ).pack(side="left", fill="x", expand=True, padx=(0,8))
        tk.Button(row_out, text="Speichern als…",
                  command=self._browse_out_brl,
                  bg=COL_BORDER, fg=COL_WHITE,
                  activebackground=COL_DARK, activeforeground=COL_WHITE,
                  font=("Helvetica", 9), relief="flat",
                  padx=10, pady=4, cursor="hand2").pack(side="left")

        # Konvertieren-Button
        row_btn = tk.Frame(parent, bg=COL_BG)
        row_btn.pack(fill="x", padx=16, pady=12)
        self.btn_conv = tk.Button(row_btn, text="  KONVERTIEREN  TRX → BRL",
                                  command=self._do_convert,
                                  bg=COL_BLUE, fg=COL_WHITE,
                                  activebackground=COL_BLUE_DK,
                                  activeforeground=COL_WHITE,
                                  font=("Helvetica", 12, "bold"),
                                  relief="flat", padx=22, pady=12,
                                  cursor="hand2")
        self.btn_conv.pack(side="left")

        # Status
        self.lbl_conv_status = tk.Label(parent, text="",
                                        bg=COL_BG, fg=COL_GRAY,
                                        font=("Helvetica", 10), anchor="w")
        self.lbl_conv_status.pack(fill="x", padx=16, pady=(0,8))

    def _browse_trx(self):
        path = filedialog.askopenfilename(
            title="TRX-Datei auswählen",
            filetypes=[("CAN Checked Profile", "*.TRX *.trx"), ("Alle Dateien", "*.*")])
        if path:
            self.var_trx.set(path)
            if not self.var_out_brl.get():
                self.var_out_brl.set(str(Path(path).with_suffix(".brl")))

    def _browse_out_brl(self):
        path = filedialog.asksaveasfilename(
            title="BRL-Datei speichern",
            defaultextension=".brl",
            filetypes=[("BRL Profile", "*.brl"), ("Alle Dateien", "*.*")],
            initialfile=Path(self.var_trx.get()).stem + ".brl" if self.var_trx.get() else "profil.brl")
        if path:
            self.var_out_brl.set(path)

    def _do_convert(self):
        trx = self.var_trx.get().strip()
        out = self.var_out_brl.get().strip()
        engine = self.var_engine.get().strip()
        name   = self.var_name.get().strip()
        if not trx:
            messagebox.showwarning("Keine Eingabe", "Bitte eine TRX-Datei auswählen.")
            return
        if not engine:
            messagebox.showwarning("Motor fehlt", "Bitte Motorcode eingeben (z.B. N47D20).")
            return
        if not name:
            messagebox.showwarning("Name fehlt", "Bitte Bezeichnung eingeben (z.B. N47 2.0d 143PS).")
            return
        if not out:
            messagebox.showwarning("Kein Ziel", "Bitte Ausgabedatei angeben.")
            return
        self.btn_conv.config(state="disabled", text="Konvertiere…")
        self.lbl_conv_status.config(text="TRX entschlüsseln…", fg=COL_GRAY)
        vehicle = {
            "make":      self.var_make.get().strip() or "BMW",
            "model":     self.var_model.get().strip(),
            "engine":    engine,
            "name":      name,
            "year_from": int(self.var_year_f.get() or 0),
            "year_to":   int(self.var_year_t.get() or 9999),
            "can":       self.var_can.get(),
            "bitrate":   int(self.var_bitrate.get() or 500),
        }
        threading.Thread(target=self._run_convert,
                         args=(Path(trx), Path(out), vehicle), daemon=True).start()

    def _run_convert(self, trx_path, out_path, vehicle):
        try:
            sensors = decrypt_trx(trx_path)
            self.after(0, self.lbl_conv_status.config,
                       {"text": f"{len(sensors)} Sensoren gelesen – verschlüssele…",
                        "fg": COL_GRAY})
            data = create_brl(vehicle, sensors)
            out_path.write_bytes(data)
            msg = (f"Fertig! {len(sensors)} Sensoren → {out_path.name}  "
                   f"({len(data)/1024:.1f} KB)")
            self.after(0, self._conv_done, True, msg, str(out_path))
        except Exception as e:
            self.after(0, self._conv_done, False, str(e), "")

    def _conv_done(self, ok, msg, path):
        self.btn_conv.config(state="normal",
                             text="  KONVERTIEREN  TRX → BRL")
        if ok:
            self.lbl_conv_status.config(text=msg, fg=COL_GREEN)
            if messagebox.askyesno("Fertig", f"{msg}\n\nOrdner öffnen?"):
                _open_folder(path)
        else:
            self.lbl_conv_status.config(text=f"Fehler: {msg}", fg=COL_RED)
            messagebox.showerror("Fehler", f"Konvertierung fehlgeschlagen:\n\n{msg}")


    # ── Tab 2: BRL Anzeigen ───────────────────────────────────────────────────
    def _build_view_tab(self, parent):
        row0 = tk.Frame(parent, bg=COL_BG)
        row0.pack(fill="x", padx=16, pady=(14, 0))
        tk.Label(row0, text="BRL-Datei:", bg=COL_BG, fg=COL_GRAY,
                 font=("Helvetica", 10), width=10, anchor="w").pack(side="left")
        self.var_brl = tk.StringVar()
        tk.Entry(row0, textvariable=self.var_brl,
                 bg=COL_PANEL, fg=COL_WHITE, insertbackground=COL_WHITE,
                 relief="flat", font=("Helvetica", 10),
                 highlightbackground=COL_BORDER, highlightthickness=1
                 ).pack(side="left", fill="x", expand=True, padx=(0,8))
        tk.Button(row0, text="Öffnen…",
                  command=self._browse_brl,
                  bg=COL_BORDER, fg=COL_WHITE,
                  activebackground=COL_DARK, activeforeground=COL_WHITE,
                  font=("Helvetica", 9), relief="flat",
                  padx=10, pady=4, cursor="hand2").pack(side="left")

        row_btns = tk.Frame(parent, bg=COL_BG)
        row_btns.pack(fill="x", padx=16, pady=10)
        tk.Button(row_btns, text="  INFO (Fahrzeugdaten)",
                  command=lambda: self._show_brl("info"),
                  bg=COL_BLUE, fg=COL_WHITE,
                  activebackground=COL_BLUE_DK, activeforeground=COL_WHITE,
                  font=("Helvetica", 10, "bold"),
                  relief="flat", padx=16, pady=8, cursor="hand2").pack(side="left", padx=(0,8))
        tk.Button(row_btns, text="  DUMP (JSON)",
                  command=lambda: self._show_brl("dump"),
                  bg=COL_PANEL, fg=COL_GRAY,
                  activebackground=COL_BORDER, activeforeground=COL_WHITE,
                  font=("Helvetica", 10),
                  relief="flat", padx=16, pady=8, cursor="hand2").pack(side="left")

        # Textausgabe
        out_frame = tk.Frame(parent, bg=COL_PANEL,
                             highlightbackground=COL_BORDER, highlightthickness=1)
        out_frame.pack(fill="both", expand=True, padx=16, pady=(0,14))
        self.txt_view = tk.Text(out_frame, bg=COL_PANEL, fg=COL_GRAY,
                                insertbackground=COL_WHITE,
                                font=("Consolas", 10), relief="flat",
                                padx=12, pady=10, state="disabled",
                                wrap="none")
        sb_v = tk.Scrollbar(out_frame, orient="vertical",
                            command=self.txt_view.yview)
        sb_h = tk.Scrollbar(out_frame, orient="horizontal",
                            command=self.txt_view.xview)
        self.txt_view.configure(yscrollcommand=sb_v.set,
                                xscrollcommand=sb_h.set)
        sb_v.pack(side="right", fill="y")
        sb_h.pack(side="bottom", fill="x")
        self.txt_view.pack(fill="both", expand=True)

    def _browse_brl(self):
        path = filedialog.askopenfilename(
            title="BRL-Datei auswählen",
            filetypes=[("BRL Profile", "*.brl"), ("Alle Dateien", "*.*")])
        if path:
            self.var_brl.set(path)

    def _show_brl(self, mode):
        path = self.var_brl.get().strip()
        if not path:
            messagebox.showwarning("Keine Datei", "Bitte eine BRL-Datei öffnen.")
            return
        try:
            data = Path(path).read_bytes()
            vehicle, sensors = parse_brl(data)
        except Exception as e:
            self._set_view_text(f"Fehler: {e}", COL_RED)
            return

        if mode == "info":
            lines = [
                f"  Datei:      {Path(path).name}",
                f"  Größe:      {len(data)} Bytes",
                "",
                f"  Hersteller: {vehicle['make']}",
                f"  Modell:     {vehicle['model']}",
                f"  Motor:      {vehicle['engine']}",
                f"  Bezeichnung:{vehicle['name']}",
                f"  Baujahr:    {vehicle['year_from']} – {vehicle['year_to']}",
                f"  CAN-Bus:    {vehicle['can']}  ({vehicle['bitrate']} kbps)",
                "",
                f"  Sensoren:   {len(sensors)} Einträge",
                "",
                "  ─────────────────────────────────────────────────────────",
            ]
            for i, s in enumerate(sensors):
                slot = s.get("slot", i)
                name = s.get("name", "?")
                can_id = s.get("can_id", "")
                scale  = s.get("scale", "")
                offset = s.get("offset", "")
                lines.append(f"  [{slot:3d}]  {name:<20}  CAN: {can_id:<6}  "
                             f"scale={scale}  offset={offset}")
            self._set_view_text("\n".join(lines), COL_WHITE)
        else:
            pretty = json.dumps({"vehicle": vehicle, "sensors": sensors},
                                ensure_ascii=False, indent=2)
            self._set_view_text(pretty, COL_GRAY)

    def _set_view_text(self, text, color=None):
        self.txt_view.config(state="normal")
        self.txt_view.delete("1.0", "end")
        self.txt_view.insert("end", text)
        if color:
            self.txt_view.config(fg=color)
        self.txt_view.config(state="disabled")


    # ── Auto-Update ───────────────────────────────────────────────────────────
    def _check_updates(self, silent=False):
        threading.Thread(target=self._fetch_update,
                         args=(silent,), daemon=True).start()

    def _fetch_update(self, silent):
        url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
        try:
            req = urllib.request.Request(
                url, headers={"User-Agent": "BRL-Format-Tool"})
            with urllib.request.urlopen(req, timeout=8) as r:
                data = json.loads(r.read())
            latest = data.get("tag_name", "").lstrip("v")
            if not latest:
                return
            installer_url = None
            for asset in data.get("assets", []):
                n = asset.get("name", "").lower()
                if n.endswith(".exe") and "brl" in n and "setup" in n:
                    installer_url = asset.get("browser_download_url")
                    break
            changelog = data.get("body", "")[:600]
            def ver(s):
                try:    return tuple(int(x) for x in s.split("."))
                except: return (0,)
            if ver(latest) > ver(APP_VERSION):
                self.after(0, self._show_update_dialog,
                           latest, installer_url, changelog)
            elif not silent:
                self.after(0, messagebox.showinfo, "Kein Update",
                           f"Du hast die neueste Version (v{APP_VERSION}).")
        except Exception as e:
            if not silent:
                self.after(0, messagebox.showwarning,
                           "Update-Prüfung",
                           f"GitHub nicht erreichbar:\n{e}")

    def _show_update_dialog(self, latest_version, installer_url, changelog):
        dlg = tk.Toplevel(self)
        dlg.title(f"Update verfügbar  –  v{latest_version}")
        dlg.configure(bg=COL_BG)
        dlg.resizable(False, False)
        dlg.grab_set()

        tk.Frame(dlg, bg=COL_BLUE, width=5).pack(side="left", fill="y")
        content = tk.Frame(dlg, bg=COL_BG, padx=24, pady=20)
        content.pack(fill="both", expand=True)

        tk.Label(content, text="UPDATE VERFÜGBAR",
                 bg=COL_BG, fg=COL_BLUE,
                 font=("Helvetica", 13, "bold")).pack(anchor="w")
        tk.Label(content,
                 text=f"Version {latest_version} verfügbar  (aktuell: v{APP_VERSION})",
                 bg=COL_BG, fg=COL_GRAY,
                 font=("Helvetica", 10)).pack(anchor="w", pady=(2,14))

        if changelog:
            cl_f = tk.Frame(content, bg=COL_PANEL,
                            highlightbackground=COL_BORDER, highlightthickness=1)
            cl_f.pack(fill="x", pady=(0,14))
            cl_txt = tk.Text(cl_f, height=6, wrap="word",
                             bg=COL_PANEL, fg=COL_GRAY,
                             font=("Helvetica", 9), relief="flat", bd=0,
                             padx=10, pady=8)
            cl_txt.insert("end", changelog)
            cl_txt.config(state="disabled")
            cl_txt.pack(fill="x")

        dl_info = tk.StringVar(value="")
        lbl_dl  = tk.Label(content, textvariable=dl_info,
                           bg=COL_BG, fg=COL_AMBER,
                           font=("Helvetica", 9))
        dl_bar  = ttk.Progressbar(content, mode="determinate",
                                   style="BMW.Horizontal.TProgressbar")

        btn_row = tk.Frame(content, bg=COL_BG)
        btn_row.pack(fill="x", pady=(4,0))

        def do_later():
            dlg.destroy()

        def do_install():
            if not installer_url:
                messagebox.showwarning(
                    "Kein Installer",
                    "Kein Windows-Installer in diesem Release gefunden.\n\n"
                    "Bitte auf GitHub manuell herunterladen.",
                    parent=dlg)
                webbrowser.open(
                    f"https://github.com/{GITHUB_REPO}/releases/latest")
                dlg.destroy()
                return
            btn_inst.config(state="disabled", text="Wird heruntergeladen…")
            btn_late.config(state="disabled")
            lbl_dl.pack(anchor="w", pady=(8,2))
            dl_bar.pack(fill="x", pady=(0,8))

            def reporthook(pct, mb, mb_total):
                dl_info.set(
                    f"Herunterladen…  {mb:.1f} / {mb_total:.1f} MB  ({pct}%)")
                dl_bar["value"] = pct

            def on_done(tmp_path):
                dl_info.set("Download abgeschlossen – Installer wird gestartet…")
                dlg.after(800, lambda: self._run_installer(tmp_path, dlg))

            def on_error(msg):
                btn_inst.config(state="normal", text="  Update installieren")
                btn_late.config(state="normal")
                dl_info.set(f"Fehler: {msg}")
                lbl_dl.config(fg=COL_RED)

            threading.Thread(
                target=self._do_download,
                args=(installer_url, latest_version,
                      reporthook, on_done, on_error),
                daemon=True).start()

        btn_inst = tk.Button(btn_row, text="  Update installieren",
                             command=do_install,
                             bg=COL_BLUE, fg=COL_WHITE,
                             activebackground=COL_BLUE_DK,
                             activeforeground=COL_WHITE,
                             font=("Helvetica", 11, "bold"),
                             relief="flat", padx=16, pady=10,
                             cursor="hand2")
        btn_inst.pack(side="left")
        btn_late = tk.Button(btn_row, text="Später",
                             command=do_later,
                             bg=COL_PANEL, fg=COL_GRAY,
                             activebackground=COL_BORDER,
                             activeforeground=COL_WHITE,
                             font=("Helvetica", 10),
                             relief="flat", padx=14, pady=10,
                             cursor="hand2")
        btn_late.pack(side="right")

        dlg.update_idletasks()
        x = self.winfo_x() + (self.winfo_width()  - dlg.winfo_reqwidth())  // 2
        y = self.winfo_y() + (self.winfo_height() - dlg.winfo_reqheight()) // 2
        dlg.geometry(f"+{x}+{y}")

    @staticmethod
    def _do_download(url, version, reporthook, on_done, on_error):
        try:
            tmp_dir  = Path(tempfile.mkdtemp())
            tmp_path = tmp_dir / f"BRL-Format-Setup-v{version}.exe"
            req = urllib.request.Request(
                url, headers={"User-Agent": "BRL-Format-Tool"})
            with urllib.request.urlopen(req, timeout=60) as resp:
                total = int(resp.headers.get("Content-Length", 0))
                done  = 0
                with open(tmp_path, "wb") as f:
                    while True:
                        chunk = resp.read(65536)
                        if not chunk:
                            break
                        f.write(chunk)
                        done += len(chunk)
                        if total:
                            reporthook(min(100, done * 100 // total),
                                       done / 1_000_000, total / 1_000_000)
            on_done(tmp_path)
        except Exception as e:
            on_error(str(e))

    def _run_installer(self, tmp_path: Path, dialog=None):
        if dialog:
            try:
                dialog.destroy()
            except Exception:
                pass
        try:
            subprocess.Popen([str(tmp_path), "/SILENT",
                              "/CLOSEAPPLICATIONS", "/RESTARTAPPLICATIONS"])
        except Exception as e:
            messagebox.showerror(
                "Installer-Fehler",
                f"Installer konnte nicht gestartet werden:\n{e}\n\n"
                f"Datei: {tmp_path}")
            return
        self.after(500, self.destroy)


# ── Hilfsfunktionen ──────────────────────────────────────────────────────────

def _open_folder(file_path):
    import platform
    try:
        p = Path(file_path)
        if platform.system() == "Windows":
            subprocess.run(["explorer", "/select,", str(p)])
        elif platform.system() == "Darwin":
            subprocess.run(["open", "-R", str(p)])
        else:
            subprocess.run(["xdg-open", str(p.parent)])
    except Exception:
        pass


# ── Einstiegspunkt ────────────────────────────────────────────────────────────

if __name__ == "__main__":
    app = App()
    app.mainloop()

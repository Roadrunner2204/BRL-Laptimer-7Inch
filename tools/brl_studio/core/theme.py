"""
Theme system — dark + light QSS, applied app-wide.

Why a single style sheet rather than per-widget overrides:
    Qt's QFileDialog / QMessageBox / QInputDialog all inherit the
    QApplication style sheet. Setting it on the application instance
    propagates everywhere; per-widget setStyleSheet only paints the
    one widget and leaks back to system colours in dialogs — which is
    exactly the inconsistent look the user complained about.

Persisted in QSettings under "ui/theme" ∈ {"dark", "light"}.
"""

from __future__ import annotations

from PyQt6.QtCore import QObject, QSettings, pyqtSignal
from PyQt6.QtGui import QColor, QPalette
from PyQt6.QtWidgets import QApplication


class _ThemeBus(QObject):
    """Singleton emitter so widgets can repaint themselves when the
    application theme changes at runtime, without a back-channel through
    QApplication or the main window."""

    theme_changed = pyqtSignal(dict)   # palette dict


_BUS: _ThemeBus | None = None


def theme_bus() -> _ThemeBus:
    global _BUS
    if _BUS is None:
        _BUS = _ThemeBus()
    return _BUS


THEME_DARK = "dark"
THEME_LIGHT = "light"
DEFAULT_THEME = THEME_DARK


# Centralised palette — referenced by chart/map widgets that draw their
# own pixels (pyqtgraph background, custom QPainter widgets) so they
# follow the active theme too.
PALETTES = {
    THEME_DARK: {
        "bg":           "#15171c",
        "surface":      "#1e2127",
        "surface_alt":  "#262a31",
        "border":       "#3a3f48",
        "text":         "#e6e6e6",
        "text_dim":     "#8a8f99",
        "accent":       "#2d6cdf",
        "accent_text":  "#ffffff",
        "ok":           "#00CC66",
        "warn":         "#FFB300",
        "danger":       "#E53935",
        "selection":    "#2d6cdf",
    },
    THEME_LIGHT: {
        "bg":           "#f4f5f7",
        "surface":      "#ffffff",
        "surface_alt":  "#eef0f3",
        "border":       "#c8ccd1",
        "text":         "#1c1f24",
        "text_dim":     "#6b7180",
        "accent":       "#2d6cdf",
        "accent_text":  "#ffffff",
        "ok":           "#1f9d55",
        "warn":         "#c97a00",
        "danger":       "#c0392b",
        "selection":    "#2d6cdf",
    },
}


def _qss(p: dict) -> str:
    """Build the application-wide stylesheet from a palette dict."""
    return f"""
    QWidget {{
        background-color: {p['bg']};
        color: {p['text']};
        font-size: 11pt;
    }}

    QMainWindow, QDialog, QMessageBox, QInputDialog, QFileDialog {{
        background-color: {p['bg']};
        color: {p['text']};
    }}

    /* Sidebar list (object name set in main_window.py) */
    QListWidget#NavList {{
        background-color: {p['surface']};
        border: 0;
    }}
    QListWidget#NavList::item {{
        padding: 12px 16px;
        color: {p['text']};
    }}
    QListWidget#NavList::item:selected {{
        background-color: {p['selection']};
        color: {p['accent_text']};
    }}

    /* Generic lists / tables */
    QListWidget, QTreeWidget, QTreeView, QListView {{
        background-color: {p['surface']};
        alternate-background-color: {p['surface_alt']};
        border: 1px solid {p['border']};
        color: {p['text']};
    }}
    QListWidget::item:selected, QTreeWidget::item:selected,
    QTreeView::item:selected, QListView::item:selected {{
        background-color: {p['selection']};
        color: {p['accent_text']};
    }}

    QTableWidget, QTableView {{
        background-color: {p['surface']};
        alternate-background-color: {p['surface_alt']};
        gridline-color: {p['border']};
        color: {p['text']};
        border: 1px solid {p['border']};
    }}
    QTableWidget::item:selected, QTableView::item:selected {{
        background-color: {p['selection']};
        color: {p['accent_text']};
    }}
    QHeaderView::section {{
        background-color: {p['surface_alt']};
        color: {p['text']};
        padding: 4px 8px;
        border: 0;
        border-right: 1px solid {p['border']};
        border-bottom: 1px solid {p['border']};
    }}

    /* Buttons */
    QPushButton {{
        background-color: {p['surface_alt']};
        color: {p['text']};
        border: 1px solid {p['border']};
        border-radius: 4px;
        padding: 6px 12px;
        min-height: 22px;
    }}
    QPushButton:hover {{ background-color: {p['surface']}; }}
    QPushButton:pressed {{ background-color: {p['accent']}; color: {p['accent_text']}; }}
    QPushButton:checked {{ background-color: {p['accent']}; color: {p['accent_text']}; border-color: {p['accent']}; }}
    QPushButton:disabled {{ color: {p['text_dim']}; background-color: {p['surface']}; }}

    /* Inputs */
    QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit, QTextEdit {{
        background-color: {p['surface']};
        color: {p['text']};
        border: 1px solid {p['border']};
        border-radius: 3px;
        padding: 4px 6px;
        selection-background-color: {p['selection']};
        selection-color: {p['accent_text']};
    }}
    QComboBox QAbstractItemView {{
        background-color: {p['surface']};
        color: {p['text']};
        selection-background-color: {p['selection']};
    }}

    /* Group + tabs */
    QGroupBox {{
        border: 1px solid {p['border']};
        border-radius: 4px;
        margin-top: 12px;
        padding-top: 6px;
        color: {p['text']};
    }}
    QGroupBox::title {{
        subcontrol-origin: margin;
        left: 12px;
        padding: 0 6px;
        color: {p['text_dim']};
    }}

    QSplitter::handle {{
        background-color: {p['border']};
    }}

    /* Status bar */
    QStatusBar {{
        background-color: {p['surface']};
        color: {p['text']};
    }}

    /* Sliders */
    QSlider::groove:horizontal {{
        height: 6px;
        background: {p['surface_alt']};
        border-radius: 3px;
    }}
    QSlider::handle:horizontal {{
        background: {p['accent']};
        width: 14px;
        margin: -5px 0;
        border-radius: 7px;
    }}

    /* Progress bar */
    QProgressBar {{
        background-color: {p['surface_alt']};
        border: 1px solid {p['border']};
        border-radius: 3px;
        text-align: center;
        color: {p['text']};
    }}
    QProgressBar::chunk {{
        background-color: {p['accent']};
    }}

    /* Tooltips */
    QToolTip {{
        background-color: {p['surface_alt']};
        color: {p['text']};
        border: 1px solid {p['border']};
        padding: 4px 6px;
    }}
    """


def _apply_palette(app: QApplication, p: dict) -> None:
    """Set QPalette so non-stylesheet drawing (selection highlights in
    QGraphicsView, etc.) follows the theme too."""
    pal = QPalette()
    pal.setColor(QPalette.ColorRole.Window, QColor(p["bg"]))
    pal.setColor(QPalette.ColorRole.WindowText, QColor(p["text"]))
    pal.setColor(QPalette.ColorRole.Base, QColor(p["surface"]))
    pal.setColor(QPalette.ColorRole.AlternateBase, QColor(p["surface_alt"]))
    pal.setColor(QPalette.ColorRole.Text, QColor(p["text"]))
    pal.setColor(QPalette.ColorRole.Button, QColor(p["surface_alt"]))
    pal.setColor(QPalette.ColorRole.ButtonText, QColor(p["text"]))
    pal.setColor(QPalette.ColorRole.Highlight, QColor(p["selection"]))
    pal.setColor(QPalette.ColorRole.HighlightedText, QColor(p["accent_text"]))
    pal.setColor(QPalette.ColorRole.ToolTipBase, QColor(p["surface_alt"]))
    pal.setColor(QPalette.ColorRole.ToolTipText, QColor(p["text"]))
    app.setPalette(pal)


def get_active_theme() -> str:
    return str(QSettings().value("ui/theme", DEFAULT_THEME, str)) or DEFAULT_THEME


def get_palette(name: str | None = None) -> dict:
    return PALETTES.get(name or get_active_theme(), PALETTES[DEFAULT_THEME])


def apply_theme(app: QApplication, name: str) -> None:
    """Apply theme app-wide and persist the choice. Fires theme_bus
    so custom-painted widgets (charts, sector bars, the Leaflet map)
    can refresh their colours without restart."""
    if name not in PALETTES:
        name = DEFAULT_THEME
    p = PALETTES[name]
    _apply_palette(app, p)
    app.setStyleSheet(_qss(p))
    QSettings().setValue("ui/theme", name)
    theme_bus().theme_changed.emit(p)

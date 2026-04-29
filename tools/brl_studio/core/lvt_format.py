"""
.lvt — BRL Layout Template format.

A .lvt file describes a fully custom dashboard layout for the laptimer's
1024x600 display. Phase 6 of the Studio writes them; the firmware-side
renderer is a follow-up task in main/ui/.

Schema (JSON):

    {
        "version": 1,
        "width": 1024,
        "height": 600,
        "background": "#000000",
        "widgets": [
            {
                "type": "value",
                "x": 0, "y": 100, "w": 300, "h": 100,
                "field": 2,                  // FieldId enum (dash_config.py)
                "font_pt": 64,
                "color": "#FFFFFF",
                "align": "center"
            },
            {
                "type": "label",
                "x": 0, "y": 0, "w": 200, "h": 40,
                "text": "BRL Laptimer",
                "font_pt": 24,
                "color": "#FFFFFF",
                "align": "left"
            },
            {
                "type": "rect",
                "x": 0, "y": 90, "w": 1024, "h": 80,
                "fill": "#1a1c20",
                "border": null,
                "border_w": 0
            },
            {
                "type": "line",
                "x1": 0, "y1": 90, "x2": 1024, "y2": 90,
                "color": "#3a3f48", "width": 2
            },
            {
                "type": "image",
                "x": 0, "y": 0, "w": 100, "h": 50,
                "src": "logo.png"             // resolved relative to .lvt
            }
        ]
    }

Studio is the source of truth for the schema; the firmware-side renderer
must accept any well-formed .lvt and ignore unknown widget types or
fields. New properties are additive.
"""

from __future__ import annotations

LVT_VERSION = 1
LVT_CANVAS_W = 1024
LVT_CANVAS_H = 600


def empty_lvt() -> dict:
    return {
        "version": LVT_VERSION,
        "width": LVT_CANVAS_W,
        "height": LVT_CANVAS_H,
        "background": "#000000",
        "widgets": [],
    }


WIDGET_DEFAULTS: dict[str, dict] = {
    "value": {
        "type": "value", "x": 100, "y": 100, "w": 300, "h": 100,
        "field": 2, "font_pt": 64, "color": "#FFFFFF", "align": "center",
    },
    "label": {
        "type": "label", "x": 100, "y": 50, "w": 240, "h": 40,
        "text": "Label", "font_pt": 24, "color": "#FFFFFF", "align": "left",
    },
    "rect": {
        "type": "rect", "x": 0, "y": 0, "w": 200, "h": 100,
        "fill": "#1a1c20", "border": "#3a3f48", "border_w": 1,
    },
    "line": {
        "type": "line", "x1": 0, "y1": 90, "x2": 1024, "y2": 90,
        "color": "#3a3f48", "width": 2,
    },
    "image": {
        "type": "image", "x": 0, "y": 0, "w": 200, "h": 100, "src": "",
    },
}

/**
 * screen_video_settings.cpp — LVGL screen for camera aim + status.
 *
 * Layout (1024×600, content area = y=90, h=510):
 *   ┌──────────────────────────────────────────────┐
 *   │  status bar                                  │ 40
 *   │  back  |  Video-Einstellungen                │ 50
 *   ├────────────────┬─────────────────────────────┤
 *   │                │  Status: …                  │
 *   │   PREVIEW      │  Recording: idle / REC      │
 *   │   640×360      │  SD frei: 87 %              │
 *   │   canvas       │  Resolution: 1920×1080@30   │
 *   │                │  IP: 192.168.4.42           │
 *   └────────────────┴─────────────────────────────┘
 *
 * The preview pump (cam_preview.cpp) feeds the canvas. A "No signal"
 * label sits on top of the canvas and fades in when the most recent
 * fetch is older than 2 s.
 */

#include "screen_video_settings.h"
#include "app.h"
#include "theme.h"
#include "i18n.h"
#include "brl_fonts.h"
#include "cam_preview.h"
#include "../camera_link/cam_link.h"
#include "../data/lap_data.h"
#include "../compat.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>

/* Preview canvas dims — 16:9 to match the cam's 1920×1080 output, sized
 * so the right column has room for the status panel + a 12 px gutter. */
#define PV_W   640
#define PV_H   360

/* Status panel widgets — refreshed by an LVGL timer while the screen
 * is alive. nullptr'd in the back callback so no stale pointer survives
 * into the next instance. */
static lv_obj_t *s_canvas       = nullptr;
static lv_obj_t *s_no_signal_lbl= nullptr;
static lv_obj_t *s_status_lbl   = nullptr;
static lv_obj_t *s_rec_lbl      = nullptr;
static lv_obj_t *s_sd_lbl       = nullptr;
static lv_obj_t *s_res_lbl      = nullptr;
static lv_obj_t *s_ip_lbl       = nullptr;
static lv_timer_t *s_refresh_timer = nullptr;

static void vs_refresh_status(lv_timer_t * /*t*/)
{
    if (!s_status_lbl || !lv_obj_is_valid(s_status_lbl)) return;

    CamLinkInfo info = cam_link_get_info();

    /* Connection status */
    if (info.link_up) {
        lv_label_set_text(s_status_lbl, tr(TR_CAM_CONNECTED));
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x00CC66), 0);
    } else {
        lv_label_set_text(s_status_lbl, tr(TR_CAM_NOT_CONNECTED));
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF4444), 0);
    }

    /* Recording */
    if (info.link_up && info.status.rec_active) {
        char buf[64];
        snprintf(buf, sizeof(buf), LV_SYMBOL_VIDEO " %s", tr(TR_CAM_RECORDING));
        lv_label_set_text(s_rec_lbl, buf);
        lv_obj_set_style_text_color(s_rec_lbl, lv_color_hex(0xFF3030), 0);
    } else {
        lv_label_set_text(s_rec_lbl, "—");
        lv_obj_set_style_text_color(s_rec_lbl, BRL_CLR_TEXT_DIM, 0);
    }

    /* SD free pct */
    if (info.link_up) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u %%", (unsigned)info.status.sd_free_pct);
        lv_label_set_text(s_sd_lbl, buf);
        lv_obj_set_style_text_color(s_sd_lbl,
            info.status.sd_free_pct < 10 ? lv_color_hex(0xFF8800) : BRL_CLR_TEXT, 0);
    } else {
        lv_label_set_text(s_sd_lbl, "—");
    }

    /* Resolution — fixed in firmware for now (1080p30). */
    if (info.link_up && info.status.cam_connected) {
        lv_label_set_text(s_res_lbl, "1920 × 1080 @ 30");
    } else {
        lv_label_set_text(s_res_lbl, tr(TR_CAM_NOT_CONNECTED));
    }

    /* IP address */
    if (info.link_up && info.status.ip_addr[0] != '\0' &&
        strcmp(info.status.ip_addr, "0.0.0.0") != 0) {
        lv_label_set_text(s_ip_lbl, info.status.ip_addr);
    } else {
        lv_label_set_text(s_ip_lbl, "—");
    }

    /* No-signal overlay on the preview canvas */
    if (s_no_signal_lbl && lv_obj_is_valid(s_no_signal_lbl)) {
        bool show = !cam_preview_has_signal();
        if (show) lv_obj_remove_flag(s_no_signal_lbl, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag   (s_no_signal_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void vs_back(lv_event_t * /*e*/)
{
    /* Tear down the preview pump BEFORE the screen disappears so the
     * worker doesn't try to paint a deleted canvas. */
    cam_preview_close();
    if (s_refresh_timer) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = nullptr;
    }
    s_canvas = s_no_signal_lbl = s_status_lbl = s_rec_lbl = nullptr;
    s_sd_lbl = s_res_lbl = s_ip_lbl = nullptr;

    open_settings_screen();
}

extern "C" void open_video_settings_screen(void)
{
    /* Use the same sub-screen skeleton everything else lives in so the
     * back button + header style stay consistent (declarations in app.h). */
    lv_obj_t *scr = make_sub_screen(tr(TR_VIDEO_SETTINGS_TITLE), vs_back,
                                    nullptr, nullptr);
    lv_obj_t *content = build_content_area(scr, false);

    /* ── Left column: preview canvas ─────────────────────────────── */
    lv_obj_t *canvas = lv_canvas_create(content);
    lv_obj_set_size(canvas, PV_W, PV_H);
    lv_obj_set_pos(canvas, 4, 4);
    lv_obj_set_style_bg_color(canvas, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(canvas, 1, 0);
    lv_obj_set_style_border_color(canvas, BRL_CLR_SURFACE2, 0);
    s_canvas = canvas;

    /* "No signal" overlay — hidden as soon as the first decode lands. */
    s_no_signal_lbl = lv_label_create(content);
    lv_label_set_text(s_no_signal_lbl, tr(TR_VIDEO_NO_SIGNAL));
    brl_style_label(s_no_signal_lbl, &BRL_FONT_24, lv_color_hex(0xAAAAAA));
    lv_obj_align_to(s_no_signal_lbl, canvas, LV_ALIGN_CENTER, 0, 0);

    /* Hint below the preview */
    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, tr(TR_VIDEO_PREVIEW_HINT));
    brl_style_label(hint, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(hint, 4, PV_H + 12);

    /* ── Right column: status panel ──────────────────────────────── */
    const int RX = PV_W + 24;
    const int RW = BRL_SCREEN_W - RX - 24;
    int row_y = 4;
    auto add_row = [&](TrKey label_key, lv_obj_t **value_out) {
        lv_obj_t *l = lv_label_create(content);
        lv_label_set_text(l, tr(label_key));
        brl_style_label(l, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        lv_obj_set_pos(l, RX, row_y);

        lv_obj_t *v = lv_label_create(content);
        lv_label_set_text(v, "—");
        brl_style_label(v, &BRL_FONT_20, BRL_CLR_TEXT);
        lv_obj_set_pos(v, RX, row_y + 22);
        lv_obj_set_width(v, RW);
        *value_out = v;
        row_y += 64;
    };

    add_row(TR_VIDEO_REC_STATUS,   &s_status_lbl);
    add_row(TR_CAM_RECORDING,      &s_rec_lbl);
    add_row(TR_VIDEO_SD_FREE,      &s_sd_lbl);
    add_row(TR_VIDEO_RESOLUTION,   &s_res_lbl);
    {
        lv_obj_t *l = lv_label_create(content);
        lv_label_set_text(l, "IP");
        brl_style_label(l, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        lv_obj_set_pos(l, RX, row_y);
        s_ip_lbl = lv_label_create(content);
        lv_label_set_text(s_ip_lbl, "—");
        brl_style_label(s_ip_lbl, &BRL_FONT_20, BRL_CLR_TEXT);
        lv_obj_set_pos(s_ip_lbl, RX, row_y + 22);
        lv_obj_set_width(s_ip_lbl, RW);
    }

    sub_screen_load(scr);

    /* Kick off the preview pump + status refresh. The cam_preview
     * worker runs at 5 Hz; the status refresh at 2 Hz is plenty. */
    cam_preview_open(canvas, PV_W, PV_H);
    s_refresh_timer = lv_timer_create(vs_refresh_status, 500, nullptr);
    vs_refresh_status(nullptr);   /* immediate first paint */
}

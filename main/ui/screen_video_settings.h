#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Video Settings screen — entered from Settings → "Camera Module" when
 * the cam link is up. Hosts a live preview canvas (for aiming) and a
 * status panel (REC state, SD-frei, resolution, IP). Closes the
 * preview pump on exit so we release the JPEG decoder + buffers.
 */
void open_video_settings_screen(void);

#ifdef __cplusplus
}
#endif

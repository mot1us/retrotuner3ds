#ifndef MINIIPTV_THEME_UI_H
#define MINIIPTV_THEME_UI_H

#include <stdbool.h>
#include "system/util/hid_types.h"

/* Call once before the UI/HID threads start. The preference is optional. */
void MiniIptv_theme_ui_init(const char *settings_path);
/* X opens the picker. While open, consume controls except START; A saves,
 * B/X cancel the preview. No playback state is touched. */
bool MiniIptv_theme_ui_hid(const Hid_info *key);
/* Draw over the bottom UI only. Return false when the picker is closed. */
bool MiniIptv_theme_ui_draw(void);
/* Bounded, non-animated decorations, within [top, top+10). */
void MiniIptv_theme_ui_draw_trim(float width, float top);

#endif

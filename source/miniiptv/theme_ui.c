#include "miniiptv/theme_ui.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "miniiptv/theme.h"
#include "miniiptv/theme_store.h"
#include "miniiptv/version.h"
#include "system/draw/draw.h"

static LightLock picker_lock;
static bool picker_ready;
static bool picker_open;
static bool save_failed;
static bool saving;
static MiniIptvThemeId original_theme;
static MiniIptvThemeId preview_theme;
static char preference_path[192];
static MiniIptvThemeStoreResult save_result;

void MiniIptv_theme_ui_init(const char *settings_path) {
    MiniIptvThemeId id = MINIIPTV_THEME_CLASSIC;

    LightLock_Init(&picker_lock);
    preference_path[0] = '\0';
    if (settings_path && strlen(settings_path) < sizeof(preference_path)) {
        snprintf(preference_path, sizeof(preference_path), "%s", settings_path);
        (void)miniiptv_theme_store_load(preference_path, &id);
    }
    (void)miniiptv_theme_set(id);
    original_theme = preview_theme = id;
    picker_open = save_failed = saving = false;
    save_result = (MiniIptvThemeStoreResult){ MINIIPTV_THEME_STORE_OK, 0, false };
    __atomic_store_n(&picker_ready, true, __ATOMIC_RELEASE);
}

static void log_save_result(MiniIptvThemeId id,
                            const MiniIptvThemeStoreResult *result) {
    char path[sizeof(preference_path) + 4];
    FILE *file;
    if (!preference_path[0]) return;
    /* Last attempt only, under 192 bytes. Optional: the visible error remains
     * useful even when the SD cannot accept this diagnostic either. */
    snprintf(path, sizeof(path), "%s.log", preference_path);
    file = fopen(path, "wb");
    if (!file) return;
    (void)fprintf(file, "version=%s\ntheme=%s\nstage=%s\nbackup=%u\nerrno=%d\n",
                  RETROTUNER_VERSION, miniiptv_theme_get(id)->key,
                  miniiptv_theme_store_stage_name(result->stage),
                  result->backup ? 1u : 0u, result->error);
    (void)fclose(file);
}

bool MiniIptv_theme_ui_hid(const Hid_info *key) {
    MiniIptvThemeId confirmed;
    MiniIptvThemeStoreResult result;
    bool ok;

    if (!key || !__atomic_load_n(&picker_ready, __ATOMIC_ACQUIRE) ||
        DEF_HID_PHY_PR(key->start)) return false;

    LightLock_Lock(&picker_lock);
    if (!picker_open) {
        if (!DEF_HID_PHY_PR(key->x)) {
            LightLock_Unlock(&picker_lock);
            return false;
        }
        original_theme = preview_theme = miniiptv_theme_current();
        picker_open = true;
        save_failed = false;
    } else if (saving) {
        /* Never send a second selection through to channel/player controls. */
    } else if (DEF_HID_PHY_PR(key->b) || DEF_HID_PHY_PR(key->x)) {
        (void)miniiptv_theme_set(original_theme);
        picker_open = false;
    } else if (DEF_HID_PHY_PR(key->d_up) || DEF_HID_PHY_PR(key->d_left)) {
        preview_theme = preview_theme == MINIIPTV_THEME_CLASSIC
            ? (MiniIptvThemeId)(MINIIPTV_THEME_COUNT - 1)
            : (MiniIptvThemeId)(preview_theme - 1);
        (void)miniiptv_theme_set(preview_theme);
        save_failed = false;
    } else if (DEF_HID_PHY_PR(key->d_down) || DEF_HID_PHY_PR(key->d_right)) {
        preview_theme = (MiniIptvThemeId)((preview_theme + 1) % MINIIPTV_THEME_COUNT);
        (void)miniiptv_theme_set(preview_theme);
        save_failed = false;
    } else if (DEF_HID_PHY_PR(key->a)) {
        confirmed = preview_theme;
        saving = true;
        LightLock_Unlock(&picker_lock);
        Draw_set_refresh_needed(true);
        ok = miniiptv_theme_store_save(preference_path, confirmed, &result);
        log_save_result(confirmed, &result);
        LightLock_Lock(&picker_lock);
        original_theme = confirmed;
        saving = false;
        save_failed = !ok;
        save_result = result;
        if (ok) picker_open = false;
    }
    LightLock_Unlock(&picker_lock);
    Draw_set_refresh_needed(true);
    return true;
}

void MiniIptv_theme_ui_draw_trim(float width, float top) {
    const MiniIptvTheme *theme = miniiptv_theme_active();
    MiniIptvThemeId id = miniiptv_theme_current();
    Draw_image_data pixel = Draw_get_empty_image();
    float left = 12.0f;
    float right = width - 12.0f;

    if (width < 80.0f || top < 0.0f || top > 230.0f) return;
    if (id == MINIIPTV_THEME_RADIO) {
        /* A small radio tuning scale, not an animated or fake buffer gauge. */
        Draw_texture(&pixel, theme->edge, left, top + 1, right - left, 1);
        for (unsigned int i = 0; i <= 20; i++) {
            float x = left + (right - left) * (float)i / 20.0f;
            Draw_texture(&pixel, theme->muted, x, top + 2, 1,
                         i % 5u == 0 ? 6 : 3);
        }
        Draw_texture(&pixel, theme->accent, width * 0.62f, top, 2, 9);
    } else if (id == MINIIPTV_THEME_CYBERPUNK) {
        Draw_texture(&pixel, theme->accent, left, top + 1, width * 0.42f, 2);
        Draw_texture(&pixel, theme->highlight, width * 0.56f, top + 1,
                     width * 0.44f - 12.0f, 2);
        for (unsigned int i = 0; i < 5; i++)
            Draw_texture(&pixel, theme->highlight, left + (float)i * 7,
                         top + 5, 4, 3);
    } else if (id == MINIIPTV_THEME_WASTELAND) {
        Draw_texture(&pixel, theme->accent, left, top, right - left, 1);
        Draw_texture(&pixel, theme->accent, left, top, 2, 8);
        Draw_texture(&pixel, theme->accent, right - 1, top, 2, 8);
        for (unsigned int i = 0; i < 4; i++)
            Draw_texture(&pixel, theme->muted, width / 2 - 9 + (float)i * 6,
                         top + 4, 2, 2);
    } else {
        Draw_texture(&pixel, theme->highlight, left, top + 2, right - left, 1);
        Draw_texture(&pixel, theme->accent, left, top + 2, 45, 2);
    }
}

bool MiniIptv_theme_ui_draw(void) {
    MiniIptvThemeId selected;
    bool failed, in_progress, open;
    MiniIptvThemeStoreResult result;
    char message[64];
    const MiniIptvTheme *theme;
    Draw_image_data pixel;

    if (!__atomic_load_n(&picker_ready, __ATOMIC_ACQUIRE)) return false;
    LightLock_Lock(&picker_lock);
    open = picker_open;
    selected = preview_theme;
    failed = save_failed;
    in_progress = saving;
    result = save_result;
    LightLock_Unlock(&picker_lock);
    if (!open) return false;

    theme = miniiptv_theme_get(selected);
    pixel = Draw_get_empty_image();
    Draw_texture(&pixel, theme->background, 0, 0, 320, 240);
    Draw_texture(&pixel, theme->accent, 12, 12, 296, 2);
    Draw_c("CHOOSE A LOOK", 14, 21, 15.0f, theme->text);
    Draw_c("D-pad to preview. Playback stays unchanged.", 14, 42, 9.5f, theme->muted);

    for (unsigned int i = 0; i < MINIIPTV_THEME_COUNT; i++) {
        const MiniIptvTheme *option = miniiptv_theme_get((MiniIptvThemeId)i);
        float y = 62.0f + (float)i * 28.0f;
        bool picked = i == (unsigned int)selected;
        Draw_texture(&pixel, picked ? theme->highlight : theme->panel,
                     12, y, 296, 24);
        Draw_c(option->name, 22, y + 4, 13.0f,
               picked ? theme->selection_text : theme->text);
        Draw_texture(&pixel, option->background, 252, y + 5, 14, 14);
        Draw_texture(&pixel, option->accent, 268, y + 5, 14, 14);
        Draw_texture(&pixel, option->highlight, 284, y + 5, 14, 14);
    }
    snprintf(message, sizeof(message), "NOT SAVED: %s%s (%d)",
             result.backup ? "BAK " : "",
             miniiptv_theme_store_stage_name(result.stage), result.error);
    Draw_align_c(in_progress ? "SAVING..." :
                 (failed ? message : theme->description),
                 12, 180, 10.5f, failed ? theme->warning : theme->muted,
                 DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 296, 17);
    Draw_align_c("D-PAD PREVIEW   A APPLY   B CANCEL", 8, 207, 10.0f,
                 theme->text, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 304, 16);
    MiniIptv_theme_ui_draw_trim(320, 226);
    return true;
}

#include "miniiptv/theme.h"

#include <string.h>

static const MiniIptvTheme themes[MINIIPTV_THEME_COUNT] = {
    [MINIIPTV_THEME_CLASSIC] = {
        .key = "classic", .name = "Classic", .description = "Navy, teal and amber",
        .background = 0xFF241A14, .panel = 0xFF3B2B24,
        .text = 0xFFE8EBED, .muted = 0xFFB6B9B9,
        .accent = 0xFF4AA6E3, .highlight = 0xFFD2B56C,
        .selection_text = 0xFF241A14, .shadow = 0xFF1B1917,
        .edge = 0xFF716052, .success = 0xFFA6D891,
        .warning = 0xFF72CEED, .danger = 0xFF9796FF,
    },
    [MINIIPTV_THEME_CYBERPUNK] = {
        .key = "cyberpunk", .name = "Cyberpunk", .description = "Midnight purple, cyan and hot pink",
        .background = 0xFF20100F, .panel = 0xFF351B23,
        .text = 0xFFFFF1ED, .muted = 0xFFDCC2C9,
        .accent = 0xFFFFF052, .highlight = 0xFFC375FF,
        .selection_text = 0xFF20100F, .shadow = 0xFF10070B,
        .edge = 0xFF815B65, .success = 0xFFA8E475,
        .warning = 0xFF6CDCFB, .danger = 0xFF9587FF,
    },
    [MINIIPTV_THEME_WASTELAND] = {
        .key = "wasteland", .name = "Wasteland", .description = "Green phosphor on an old terminal",
        .background = 0xFF111910, .panel = 0xFF1B2C20,
        .text = 0xFFBDF0CB, .muted = 0xFF9BC3A8,
        .accent = 0xFF6ED89F, .highlight = 0xFF85F3B8,
        .selection_text = 0xFF111910, .shadow = 0xFF080E09,
        .edge = 0xFF416451, .success = 0xFF74E09B,
        .warning = 0xFF81DDE8, .danger = 0xFF91A9FF,
    },
    [MINIIPTV_THEME_RADIO] = {
        .key = "radio", .name = "Old-Time Radio", .description = "Warm walnut, ivory and brass",
        .background = 0xFF172632, .panel = 0xFF263A48,
        .text = 0xFFCDE9FA, .muted = 0xFFA9CBDE,
        .accent = 0xFF61BDEB, .highlight = 0xFF9BDCF5,
        .selection_text = 0xFF172632, .shadow = 0xFF0D151C,
        .edge = 0xFF60839D, .success = 0xFFA0D3B9,
        .warning = 0xFF7CDDF7, .danger = 0xFFA3ACFF,
    },
};

static unsigned int active_theme = MINIIPTV_THEME_CLASSIC;

const MiniIptvTheme *miniiptv_theme_get(MiniIptvThemeId id) {
    if ((unsigned int)id >= MINIIPTV_THEME_COUNT)
        id = MINIIPTV_THEME_CLASSIC;
    return &themes[id];
}

MiniIptvThemeId miniiptv_theme_current(void) {
    return (MiniIptvThemeId)__atomic_load_n(&active_theme, __ATOMIC_RELAXED);
}

bool miniiptv_theme_set(MiniIptvThemeId id) {
    if ((unsigned int)id >= MINIIPTV_THEME_COUNT)
        return false;
    /* Only an index is published; no other mutable data needs ordering. */
    __atomic_store_n(&active_theme, (unsigned int)id, __ATOMIC_RELAXED);
    return true;
}

const MiniIptvTheme *miniiptv_theme_active(void) {
    return miniiptv_theme_get(miniiptv_theme_current());
}

bool miniiptv_theme_parse(const char *data, size_t size, MiniIptvThemeId *out) {
    if (!data || !out || size == 0 || size > 32)
        return false;
    if (data[size - 1] == '\n') {
        size--;
        if (size > 0 && data[size - 1] == '\r')
            size--;
    }
    for (unsigned int i = 0; i < MINIIPTV_THEME_COUNT; i++) {
        size_t key_size = strlen(themes[i].key);
        if (size == key_size && memcmp(data, themes[i].key, size) == 0) {
            *out = (MiniIptvThemeId)i;
            return true;
        }
    }
    return false;
}

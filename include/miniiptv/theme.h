#ifndef MINIIPTV_THEME_H
#define MINIIPTV_THEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum MiniIptvThemeId {
    MINIIPTV_THEME_CLASSIC = 0,
    MINIIPTV_THEME_CYBERPUNK,
    MINIIPTV_THEME_WASTELAND,
    MINIIPTV_THEME_RADIO,
    MINIIPTV_THEME_COUNT
} MiniIptvThemeId;

/* Opaque ABGR8888 colors; tables are immutable and safe to retain. */
typedef struct MiniIptvTheme {
    const char *key;
    const char *name;
    const char *description;
    uint32_t background;
    uint32_t panel;
    uint32_t text;
    uint32_t muted;
    uint32_t accent;
    uint32_t highlight;
    uint32_t selection_text;
    uint32_t shadow;
    uint32_t edge;
    uint32_t success;
    uint32_t warning;
    uint32_t danger;
} MiniIptvTheme;

/* Invalid lookup IDs fall back to Classic; invalid sets change nothing. */
const MiniIptvTheme *miniiptv_theme_get(MiniIptvThemeId id);
MiniIptvThemeId miniiptv_theme_current(void);
bool miniiptv_theme_set(MiniIptvThemeId id);
const MiniIptvTheme *miniiptv_theme_active(void);

/* Parse 1..32 bytes containing an exact key, optionally ending in LF/CRLF.
 * Input need not be NUL-terminated. Failure leaves *out unchanged. */
bool miniiptv_theme_parse(const char *data, size_t size, MiniIptvThemeId *out);

#endif

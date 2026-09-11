#ifndef MINIIPTV_THEME_STORE_H
#define MINIIPTV_THEME_STORE_H

#include "miniiptv/theme.h"

typedef enum {
    MINIIPTV_THEME_STORE_OK = 0,
    MINIIPTV_THEME_STORE_PATH,
    MINIIPTV_THEME_STORE_OPEN,
    MINIIPTV_THEME_STORE_WRITE,
    MINIIPTV_THEME_STORE_FLUSH,
    MINIIPTV_THEME_STORE_CLOSE,
    MINIIPTV_THEME_STORE_VERIFY
} MiniIptvThemeStoreStage;

typedef struct {
    MiniIptvThemeStoreStage stage;
    int error;
    bool backup;
} MiniIptvThemeStoreResult;

/* Read a bounded preference; fall back to path.bak if the primary is invalid.
 * A failed load leaves *id unchanged. No writes occur during loading. */
bool miniiptv_theme_store_load(const char *path, MiniIptvThemeId *id);

/* Preserve a valid previous setting in path.bak before replacing path.
 * Uses no rename/remove and verifies written bytes by reopening the file.
 * This is recovery for a small optional preference, not an atomic filesystem
 * transaction or a guarantee against SD removal/power loss. */
bool miniiptv_theme_store_save(const char *path, MiniIptvThemeId id,
                              MiniIptvThemeStoreResult *result);

const char *miniiptv_theme_store_stage_name(MiniIptvThemeStoreStage stage);

#endif

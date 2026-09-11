#include "miniiptv/theme_store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define THEME_PATH_BYTES 192u

static bool valid_path(const char *path) {
    return path && path[0] && strlen(path) < THEME_PATH_BYTES;
}

static bool read_preference(const char *path, MiniIptvThemeId *id) {
    char data[33];
    MiniIptvThemeId parsed;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t count = fread(data, 1, sizeof(data), file);
    bool ok = !ferror(file) && miniiptv_theme_parse(data, count, &parsed);
    if (fclose(file) != 0) ok = false;
    if (ok) *id = parsed;
    return ok;
}

bool miniiptv_theme_store_load(const char *path, MiniIptvThemeId *id) {
    char backup[THEME_PATH_BYTES + 4u];
    if (!valid_path(path) || !id) return false;
    if (read_preference(path, id)) return true;
    snprintf(backup, sizeof(backup), "%s.bak", path);
    return read_preference(backup, id);
}

static bool failed(MiniIptvThemeStoreResult *result,
                   MiniIptvThemeStoreStage stage) {
    result->stage = stage;
    result->error = errno ? errno : EIO;
    return false;
}

static bool write_verified(const char *path, MiniIptvThemeId id,
                           MiniIptvThemeStoreResult *result) {
    char data[32];
    MiniIptvThemeId stored;
    size_t count = (size_t)snprintf(data, sizeof(data), "%s\n",
                                   miniiptv_theme_get(id)->key);
    errno = 0;
    FILE *file = fopen(path, "wb");
    if (!file) return failed(result, MINIIPTV_THEME_STORE_OPEN);
    errno = 0;
    bool ok = fwrite(data, 1, count, file) == count;
    if (!ok) failed(result, MINIIPTV_THEME_STORE_WRITE);
    if (ok) {
        errno = 0;
        if (fflush(file) != 0)
            ok = failed(result, MINIIPTV_THEME_STORE_FLUSH);
    }
    errno = 0;
    if (fclose(file) != 0 && ok)
        ok = failed(result, MINIIPTV_THEME_STORE_CLOSE);
    if (!ok) return false;
    errno = 0;
    if (!read_preference(path, &stored))
        return failed(result, MINIIPTV_THEME_STORE_VERIFY);
    if (stored != id) {
        errno = EIO;
        return failed(result, MINIIPTV_THEME_STORE_VERIFY);
    }
    return true;
}

bool miniiptv_theme_store_save(const char *path, MiniIptvThemeId id,
                              MiniIptvThemeStoreResult *result) {
    MiniIptvThemeStoreResult local = { MINIIPTV_THEME_STORE_OK, 0, false };
    MiniIptvThemeId previous;
    char backup[THEME_PATH_BYTES + 4u];
    if (!result) result = &local;
    *result = local;
    if (!valid_path(path) || (unsigned int)id >= MINIIPTV_THEME_COUNT) {
        errno = EINVAL;
        return failed(result, MINIIPTV_THEME_STORE_PATH);
    }

    /* The bundled 3DS filesystem does not offer desktop-POSIX rename
     * guarantees. Keep recovery independent of rename and target deletion.
     * Only touch the primary after a valid prior value is backed up. */
    if (read_preference(path, &previous)) {
        if (previous == id) return true;
        snprintf(backup, sizeof(backup), "%s.bak", path);
        result->backup = true;
        if (!write_verified(backup, previous, result)) return false;
    }
    result->backup = false;
    return write_verified(path, id, result);
}

const char *miniiptv_theme_store_stage_name(MiniIptvThemeStoreStage stage) {
    switch (stage) {
        case MINIIPTV_THEME_STORE_OK: return "OK";
        case MINIIPTV_THEME_STORE_PATH: return "PATH";
        case MINIIPTV_THEME_STORE_OPEN: return "OPEN";
        case MINIIPTV_THEME_STORE_WRITE: return "WRITE";
        case MINIIPTV_THEME_STORE_FLUSH: return "FLUSH";
        case MINIIPTV_THEME_STORE_CLOSE: return "CLOSE";
        case MINIIPTV_THEME_STORE_VERIFY: return "VERIFY";
        default: return "UNKNOWN";
    }
}

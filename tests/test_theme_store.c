#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "miniiptv/theme_store.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char fixture_dir[] = "/tmp/retrotuner-theme-store.XXXXXX";
static char preference_path[192];
static char backup_path[196];
static MiniIptvThemeStoreStage injected_stage;
static bool inject_backup;
static bool injected;
static bool target_was_written;
static unsigned int write_opens;
static FILE *target_stream;

/* Keep real stdio underneath the store so byte contents and reload behavior
 * are checked on disk. Only one selected operation fails on each attempt. */
static bool is_fault_path(const char *path) {
    return strcmp(path, inject_backup ? backup_path : preference_path) == 0;
}

static FILE *store_fopen(const char *path, const char *mode) {
    bool writing = strchr(mode, 'w') != NULL;
    if (writing) write_opens++;
    if (!injected && is_fault_path(path) &&
        ((writing && injected_stage == MINIIPTV_THEME_STORE_OPEN) ||
         (!writing && target_was_written &&
          injected_stage == MINIIPTV_THEME_STORE_VERIFY))) {
        injected = true;
        errno = EACCES;
        return NULL;
    }
    FILE *file = fopen(path, mode);
    if (file && writing && is_fault_path(path)) {
        target_stream = file;
        target_was_written = true;
    }
    return file;
}

static size_t store_fwrite(const void *data, size_t size, size_t count,
                          FILE *file) {
    if (!injected && file == target_stream &&
        injected_stage == MINIIPTV_THEME_STORE_WRITE) {
        injected = true;
        /* Leave a real partial preference, not just a fabricated return. */
        assert(size == 1 && count > 1);
        assert(fwrite(data, 1, 1, file) == 1);
        errno = ENOSPC;
        return 1;
    }
    return fwrite(data, size, count, file);
}

static int store_fflush(FILE *file) {
    int result = fflush(file);
    if (!injected && file == target_stream &&
        injected_stage == MINIIPTV_THEME_STORE_FLUSH) {
        injected = true;
        errno = ENOSPC;
        return EOF;
    }
    return result;
}

static int store_fclose(FILE *file) {
    bool target = file == target_stream;
    if (target) target_stream = NULL;
    int result = fclose(file);
    if (!injected && target &&
        injected_stage == MINIIPTV_THEME_STORE_CLOSE) {
        injected = true;
        errno = EIO;
        return EOF;
    }
    return result;
}

#define fopen store_fopen
#define fwrite store_fwrite
#define fflush store_fflush
#define fclose store_fclose
#include "../source/miniiptv/theme_store.c"
#undef fclose
#undef fflush
#undef fwrite
#undef fopen

static void reset_fault(MiniIptvThemeStoreStage stage, bool backup) {
    injected_stage = stage;
    inject_backup = backup;
    injected = target_was_written = false;
    write_opens = 0;
    assert(target_stream == NULL);
}

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void write_text(const char *path, const char *text) {
    write_bytes(path, text, strlen(text));
}

static void assert_file(const char *path, const char *expected) {
    char actual[128];
    FILE *file = fopen(path, "rb");
    assert(file);
    size_t size = fread(actual, 1, sizeof(actual), file);
    assert(!ferror(file));
    assert(size == strlen(expected));
    assert(memcmp(actual, expected, size) == 0);
    assert(fclose(file) == 0);
}

static void remove_optional(const char *path) {
    assert(unlink(path) == 0 || errno == ENOENT);
}

static void clear_preferences(void) {
    reset_fault(MINIIPTV_THEME_STORE_OK, false);
    remove_optional(preference_path);
    remove_optional(backup_path);
}

static void assert_load(MiniIptvThemeId expected) {
    MiniIptvThemeId loaded = MINIIPTV_THEME_COUNT;
    unsigned int before = write_opens;
    assert(miniiptv_theme_store_load(preference_path, &loaded));
    assert(loaded == expected);
    assert(write_opens == before);
}

static void test_load_and_backup_fallback(void) {
    clear_preferences();
    MiniIptvThemeId loaded = MINIIPTV_THEME_RADIO;
    assert(!miniiptv_theme_store_load(preference_path, &loaded));
    assert(loaded == MINIIPTV_THEME_RADIO);
    static const char *valid[] = {
        "classic", "cyberpunk\n", "wasteland\r\n", "radio\n"
    };
    write_text(backup_path, "radio\n");
    for (unsigned int i = 0; i < MINIIPTV_THEME_COUNT; i++) {
        write_text(preference_path, valid[i]);
        assert_load((MiniIptvThemeId)i);
    }
    static const char *invalid[] = {
        "", "unknown\n", "radio\nclassic\n", "RADIO", " radio ", "radio\r"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        write_text(preference_path, invalid[i]);
        assert_load(MINIIPTV_THEME_RADIO);
    }
    const char embedded_nul[] = "classic\0\n";
    write_bytes(preference_path, embedded_nul, sizeof(embedded_nul) - 1);
    assert_load(MINIIPTV_THEME_RADIO);
    char oversized[80];
    memset(oversized, 'x', sizeof(oversized));
    memcpy(oversized, "classic\n", 8);
    write_bytes(preference_path, oversized, sizeof(oversized));
    assert_load(MINIIPTV_THEME_RADIO);
    assert(unlink(preference_path) == 0);
    assert_load(MINIIPTV_THEME_RADIO);
    write_text(backup_path, "bad\n");
    assert(!miniiptv_theme_store_load(preference_path, &loaded));
    assert(loaded == MINIIPTV_THEME_RADIO);
    assert(write_opens == 0);
}

static void test_repeated_saves_and_unchanged_preference(void) {
    clear_preferences();
    MiniIptvThemeStoreResult result;
    const char *previous = NULL;
    for (unsigned int iteration = 0; iteration < 12; iteration++) {
        MiniIptvThemeId id = (MiniIptvThemeId)(iteration % MINIIPTV_THEME_COUNT);
        const char *key = miniiptv_theme_get(id)->key;
        char expected[32];
        int size = snprintf(expected, sizeof(expected), "%s\n", key);
        assert(size > 0 && (size_t)size < sizeof(expected));
        reset_fault(MINIIPTV_THEME_STORE_OK, false);
        assert(miniiptv_theme_store_save(preference_path, id, &result));
        assert(result.stage == MINIIPTV_THEME_STORE_OK && result.error == 0);
        assert_file(preference_path, expected);
        assert(write_opens == (iteration == 0 ? 1u : 2u));
        if (previous) {
            char backup[32];
            size = snprintf(backup, sizeof(backup), "%s\n", previous);
            assert(size > 0 && (size_t)size < sizeof(backup));
            assert_file(backup_path, backup);
        } else {
            assert(access(backup_path, F_OK) != 0);
        }
        assert_load(id);
        /* An unchanged setting remains successful even if a write would fail. */
        reset_fault(MINIIPTV_THEME_STORE_OPEN, false);
        assert(miniiptv_theme_store_save(preference_path, id, &result));
        assert(!injected && write_opens == 0);
        assert(result.stage == MINIIPTV_THEME_STORE_OK && result.error == 0);
        previous = key;
    }
    reset_fault(MINIIPTV_THEME_STORE_OK, false);
    write_text(preference_path, "classic\r\n");
    assert(miniiptv_theme_store_save(preference_path, MINIIPTV_THEME_CLASSIC, NULL));
    assert(write_opens == 0);
    assert_file(preference_path, "classic\r\n");
}

static void test_missing_or_corrupt_primary_preserves_backup(void) {
    clear_preferences();
    for (unsigned int corrupt = 0; corrupt < 2; corrupt++) {
        remove_optional(preference_path);
        if (corrupt) write_text(preference_path, "broken\n");
        write_text(backup_path, "wasteland\n");
        reset_fault(MINIIPTV_THEME_STORE_OPEN, true);
        MiniIptvThemeStoreResult result;
        assert(miniiptv_theme_store_save(preference_path, MINIIPTV_THEME_RADIO,
                                        &result));
        assert(!injected && write_opens == 1);
        assert_file(preference_path, "radio\n");
        assert_file(backup_path, "wasteland\n");
        assert_load(MINIIPTV_THEME_RADIO);
    }
}

static void test_each_io_failure(void) {
    static const MiniIptvThemeStoreStage stages[] = {
        MINIIPTV_THEME_STORE_OPEN, MINIIPTV_THEME_STORE_WRITE,
        MINIIPTV_THEME_STORE_FLUSH, MINIIPTV_THEME_STORE_CLOSE,
        MINIIPTV_THEME_STORE_VERIFY
    };
    for (unsigned int backup = 0; backup < 2; backup++) {
        for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
            clear_preferences();
            write_text(preference_path, "classic\n");
            write_text(backup_path, "radio\n");
            reset_fault(stages[i], backup != 0);
            MiniIptvThemeStoreResult result;
            assert(!miniiptv_theme_store_save(preference_path,
                                             MINIIPTV_THEME_CYBERPUNK, &result));
            assert(injected);
            assert(result.stage == stages[i]);
            assert(result.backup == (backup != 0));
            int expected_errno = (stages[i] == MINIIPTV_THEME_STORE_WRITE ||
                                  stages[i] == MINIIPTV_THEME_STORE_FLUSH)
                ? ENOSPC : (stages[i] == MINIIPTV_THEME_STORE_CLOSE ? EIO : EACCES);
            assert(result.error == expected_errno);
            assert(target_stream == NULL);
            if (backup) {
                /* Failure to preserve the old selection must prevent any
                 * truncation or replacement of the primary preference. */
                assert(write_opens == 1);
                assert_file(preference_path, "classic\n");
                assert_load(MINIIPTV_THEME_CLASSIC);
            } else {
                assert(write_opens == 2);
                assert_file(backup_path, "classic\n");
                if (stages[i] == MINIIPTV_THEME_STORE_OPEN ||
                    stages[i] == MINIIPTV_THEME_STORE_WRITE)
                    assert_load(MINIIPTV_THEME_CLASSIC);
                /* Flush/close errors can occur after bytes reach disk. The
                 * prior backup must still recover an absent/corrupt primary. */
                assert(unlink(preference_path) == 0);
                assert_load(MINIIPTV_THEME_CLASSIC);
            }
        }
    }
}

static void test_invalid_arguments_and_path_boundary(void) {
    clear_preferences();
    char long_path[193];
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    const char *invalid_paths[] = {NULL, "", long_path};
    for (size_t i = 0; i < sizeof(invalid_paths) / sizeof(invalid_paths[0]); i++) {
        MiniIptvThemeId id = MINIIPTV_THEME_RADIO;
        MiniIptvThemeStoreResult result;
        assert(!miniiptv_theme_store_load(invalid_paths[i], &id));
        assert(id == MINIIPTV_THEME_RADIO);
        assert(!miniiptv_theme_store_save(invalid_paths[i], MINIIPTV_THEME_CLASSIC,
                                         &result));
        assert(result.stage == MINIIPTV_THEME_STORE_PATH && result.error != 0);
        assert(!result.backup);
    }
    assert(!miniiptv_theme_store_load(preference_path, NULL));
    const MiniIptvThemeId invalid_ids[] = {
        (MiniIptvThemeId)-1, MINIIPTV_THEME_COUNT, (MiniIptvThemeId)99
    };
    write_text(preference_path, "radio\n");
    for (size_t i = 0; i < sizeof(invalid_ids) / sizeof(invalid_ids[0]); i++) {
        MiniIptvThemeStoreResult result;
        assert(!miniiptv_theme_store_save(preference_path, invalid_ids[i], &result));
        assert(result.stage == MINIIPTV_THEME_STORE_PATH && result.error == EINVAL);
        assert(!result.backup);
        assert_file(preference_path, "radio\n");
    }
    assert(write_opens == 0);
    char maximum_path[192];
    size_t prefix_size = strlen(fixture_dir);
    memcpy(maximum_path, fixture_dir, prefix_size);
    maximum_path[prefix_size++] = '/';
    memset(maximum_path + prefix_size, 'm', sizeof(maximum_path) - prefix_size - 1);
    maximum_path[sizeof(maximum_path) - 1] = '\0';
    assert(miniiptv_theme_store_save(maximum_path, MINIIPTV_THEME_RADIO, NULL));
    assert_file(maximum_path, "radio\n");
    assert(unlink(maximum_path) == 0);
    assert(strcmp(miniiptv_theme_store_stage_name(MINIIPTV_THEME_STORE_OK), "OK") == 0);
    assert(strcmp(miniiptv_theme_store_stage_name(MINIIPTV_THEME_STORE_PATH), "PATH") == 0);
    assert(strcmp(miniiptv_theme_store_stage_name(MINIIPTV_THEME_STORE_OPEN), "OPEN") == 0);
    assert(strcmp(miniiptv_theme_store_stage_name(MINIIPTV_THEME_STORE_WRITE), "WRITE") == 0);
    assert(strcmp(miniiptv_theme_store_stage_name(MINIIPTV_THEME_STORE_FLUSH), "FLUSH") == 0);
    assert(strcmp(miniiptv_theme_store_stage_name(MINIIPTV_THEME_STORE_CLOSE), "CLOSE") == 0);
    assert(strcmp(miniiptv_theme_store_stage_name(MINIIPTV_THEME_STORE_VERIFY), "VERIFY") == 0);
}

int main(void) {
    assert(mkdtemp(fixture_dir));
    int size = snprintf(preference_path, sizeof(preference_path), "%s/theme.txt", fixture_dir);
    assert(size > 0 && (size_t)size < sizeof(preference_path));
    size = snprintf(backup_path, sizeof(backup_path), "%s.bak", preference_path);
    assert(size > 0 && (size_t)size < sizeof(backup_path));
    test_load_and_backup_fallback();
    test_repeated_saves_and_unchanged_preference();
    test_missing_or_corrupt_primary_preserves_backup();
    test_each_io_failure();
    test_invalid_arguments_and_path_boundary();
    clear_preferences();
    assert(rmdir(fixture_dir) == 0);
    puts("theme store tests passed");
    return 0;
}

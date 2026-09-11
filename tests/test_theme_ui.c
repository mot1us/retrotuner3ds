#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "miniiptv/theme.h"
#include "miniiptv/theme_ui.h"
#include "system/draw/draw.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned int refresh_count;
static unsigned int rectangle_count;
static unsigned int text_count;
static char drawn_text[32][128];
static char fixture_dir[] = "/tmp/retrotuner-theme-ui.XXXXXX";
static char preference_path[192];
static char backup_path[196];
static char log_path[196];

Draw_image_data Draw_get_empty_image(void) {
    Draw_image_data image = {0};
    return image;
}

void Draw_set_refresh_needed(bool needed) {
    assert(needed);
    refresh_count++;
}

static void assert_rectangle(float x, float y, float width, float height) {
    assert(x >= 0 && y >= 0 && width >= 0 && height >= 0);
    assert(x + width <= 320 && y + height <= 240);
}

void Draw_texture(Draw_image_data *image, uint32_t color, float x, float y,
                  float width, float height) {
    assert(image != NULL);
    assert((color >> 24) == 0xffu);
    assert_rectangle(x, y, width, height);
    rectangle_count++;
}

void Draw_c(const char *text, float x, float y, float size, uint32_t color) {
    assert(text != NULL);
    assert(x >= 0 && x < 320 && y >= 0 && y + size <= 240);
    assert(size >= 9.5f);
    assert((color >> 24) == 0xffu);
    assert(text_count < sizeof(drawn_text) / sizeof(drawn_text[0]));
    assert(strlen(text) < sizeof(drawn_text[0]));
    snprintf(drawn_text[text_count++], sizeof(drawn_text[0]), "%s", text);
}

void Draw_align_c(const char *text, float x, float y, float size, uint32_t color,
                  Draw_text_align_x align_x, Draw_text_align_y align_y,
                  float width, float height) {
    assert(align_x == DRAW_X_ALIGN_CENTER);
    assert(align_y == DRAW_Y_ALIGN_CENTER);
    assert_rectangle(x, y, width, height);
    Draw_c(text, x, y, size, color);
}

static void reset_capture(void) {
    rectangle_count = text_count = 0;
}

static bool draw_picker(void) {
    reset_capture();
    return MiniIptv_theme_ui_draw();
}

static bool contains_text(const char *needle) {
    for (unsigned int i = 0; i < text_count; i++)
        if (strstr(drawn_text[i], needle)) return true;
    return false;
}

static bool press(Hid_key_bit bit) {
    Hid_info key = {0};
    key.a.is_active = (bit & HID_KEY_BIT_A) != 0;
    key.b.is_active = (bit & HID_KEY_BIT_B) != 0;
    key.x.is_active = (bit & HID_KEY_BIT_X) != 0;
    key.l.is_active = (bit & HID_KEY_BIT_L) != 0;
    key.r.is_active = (bit & HID_KEY_BIT_R) != 0;
    key.select.is_active = (bit & HID_KEY_BIT_SELECT) != 0;
    key.start.is_active = (bit & HID_KEY_BIT_START) != 0;
    key.d_up.is_active = (bit & HID_KEY_BIT_D_UP) != 0;
    key.d_down.is_active = (bit & HID_KEY_BIT_D_DOWN) != 0;
    key.d_left.is_active = (bit & HID_KEY_BIT_D_LEFT) != 0;
    key.d_right.is_active = (bit & HID_KEY_BIT_D_RIGHT) != 0;
    return MiniIptv_theme_ui_hid(&key);
}

static void make_path(char *out, size_t size, const char *name) {
    int written = snprintf(out, size, "%s/%s", fixture_dir, name);
    assert(written > 0 && (size_t)written < size);
}

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
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

static void assert_log(const char *theme, const char *stage, bool backup) {
    char data[512], expected[64];
    FILE *file = fopen(log_path, "rb");
    assert(file);
    size_t size = fread(data, 1, sizeof(data) - 1, file);
    assert(!ferror(file) && size > 0 && size < sizeof(data) - 1);
    assert(fclose(file) == 0);
    data[size] = '\0';
    assert(strstr(data, "version=") == data);
    snprintf(expected, sizeof(expected), "\ntheme=%s\n", theme);
    assert(strstr(data, expected));
    snprintf(expected, sizeof(expected), "\nstage=%s\n", stage);
    assert(strstr(data, expected));
    snprintf(expected, sizeof(expected), "\nbackup=%u\n", backup ? 1u : 0u);
    assert(strstr(data, expected));
    assert(strstr(data, "\nerrno="));
    if (strcmp(stage, "OK") == 0) assert(strstr(data, "\nerrno=0\n"));
    /* Each apply replaces the previous result; it never grows an SD log. */
    unsigned int lines = 0;
    for (size_t i = 0; i < size; i++) if (data[i] == '\n') lines++;
    assert(lines == 5);
}

static void test_before_init(void) {
    assert(!press(HID_KEY_BIT_X));
    assert(!MiniIptv_theme_ui_hid(NULL));
    assert(!draw_picker());
    assert(text_count == 0 && rectangle_count == 0);
}

static void test_init_and_bad_preferences(void) {
    MiniIptv_theme_ui_init(preference_path); /* Missing file. */
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(!draw_picker());
    static const char *valid[] = {"classic", "cyberpunk\n", "wasteland\r\n", "radio\n"};
    for (unsigned int i = 0; i < MINIIPTV_THEME_COUNT; i++) {
        write_bytes(preference_path, valid[i], strlen(valid[i]));
        MiniIptv_theme_ui_init(preference_path);
        assert(miniiptv_theme_current() == (MiniIptvThemeId)i);
        assert(!draw_picker());
    }
    static const char *invalid[] = {"", "unknown\n", "radio\nclassic\n", "RADIO", " radio ", "radio\r"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        write_bytes(preference_path, invalid[i], strlen(invalid[i]));
        MiniIptv_theme_ui_init(preference_path);
        assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    }
    const char corrupt[] = "radio\0\n";
    write_bytes(preference_path, corrupt, sizeof(corrupt) - 1);
    MiniIptv_theme_ui_init(preference_path);
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    char oversized[80];
    memset(oversized, 'x', sizeof(oversized));
    memcpy(oversized, "radio\n", 6);
    write_bytes(preference_path, oversized, sizeof(oversized));
    MiniIptv_theme_ui_init(preference_path);
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(unlink(preference_path) == 0);
    write_bytes(backup_path, "wasteland\n", 10);
    MiniIptv_theme_ui_init(preference_path);
    assert(miniiptv_theme_current() == MINIIPTV_THEME_WASTELAND);
    write_bytes(preference_path, "broken\n", 7);
    MiniIptv_theme_ui_init(preference_path);
    assert(miniiptv_theme_current() == MINIIPTV_THEME_WASTELAND);
    write_bytes(preference_path, "classic\n", 8);
    MiniIptv_theme_ui_init(preference_path);
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(unlink(preference_path) == 0);
    assert(unlink(backup_path) == 0);
}

static void test_input_preview_and_cancel(void) {
    MiniIptv_theme_ui_init(preference_path);
    static const Hid_key_bit pass_through[] = {
        HID_KEY_BIT_NONE, HID_KEY_BIT_A, HID_KEY_BIT_B, HID_KEY_BIT_L,
        HID_KEY_BIT_R, HID_KEY_BIT_SELECT, HID_KEY_BIT_START,
        HID_KEY_BIT_D_UP, HID_KEY_BIT_D_DOWN,
    };
    for (size_t i = 0; i < sizeof(pass_through) / sizeof(pass_through[0]); i++)
        assert(!press(pass_through[i]));
    unsigned int previous_refresh = refresh_count;
    assert(press(HID_KEY_BIT_X));
    assert(refresh_count > previous_refresh);
    assert(draw_picker());
    assert(contains_text("CHOOSE A LOOK"));
    assert(press(HID_KEY_BIT_D_UP));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_RADIO);
    assert(press(HID_KEY_BIT_D_RIGHT));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(press(HID_KEY_BIT_D_LEFT));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_RADIO);
    assert(press(HID_KEY_BIT_D_DOWN));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(press(HID_KEY_BIT_D_DOWN));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CYBERPUNK);
    Hid_info held = {0};
    held.d_down.is_active = held.d_down.was_active = true;
    assert(MiniIptv_theme_ui_hid(&held));
    held.d_down.is_active = false;
    assert(MiniIptv_theme_ui_hid(&held));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CYBERPUNK);
    assert(press(HID_KEY_BIT_L));
    assert(press(HID_KEY_BIT_R));
    assert(press(HID_KEY_BIT_SELECT));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CYBERPUNK);
    assert(!press(HID_KEY_BIT_START));
    assert(!press(HID_KEY_BIT_START | HID_KEY_BIT_A));
    assert(draw_picker());
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CYBERPUNK);
    assert(access(preference_path, F_OK) != 0);
    assert(press(HID_KEY_BIT_B));
    assert(!draw_picker());
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(press(HID_KEY_BIT_X));
    assert(press(HID_KEY_BIT_D_DOWN));
    assert(press(HID_KEY_BIT_X));
    assert(!draw_picker());
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(access(preference_path, F_OK) != 0);
}

static void test_save_and_reload(void) {
    MiniIptv_theme_ui_init(preference_path);
    assert(press(HID_KEY_BIT_X));
    assert(press(HID_KEY_BIT_D_LEFT));
    assert(press(HID_KEY_BIT_A));
    assert(!draw_picker());
    assert(miniiptv_theme_current() == MINIIPTV_THEME_RADIO);
    assert_file(preference_path, "radio\n");
    assert_log("radio", "OK", false);
    assert(miniiptv_theme_set(MINIIPTV_THEME_CLASSIC));
    MiniIptv_theme_ui_init(preference_path);
    assert(miniiptv_theme_current() == MINIIPTV_THEME_RADIO);
    /* A subsequent cancel restores the saved theme, not a fixed default. */
    assert(press(HID_KEY_BIT_X));
    assert(press(HID_KEY_BIT_D_DOWN));
    assert(press(HID_KEY_BIT_B));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_RADIO);
    assert_file(preference_path, "radio\n");

    for (unsigned int iteration = 0; iteration < 8; iteration++) {
        MiniIptvThemeId expected = (MiniIptvThemeId)(iteration % MINIIPTV_THEME_COUNT);
        const char *previous = miniiptv_theme_active()->key;
        assert(press(HID_KEY_BIT_X));
        assert(press(HID_KEY_BIT_D_DOWN));
        assert(miniiptv_theme_current() == expected);
        assert(press(HID_KEY_BIT_A));
        assert(!draw_picker());
        char bytes[32];
        snprintf(bytes, sizeof(bytes), "%s\n", miniiptv_theme_get(expected)->key);
        assert_file(preference_path, bytes);
        snprintf(bytes, sizeof(bytes), "%s\n", previous);
        assert_file(backup_path, bytes);
        assert_log(miniiptv_theme_get(expected)->key, "OK", false);
        MiniIptv_theme_ui_init(preference_path);
        assert(miniiptv_theme_current() == expected);
    }
    /* Applying an unchanged theme must still refresh the bounded result log. */
    write_bytes(log_path, "stale diagnostic", 16);
    assert(press(HID_KEY_BIT_X));
    assert(press(HID_KEY_BIT_A));
    assert(!draw_picker());
    assert_log("radio", "OK", false);
}

static void test_failed_save_preserves_previous_file(void) {
    assert(unlink(backup_path) == 0);
    /* A directory at the backup filename forces fopen to fail on all
     * hosts, including when tests run with privileges that ignore chmod. */
    assert(mkdir(backup_path, 0700) == 0);
    MiniIptv_theme_ui_init(preference_path);
    assert(press(HID_KEY_BIT_X));
    assert(press(HID_KEY_BIT_D_DOWN));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(press(HID_KEY_BIT_A));
    assert(draw_picker());
    assert(contains_text("NOT SAVED: BAK OPEN ("));
    assert_log("classic", "OPEN", true);
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert_file(preference_path, "radio\n");
    assert(press(HID_KEY_BIT_B));
    assert(!draw_picker());
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(rmdir(backup_path) == 0);
    MiniIptv_theme_ui_init(preference_path);
    assert(miniiptv_theme_current() == MINIIPTV_THEME_RADIO);
    assert(unlink(preference_path) == 0);
}

static void test_failed_open_and_missing_path(void) {
    char destination[192], marker[192], destination_log[196];
    make_path(destination, sizeof(destination), "destination");
    make_path(marker, sizeof(marker), "destination/keep");
    assert(mkdir(destination, 0700) == 0);
    write_bytes(marker, "keep", 4);
    MiniIptv_theme_ui_init(destination);
    assert(press(HID_KEY_BIT_X));
    assert(press(HID_KEY_BIT_D_DOWN));
    assert(press(HID_KEY_BIT_A));
    assert(draw_picker() && contains_text("NOT SAVED: OPEN ("));
    assert_file(marker, "keep");
    int size = snprintf(destination_log, sizeof(destination_log), "%s.log", destination);
    assert(size > 0 && (size_t)size < sizeof(destination_log));
    assert(access(destination_log, F_OK) == 0);
    assert(press(HID_KEY_BIT_X));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CYBERPUNK);
    assert(unlink(marker) == 0);
    assert(rmdir(destination) == 0);
    assert(unlink(destination_log) == 0);

    char long_path[193];
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    const char *invalid_paths[] = {NULL, "", long_path};
    for (size_t i = 0; i < sizeof(invalid_paths) / sizeof(invalid_paths[0]); i++) {
        MiniIptv_theme_ui_init(invalid_paths[i]);
        assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
        assert(press(HID_KEY_BIT_X));
        assert(press(HID_KEY_BIT_D_DOWN));
        assert(press(HID_KEY_BIT_A));
        assert(draw_picker() && contains_text("NOT SAVED: PATH ("));
        assert(miniiptv_theme_current() == MINIIPTV_THEME_CYBERPUNK);
        assert(press(HID_KEY_BIT_B));
        assert(miniiptv_theme_current() == MINIIPTV_THEME_CYBERPUNK);
    }
}

static void test_unwritable_log_does_not_undo_saved_theme(void) {
    assert(unlink(log_path) == 0);
    assert(mkdir(log_path, 0700) == 0);
    MiniIptv_theme_ui_init(preference_path);
    assert(press(HID_KEY_BIT_X));
    assert(press(HID_KEY_BIT_D_DOWN));
    assert(press(HID_KEY_BIT_A));
    assert(!draw_picker());
    assert_file(preference_path, "cyberpunk\n");
    MiniIptv_theme_ui_init(preference_path);
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CYBERPUNK);
    assert(unlink(preference_path) == 0);
    assert(rmdir(log_path) == 0);
}

static void test_all_theme_draws_and_trim(void) {
    MiniIptv_theme_ui_init(preference_path);
    assert(press(HID_KEY_BIT_X));
    for (unsigned int i = 0; i < MINIIPTV_THEME_COUNT; i++) {
        assert(miniiptv_theme_current() == (MiniIptvThemeId)i);
        assert(draw_picker());
        assert(rectangle_count >= 19 && text_count == 8);
        assert(contains_text(miniiptv_theme_active()->description));
        for (unsigned int j = 0; j < MINIIPTV_THEME_COUNT; j++)
            assert(contains_text(miniiptv_theme_get((MiniIptvThemeId)j)->name));
        reset_capture();
        MiniIptv_theme_ui_draw_trim(320, 230);
        assert(rectangle_count > 0 && text_count == 0);
        reset_capture();
        MiniIptv_theme_ui_draw_trim(79, 0);
        MiniIptv_theme_ui_draw_trim(320, -1);
        MiniIptv_theme_ui_draw_trim(320, 231);
        assert(rectangle_count == 0 && text_count == 0);
        assert(press(HID_KEY_BIT_D_DOWN));
    }
    assert(press(HID_KEY_BIT_B));
    assert(!draw_picker());
}

int main(void) {
    assert(mkdtemp(fixture_dir));
    make_path(preference_path, sizeof(preference_path), "theme.txt");
    int size = snprintf(backup_path, sizeof(backup_path), "%s.bak", preference_path);
    assert(size > 0 && (size_t)size < sizeof(backup_path));
    size = snprintf(log_path, sizeof(log_path), "%s.log", preference_path);
    assert(size > 0 && (size_t)size < sizeof(log_path));
    test_before_init();
    test_init_and_bad_preferences();
    test_input_preview_and_cancel();
    test_save_and_reload();
    test_failed_save_preserves_previous_file();
    test_failed_open_and_missing_path();
    test_unwritable_log_does_not_undo_saved_theme();
    test_all_theme_draws_and_trim();
    assert(rmdir(fixture_dir) == 0);
    puts("theme UI tests passed");
    return 0;
}

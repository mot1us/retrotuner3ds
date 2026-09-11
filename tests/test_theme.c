#include "miniiptv/theme.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_lookup_and_selection(void) {
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(miniiptv_theme_get((MiniIptvThemeId)-1) ==
        miniiptv_theme_get(MINIIPTV_THEME_CLASSIC));
    assert(miniiptv_theme_get(MINIIPTV_THEME_COUNT) ==
        miniiptv_theme_get(MINIIPTV_THEME_CLASSIC));
    for (unsigned int i = 0; i < MINIIPTV_THEME_COUNT; i++) {
        MiniIptvThemeId id = (MiniIptvThemeId)i;
        assert(miniiptv_theme_set(id));
        assert(miniiptv_theme_current() == id);
        assert(miniiptv_theme_active() == miniiptv_theme_get(id));
        assert(!miniiptv_theme_set((MiniIptvThemeId)-1));
        assert(!miniiptv_theme_set(MINIIPTV_THEME_COUNT));
        assert(miniiptv_theme_current() == id);
    }
    /* Cycling belongs to the caller; both boundary choices stay valid. */
    assert(miniiptv_theme_set((MiniIptvThemeId)(
        (miniiptv_theme_current() + 1) % MINIIPTV_THEME_COUNT)));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
    assert(miniiptv_theme_set((MiniIptvThemeId)(
        (miniiptv_theme_current() + MINIIPTV_THEME_COUNT - 1) % MINIIPTV_THEME_COUNT)));
    assert(miniiptv_theme_current() == MINIIPTV_THEME_RADIO);
    assert(miniiptv_theme_set(MINIIPTV_THEME_CLASSIC));
}

static void test_parse(void) {
    MiniIptvThemeId out = MINIIPTV_THEME_CLASSIC;
    char data[32];
    for (unsigned int i = 0; i < MINIIPTV_THEME_COUNT; i++) {
        const char *key = miniiptv_theme_get((MiniIptvThemeId)i)->key;
        size_t size = strlen(key);
        memcpy(data, key, size);
        /* No terminator: parser must honor the supplied length. */
        assert(miniiptv_theme_parse(data, size, &out));
        assert(out == (MiniIptvThemeId)i);
        data[size] = '\n';
        assert(miniiptv_theme_parse(data, size + 1, &out));
        assert(out == (MiniIptvThemeId)i);
        data[size] = '\r';
        data[size + 1] = '\n';
        assert(miniiptv_theme_parse(data, size + 2, &out));
        assert(out == (MiniIptvThemeId)i);
    }
    static const char *bad[] = {
        "", "unknown", "Classic", " classic", "classic ", "classic\r",
        "classic\n\n", "classic\r\r\n", "classic\nradio", "radio\t",
        "\n", "\r\n", "classical", "rad", "radio\nextra",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        out = MINIIPTV_THEME_WASTELAND;
        assert(!miniiptv_theme_parse(bad[i], strlen(bad[i]), &out));
        assert(out == MINIIPTV_THEME_WASTELAND);
    }
    out = MINIIPTV_THEME_WASTELAND;
    const char embedded_nul[] = "radio\0classic";
    const char terminating_nul[] = "radio";
    char oversized[33];
    memset(oversized, 'x', sizeof(oversized));
    assert(!miniiptv_theme_parse(embedded_nul, sizeof(embedded_nul) - 1, &out));
    assert(!miniiptv_theme_parse(terminating_nul, sizeof(terminating_nul), &out));
    assert(!miniiptv_theme_parse(oversized, 32, &out));
    assert(!miniiptv_theme_parse(oversized, sizeof(oversized), &out));
    assert(!miniiptv_theme_parse(NULL, 5, &out));
    assert(!miniiptv_theme_parse("radio", 5, NULL));
    assert(out == MINIIPTV_THEME_WASTELAND);
    assert(miniiptv_theme_current() == MINIIPTV_THEME_CLASSIC);
}

static double linear_component(unsigned int value) {
    double scaled = (double)value / 255.0;
    return scaled <= 0.04045 ? scaled / 12.92 :
        pow((scaled + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t color) {
    return 0.2126 * linear_component(color & 0xffu) +
        0.7152 * linear_component((color >> 8) & 0xffu) +
        0.0722 * linear_component((color >> 16) & 0xffu);
}

static double contrast(uint32_t a, uint32_t b) {
    double first = luminance(a);
    double second = luminance(b);
    return first >= second ? (first + 0.05) / (second + 0.05) :
        (second + 0.05) / (first + 0.05);
}

static void test_palette_readability(void) {
    for (unsigned int i = 0; i < MINIIPTV_THEME_COUNT; i++) {
        const MiniIptvTheme *theme = miniiptv_theme_get((MiniIptvThemeId)i);
        assert(strlen(theme->key) > 0 && strlen(theme->key) <= 30);
        assert(strlen(theme->name) > 0 && strlen(theme->name) <= 18);
        assert(strlen(theme->description) > 0 && strlen(theme->description) <= 38);
        const uint32_t colors[] = {
            theme->background, theme->panel, theme->text, theme->muted,
            theme->accent, theme->highlight, theme->selection_text,
            theme->shadow, theme->edge, theme->success, theme->warning,
            theme->danger,
        };
        for (size_t j = 0; j < sizeof(colors) / sizeof(colors[0]); j++)
            assert((colors[j] >> 24) == 0xffu);
        const uint32_t readable[] = {
            theme->text, theme->muted, theme->accent,
            theme->success, theme->warning, theme->danger,
        };
        for (size_t j = 0; j < sizeof(readable) / sizeof(readable[0]); j++) {
            assert(contrast(readable[j], theme->background) >= 4.5);
            assert(contrast(readable[j], theme->panel) >= 4.5);
        }
        assert(contrast(theme->selection_text, theme->highlight) >= 4.5);
        for (unsigned int j = 0; j < i; j++) {
            const MiniIptvTheme *other = miniiptv_theme_get((MiniIptvThemeId)j);
            assert(strcmp(theme->key, other->key) != 0);
            assert(theme->background != other->background);
            assert(theme->highlight != other->highlight);
        }
    }
}

int main(void) {
    test_lookup_and_selection();
    test_parse();
    test_palette_readability();
    puts("theme tests passed");
    return 0;
}

#include "miniiptv/playlist.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_metadata_and_display_name(void) {
    static const char text[] =
        "#EXTM3U\r\n"
        "#EXTINF:-1 xgroup-title=\"Wrong\" group-title=\"Retro, TV\" "
        "http-user-agent=\"EXTINF agent\" http-referrer=\"https://ref.example/\", "
        "TVS Turbo, West group-title=\"Not metadata\" \r\n"
        "#EXTVLCOPT:http-user-agent=VLC agent\r\n"
        "https://example.test/live/master.m3u8\r\n";
    MiniIptvPlaylist playlist;

    assert(playlist_parse_text(text, &playlist) == 0);
    assert(playlist.count == 1);
    assert(strcmp(playlist.channels[0].name,
                  "TVS Turbo, West group-title=\"Not metadata\"") == 0);
    assert(strcmp(playlist.channels[0].group, "Retro, TV") == 0);
    assert(strcmp(playlist.channels[0].user_agent, "VLC agent") == 0);
    assert(strcmp(playlist.channels[0].referrer, "https://ref.example/") == 0);
    assert(strcmp(playlist.channels[0].url,
                  "https://example.test/live/master.m3u8") == 0);
}

static void test_unsupported_uri_consumes_entry(void) {
    static const char text[] =
        "#EXTM3U\n"
        "#EXTINF:-1,Unsupported transport\n"
        "udp://239.0.0.1:1234\n"
        "https://example.test/should-not-inherit.m3u8\n";
    MiniIptvPlaylist playlist;

    assert(playlist_parse_text(text, &playlist) == -3);
    assert(playlist.count == 0);
}

static void test_recovery_and_default_name(void) {
    static const char text[] =
        "#EXTM3U\n"
        "#EXTINF:-1,Ignored\n"
        "file:///tmp/video.ts\n"
        "#EXTINF:-1\n"
        "http://example.test/valid.m3u8\n";
    MiniIptvPlaylist playlist;

    assert(playlist_parse_text(text, &playlist) == 0);
    assert(playlist.count == 1);
    assert(strcmp(playlist.channels[0].name, "Unnamed channel") == 0);
    assert(strcmp(playlist.channels[0].url, "http://example.test/valid.m3u8") == 0);
}

static void test_invalid_arguments(void) {
    MiniIptvPlaylist playlist;

    assert(playlist_parse_text(NULL, &playlist) == -1);
    assert(playlist_parse_text("#EXTM3U\n", NULL) == -1);
}

static void test_oversized_url_is_rejected(void) {
    char text[MINIIPTV_URL_MAX + 64];
    MiniIptvPlaylist playlist;
    size_t offset;

    offset = (size_t)snprintf(text, sizeof(text),
                              "#EXTM3U\n#EXTINF:-1,Too long\nhttps://example.test/");
    assert(offset < sizeof(text));
    memset(text + offset, 'a', sizeof(text) - offset - 2);
    text[sizeof(text) - 2] = '\n';
    text[sizeof(text) - 1] = '\0';

    assert(strlen(text + strlen("#EXTM3U\n#EXTINF:-1,Too long\n")) >=
           MINIIPTV_URL_MAX);
    assert(playlist_parse_text(text, &playlist) == -3);
    assert(playlist.count == 0);
}

static void test_channel_limit(void) {
    char text[8192];
    char expected_name[32];
    MiniIptvPlaylist playlist;
    size_t offset = 0;
    unsigned int i;

    offset += (size_t)snprintf(text + offset, sizeof(text) - offset,
                               "#EXTM3U\n");
    for (i = 0; i < MINIIPTV_MAX_CHANNELS + 2; i++) {
        int written = snprintf(text + offset, sizeof(text) - offset,
                               "#EXTINF:-1,Channel %u\n"
                               "https://example.test/%u.m3u8\n",
                               i, i);
        assert(written > 0);
        assert((size_t)written < sizeof(text) - offset);
        offset += (size_t)written;
    }

    assert(playlist_parse_text(text, &playlist) == 0);
    assert(playlist.count == MINIIPTV_MAX_CHANNELS);
    assert(strcmp(playlist.channels[0].name, "Channel 0") == 0);
    snprintf(expected_name, sizeof(expected_name), "Channel %u",
             MINIIPTV_MAX_CHANNELS - 1u);
    assert(strcmp(playlist.channels[MINIIPTV_MAX_CHANNELS - 1].name,
                  expected_name) == 0);
}

int main(void) {
    test_metadata_and_display_name();
    test_unsupported_uri_consumes_entry();
    test_recovery_and_default_name();
    test_invalid_arguments();
    test_oversized_url_is_rejected();
    test_channel_limit();
    puts("playlist tests passed");
    return 0;
}

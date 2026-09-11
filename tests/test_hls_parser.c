#include "miniiptv/hls.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_peak_bandwidth_is_not_average_bandwidth(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=200000,BANDWIDTH=900000,"
        "RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\"\n"
        "average-looks-low.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,AVERAGE-BANDWIDTH=450000,"
        "RESOLUTION=480x270,CODECS=\"avc1.42c01e,mp4a.40.2\"\n"
        "actually-low.m3u8\n";
    HlsSelection selection;

    assert(hls_select_stream(manifest, "https://example.test/live/master.m3u8",
                             &selection) == 0);
    assert(selection.type == HLS_MASTER_PLAYLIST);
    assert(selection.bandwidth == 500000);
    assert(selection.width == 480);
    assert(selection.height == 270);
    assert(selection.frame_rate_millihz == 0);
    assert(strcmp(selection.url,
                  "https://example.test/live/actually-low.m3u8") == 0);
}

static void test_frame_rate_is_parsed(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=480x270,"
        "FRAME-RATE=29.970,CODECS=\"avc1.42c01e,mp4a.40.2\"\n"
        "live.m3u8\n";
    HlsSelection selection;

    assert(hls_select_stream(manifest, "https://example.test/master.m3u8",
                             &selection) == 0);
    assert(selection.frame_rate_millihz == 29970u);
}

static void test_incompatible_low_variant_is_skipped(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100000,CODECS=\"hvc1.1.6.L90,mp4a.40.2\"\n"
        "hevc.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=400000,CODECS=\"avc1.42c01e,mp4a.40.2\"\n"
        "/safe/h264.m3u8\n";
    HlsSelection selection;

    assert(hls_select_stream(manifest, "https://example.test/master.m3u8",
                             &selection) == 0);
    assert(selection.bandwidth == 400000);
    assert(strcmp(selection.url, "https://example.test/safe/h264.m3u8") == 0);
}

static void test_heavy_variant_never_displaces_compatible_variant(void) {
    static const char *heavy_metadata[] = {
        "RESOLUTION=1280x720,FRAME-RATE=30",
        "RESOLUTION=641x360,FRAME-RATE=30",
        "RESOLUTION=480x481,FRAME-RATE=30",
        "RESOLUTION=640x480,FRAME-RATE=30.501"
    };
    char heavy_variant[256];
    char manifest[768];
    static const char safe_variant[] =
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x480,"
        "FRAME-RATE=30.500,CODECS=\"avc1.4d401e,mp4a.40.2\"\n"
        "safe.m3u8\n";
    HlsSelection selection;

    for (size_t i = 0; i < sizeof(heavy_metadata) / sizeof(heavy_metadata[0]); i++) {
        snprintf(heavy_variant, sizeof(heavy_variant),
            "#EXT-X-STREAM-INF:BANDWIDTH=400000,%s,"
            "CODECS=\"avc1.4d401e,mp4a.40.2\"\nheavy.m3u8\n",
            heavy_metadata[i]);
        /* Both orders matter: a later low-bitrate HD entry must not undo a
         * compatible selection, and a later SD entry must replace HD. */
        for (int heavy_first = 0; heavy_first < 2; heavy_first++) {
            snprintf(manifest, sizeof(manifest), "#EXTM3U\n%s%s",
                heavy_first ? heavy_variant : safe_variant,
                heavy_first ? safe_variant : heavy_variant);
            assert(hls_select_stream(manifest,
                "https://example.test/master.m3u8", &selection) == 0);
            assert(selection.bandwidth == 500000u);
            assert(selection.width == 640u && selection.height == 480u);
            assert(selection.frame_rate_millihz == 30500u);
            assert(strcmp(selection.url, "https://example.test/safe.m3u8") == 0);
        }
    }
}

static void test_all_heavy_variants_keep_lowest_for_caller_rejection(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=1920x1080,"
        "CODECS=\"avc1.4d4028,mp4a.40.2\"\nhigh.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=1280x720,"
        "CODECS=\"avc1.4d401f,mp4a.40.2\"\nheavy.m3u8\n";
    HlsSelection selection;

    assert(hls_select_stream(manifest, "https://example.test/master.m3u8",
                             &selection) == 0);
    assert(selection.width == 1280u && selection.height == 720u);
    assert(selection.bandwidth == 400000u);
}

static void test_missing_metadata_remains_an_unverified_candidate(void) {
    static const char *unknown_metadata[] = {
        "BANDWIDTH=500000",
        "BANDWIDTH=500000,CODECS=\"avc1.4d401e,mp4a.40.2\"",
        "BANDWIDTH=500000,RESOLUTION=640x360",
        ("BANDWIDTH=500000,RESOLUTION=640x360,"
         "CODECS=\"avc1.4d401e,mp4a.40.2\""),
        "RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\""
    };
    char manifest[768];
    HlsSelection selection;

    for (size_t i = 0; i < sizeof(unknown_metadata) / sizeof(unknown_metadata[0]); i++) {
        snprintf(manifest, sizeof(manifest),
            "#EXTM3U\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=1280x720,"
            "CODECS=\"avc1.4d401f,mp4a.40.2\"\nheavy.m3u8\n"
            "#EXT-X-STREAM-INF:%s\nunknown.m3u8\n", unknown_metadata[i]);
        assert(hls_select_stream(manifest, "https://example.test/master.m3u8",
                                 &selection) == 0);
        assert(strcmp(selection.url, "https://example.test/unknown.m3u8") == 0);
        assert(selection.frame_rate_millihz == 0u);
        if (i == 0u) {
            assert(selection.width == 0u && selection.height == 0u);
            assert(selection.codecs[0] == '\0');
        }
        if (i == 4u) assert(selection.bandwidth == 0u);
    }
}

static void test_metadata_filter_does_not_admit_unsupported_codecs(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100000,RESOLUTION=480x270,"
        "CODECS=\"hvc1.1.6.L90,mp4a.40.2\"\nhevc.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=480x270,"
        "CODECS=\"avc1.4d401e,ec-3\"\ndolby.m3u8\n";
    HlsSelection selection;

    assert(hls_select_stream(manifest, "https://example.test/master.m3u8",
                             &selection) == -5);
}

static void test_separate_audio_rendition_is_resolved(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"stereo\",NAME=\"English\","
        "DEFAULT=YES,URI=\"audio/live.m3u8?token=1\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=640x360,"
        "CODECS=\"avc1.64001e,mp4a.40.2\",AUDIO=\"stereo\"\n"
        "video/360.m3u8\n";
    HlsSelection selection;

    assert(hls_select_stream(manifest,
        "https://example.test/root/master.m3u8", &selection) == 0);
    assert(selection.has_separate_audio == 1);
    assert(strcmp(selection.audio_group, "stereo") == 0);
    assert(strcmp(selection.audio_url,
        "https://example.test/root/audio/live.m3u8?token=1") == 0);
}

static void test_media_playlist_flags_and_window(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXT-X-MEDIA-SEQUENCE:40\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXT-X-BYTERANGE:188@0\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:6.0,\ns0.ts\n"
        "#EXT-X-DISCONTINUITY\n"
        "#EXTINF:6.0,\ns1.ts\n"
        "#EXTINF:6.0,\ns2.ts\n"
        "#EXTINF:6.0,\ns3.ts\n"
        "#EXTINF:6.0,\ns4.ts\n"
        "#EXTINF:6.0,\ns5.ts\n"
        "#EXTINF:6.0,\ns6.ts\n"
        "#EXTINF:6.0,\ns7.ts\n"
        "#EXTINF:6.0,\ns8.ts\n"
        "#EXT-X-ENDLIST\n";
    HlsMediaPlaylist playlist;

    assert(hls_parse_media_playlist(manifest,
                                    "https://example.test/path/media.m3u8",
                                    &playlist) == 0);
    assert(playlist.count == MINIIPTV_HLS_MAX_SEGMENTS);
    assert(playlist.media_sequence == 40);
    assert(playlist.target_duration == 6);
    assert(playlist.encrypted == 1);
    assert(playlist.uses_byte_ranges == 1);
    assert(playlist.uses_init_map == 1);
    assert(playlist.is_live == 0);
    assert(playlist.segments[0].sequence == 41);
    assert(playlist.segments[0].discontinuity == 1);
    assert(playlist.segments[7].sequence == 48);
    assert(strcmp(playlist.segments[7].url,
                  "https://example.test/path/s8.ts") == 0);
}

static void test_program_date_time_is_attached_to_next_segment(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXT-X-MEDIA-SEQUENCE:7\n"
        "#EXTINF:4.0,\n"
        "#EXT-X-PROGRAM-DATE-TIME:2026-08-24T18:34:10.086Z\n"
        "s0.ts\n"
        "#EXTINF:4.0,\n"
        "#EXT-X-PROGRAM-DATE-TIME:2026-08-24T10:34:14.086-08:00\n"
        "s1.ts\n";
    HlsMediaPlaylist playlist;

    assert(hls_parse_media_playlist(manifest,
        "https://example.test/live/index.m3u8", &playlist) == 0);
    assert(playlist.count == 2u);
    assert(playlist.segments[0].has_program_date_time == 1);
    assert(playlist.segments[1].has_program_date_time == 1);
    assert(playlist.segments[1].program_date_time_ms -
           playlist.segments[0].program_date_time_ms == 4000);
}

static void test_key_method_is_parsed_as_an_attribute(void) {
    static const char encrypted_manifest[] =
        "#EXTM3U\n"
        "#EXT-X-KEY:URI=\"key.bin,METHOD=NONE,foo\",METHOD=AES-128\n"
        "#EXTINF:6,\ns0.ts\n";
    static const char clear_manifest[] =
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=NONE,URI=\"unused\"\n"
        "#EXTINF:6,\ns0.ts\n";
    HlsMediaPlaylist playlist;

    assert(hls_parse_media_playlist(encrypted_manifest,
                                    "https://example.test/media.m3u8",
                                    &playlist) == 0);
    assert(playlist.encrypted == 1);
    assert(hls_parse_media_playlist(clear_manifest,
                                    "https://example.test/media.m3u8",
                                    &playlist) == 0);
    assert(playlist.encrypted == 0);
}

static void test_invalid_duration_is_not_a_segment(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXTINF:inf,\nignored.ts\n"
        "#EXTINF:-1,\nignored-too.ts\n";
    HlsMediaPlaylist playlist;

    assert(hls_parse_media_playlist(manifest,
                                    "https://example.test/media.m3u8",
                                    &playlist) == -4);
    assert(playlist.count == 0);
}

static void test_url_resolution(void) {
    char output[MINIIPTV_HLS_URL_MAX];

    assert(hls_resolve_url("https://example.test/a/master.m3u8", "//cdn.test/b.m3u8",
                           output, sizeof(output)) == 0);
    assert(strcmp(output, "https://cdn.test/b.m3u8") == 0);
    assert(hls_resolve_url("https://example.test/a/master.m3u8", "/root/b.m3u8",
                           output, sizeof(output)) == 0);
    assert(strcmp(output, "https://example.test/root/b.m3u8") == 0);
    assert(hls_resolve_url("https://example.test", "sibling.m3u8",
                           output, sizeof(output)) == 0);
    assert(strcmp(output, "https://example.test/sibling.m3u8") == 0);
    assert(hls_resolve_url("https://example.test/path/master.m3u8?token=abc",
                           "sibling.m3u8", output, sizeof(output)) == 0);
    assert(strcmp(output, "https://example.test/path/sibling.m3u8") == 0);
    assert(hls_resolve_url("https://example.test/path/master.m3u8?old=1#part",
                           "?new=1", output, sizeof(output)) == 0);
    assert(strcmp(output,
                  "https://example.test/path/master.m3u8?new=1") == 0);
    assert(hls_resolve_url("https://example.test/path/master.m3u8?old=1#part",
                           "#next", output, sizeof(output)) == 0);
    assert(strcmp(output,
                  "https://example.test/path/master.m3u8?old=1#next") == 0);
}

int main(void) {
    test_peak_bandwidth_is_not_average_bandwidth();
    test_incompatible_low_variant_is_skipped();
    test_heavy_variant_never_displaces_compatible_variant();
    test_all_heavy_variants_keep_lowest_for_caller_rejection();
    test_missing_metadata_remains_an_unverified_candidate();
    test_metadata_filter_does_not_admit_unsupported_codecs();
    test_frame_rate_is_parsed();
    test_separate_audio_rendition_is_resolved();
    test_media_playlist_flags_and_window();
    test_program_date_time_is_attached_to_next_segment();
    test_key_method_is_parsed_as_an_attribute();
    test_invalid_duration_is_not_a_segment();
    test_url_resolution();
    puts("hls parser tests passed");
    return 0;
}

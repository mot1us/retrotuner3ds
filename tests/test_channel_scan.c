#include "miniiptv/channel_scan.h"

#include <assert.h>
#include <stdio.h>

static const char media[] =
    "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:9\n"
    "#EXTINF:6.0,\ns9.ts\n#EXTINF:6.0,\ns10.ts\n";

static void test_safe_master_is_ready_after_media(void) {
    static const char root[] =
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=480x360,"
        "FRAME-RATE=29.97,CODECS=\"avc1.42c01e,mp4a.40.2\"\nlow.m3u8\n";
    MiniIptvScanResult result;
    assert(miniiptv_channel_scan_classify_root(
        root, "https://example.test/master.m3u8", &result) == 1);
    miniiptv_channel_scan_classify_media(
        media, "https://example.test/low.m3u8", &result);
    assert(result.status == MINIIPTV_SCAN_READY);
    assert(result.width == 480u && result.height == 360u);
}

static void test_heavy_master_stops_before_media(void) {
    static const char root[] =
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1330000,RESOLUTION=1280x720,"
        "FRAME-RATE=23.976,CODECS=\"avc1.4d401f,mp4a.40.2\"\n720.m3u8\n";
    MiniIptvScanResult result;
    assert(miniiptv_channel_scan_classify_root(
        root, "https://example.test/master.m3u8", &result) == 0);
    assert(result.status == MINIIPTV_SCAN_HEAVY);
}

static void test_direct_live_media_is_unknown(void) {
    MiniIptvScanResult result;
    assert(miniiptv_channel_scan_classify_root(
        media, "https://example.test/live.m3u8", &result) == 0);
    assert(result.status == MINIIPTV_SCAN_UNKNOWN);
}

static void test_master_without_codec_metadata_is_unknown(void) {
    static const char root[] =
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=480x360\n"
        "low.m3u8\n";
    MiniIptvScanResult result;
    assert(miniiptv_channel_scan_classify_root(
        root, "https://example.test/master.m3u8", &result) == 1);
    miniiptv_channel_scan_classify_media(
        media, "https://example.test/low.m3u8", &result);
    assert(result.status == MINIIPTV_SCAN_UNKNOWN);
}

static void test_packaged_or_encrypted_media_is_rejected(void) {
    static const char packaged[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:6.0,\ns0.m4s\n";
    static const char encrypted[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n#EXTINF:6.0,\ns0.ts\n";
    MiniIptvScanResult result;
    assert(miniiptv_channel_scan_classify_root(
        packaged, "https://example.test/live.m3u8", &result) == 0);
    assert(result.status == MINIIPTV_SCAN_UNSUPPORTED);
    assert(miniiptv_channel_scan_classify_root(
        encrypted, "https://example.test/live.m3u8", &result) == 0);
    assert(result.status == MINIIPTV_SCAN_UNSUPPORTED);
}

int main(void) {
    test_safe_master_is_ready_after_media();
    test_heavy_master_stops_before_media();
    test_direct_live_media_is_unknown();
    test_master_without_codec_metadata_is_unknown();
    test_packaged_or_encrypted_media_is_rejected();
    puts("channel scan tests passed");
    return 0;
}

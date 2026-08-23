#include "miniiptv/hls_prefetch.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad_segment;
static const char *media_manifest;
static size_t observed_segment_limit;

static int set_response(NetworkTextResponse *response, const void *data, size_t size,
                        const char *final_url) {
    response->data = malloc(size + 1);
    if (!response->data) return -1;
    memcpy(response->data, data, size);
    response->data[size] = '\0';
    response->size = size;
    response->http_status = 200;
    snprintf(response->final_url, sizeof(response->final_url), "%s", final_url);
    return 0;
}

static int mock_fetch(const char *url, const char *user_agent, const char *referrer,
                      size_t maximum_size, NetworkTextResponse *response) {
    static const char master[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\"\n"
        "low/media.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\n"
        "high/media.m3u8\n";
    unsigned char packet[376];

    (void)user_agent;
    (void)referrer;
    memset(response, 0, sizeof(*response));

    if (strcmp(url, "https://example.test/master.m3u8") == 0)
        return set_response(response, master, strlen(master), url);
    if (strcmp(url, "https://example.test/low/media.m3u8") == 0)
        return set_response(response, media_manifest, strlen(media_manifest), url);
    if (strstr(url, ".ts")) {
        observed_segment_limit = maximum_size;
        if (maximum_size < sizeof(packet)) return -2;
        memset(packet, 0, sizeof(packet));
        packet[0] = bad_segment ? 0x00 : 0x47;
        packet[188] = bad_segment ? 0x00 : 0x47;
        packet[1] = (unsigned char)url[strlen(url) - 4];
        return set_response(response, packet, sizeof(packet), url);
    }
    return -3;
}

static MiniIptvChannel channel(void) {
    MiniIptvChannel value;
    memset(&value, 0, sizeof(value));
    snprintf(value.name, sizeof(value.name), "%s", "Test channel");
    snprintf(value.url, sizeof(value.url), "%s", "https://example.test/master.m3u8");
    return value;
}

static void test_success(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXT-X-MEDIA-SEQUENCE:100\n"
        "#EXTINF:6.0,\ns0.ts\n"
        "#EXTINF:6.0,\ns1.ts\n"
        "#EXTINF:6.0,\ns2.ts\n"
        "#EXTINF:6.0,\ns3.ts\n"
        "#EXTINF:6.0,\ns4.ts\n";
    const char *path = "tests/prefetch-output.ts";
    MiniIptvChannel test_channel = channel();
    MiniIptvStageInfo info;
    FILE *file;

    media_manifest = manifest;
    bad_segment = 0;
    observed_segment_limit = 0;
    remove(path);
    assert(miniiptv_stage_hls(&test_channel, path, mock_fetch, &info) == MINIIPTV_STAGE_OK);
    assert(info.segments_staged == 3);
    assert(info.bytes_staged == 3 * 376);
    assert(info.duration_staged == 18.0);
    assert(info.first_sequence == 101);
    assert(info.last_sequence == 103);
    assert(info.segment_limit_bytes == MINIIPTV_SEGMENT_LIMIT);
    assert(info.attempted_segment_bytes == 376u);
    assert(info.reported_segment_bytes == 376u);
    assert(observed_segment_limit == MINIIPTV_SEGMENT_LIMIT);
    file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    assert(ftell(file) == (long)(3 * 376));
    fclose(file);
    remove(path);
}

static void test_unsupported_encryption(void) {
    static const char manifest[] =
        "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n"
        "#EXTINF:5,\ns0.ts\n#EXTINF:5,\ns1.ts\n";
    MiniIptvChannel test_channel = channel();
    MiniIptvStageInfo info;

    media_manifest = manifest;
    assert(miniiptv_stage_hls(&test_channel, "tests/prefetch-output.ts",
                              mock_fetch, &info) == MINIIPTV_STAGE_UNSUPPORTED_HLS);
}

static void test_internal_discontinuity(void) {
    static const char manifest[] =
        "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXTINF:5,\ns0.ts\n#EXTINF:5,\ns1.ts\n"
        "#EXT-X-DISCONTINUITY\n#EXTINF:5,\ns2.ts\n"
        "#EXTINF:5,\ns3.ts\n#EXTINF:5,\ns4.ts\n";
    MiniIptvChannel test_channel = channel();
    MiniIptvStageInfo info;

    media_manifest = manifest;
    assert(miniiptv_stage_hls(&test_channel, "tests/prefetch-output.ts",
                              mock_fetch, &info) == MINIIPTV_STAGE_DISCONTINUITY);
}

static void test_bad_transport_stream_is_removed(void) {
    static const char manifest[] =
        "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n"
        "#EXTINF:5,\ns0.ts\n#EXTINF:5,\ns1.ts\n#EXT-X-ENDLIST\n";
    const char *path = "tests/prefetch-output.ts";
    MiniIptvChannel test_channel = channel();
    MiniIptvStageInfo info;
    FILE *file;

    media_manifest = manifest;
    bad_segment = 1;
    assert(miniiptv_stage_hls(&test_channel, path, mock_fetch, &info) == MINIIPTV_STAGE_NOT_MPEG_TS);
    file = fopen(path, "rb");
    assert(!file);
    bad_segment = 0;
}

static void test_master_skips_incompatible_lower_variant(void) {
    static const char manifest[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100000,RESOLUTION=320x180,CODECS=\"hvc1.1.6.L90,mp4a.40.2\"\n"
        "hevc/media.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360,CODECS=\"avc1.42c01e,mp4a.40.2\"\n"
        "h264/media.m3u8\n";
    HlsSelection selection;
    assert(hls_select_stream(manifest, "https://example.test/master.m3u8",
                             &selection) == 0);
    assert(selection.type == HLS_MASTER_PLAYLIST);
    assert(selection.bandwidth == 500000);
    assert(strcmp(selection.url,
                  "https://example.test/h264/media.m3u8") == 0);
}

int main(void) {
    test_success();
    test_unsupported_encryption();
    test_internal_discontinuity();
    test_bad_transport_stream_is_removed();
    test_master_skips_incompatible_lower_variant();
    puts("hls_prefetch tests passed");
    return 0;
}

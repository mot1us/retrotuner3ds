#include "miniiptv/channel_scan.h"

#include <stdio.h>
#include <string.h>

#define SCAN_MAX_WIDTH 640u
#define SCAN_MAX_HEIGHT 480u
#define SCAN_MAX_FRAME_RATE_MILLIHZ 30500u

static void copy_text(char *output, size_t output_size, const char *input) {
    if (!output || output_size == 0) return;
    snprintf(output, output_size, "%s", input ? input : "");
}

static int metadata_is_heavy(const MiniIptvScanResult *result) {
    return (result->width && result->width > SCAN_MAX_WIDTH) ||
           (result->height && result->height > SCAN_MAX_HEIGHT) ||
           (result->frame_rate_millihz &&
            result->frame_rate_millihz > SCAN_MAX_FRAME_RATE_MILLIHZ);
}

int miniiptv_channel_scan_classify_root(const char *manifest,
                                        const char *manifest_url,
                                        MiniIptvScanResult *result) {
    HlsSelection selection;
    int parse_result;
    if (!result) return 0;
    memset(result, 0, sizeof(*result));
    result->status = MINIIPTV_SCAN_UNSUPPORTED;
    parse_result = hls_select_stream(manifest, manifest_url, &selection);
    result->detail = parse_result;
    if (parse_result != 0) return 0;

    result->playlist_type = selection.type;
    result->bandwidth = selection.bandwidth;
    result->width = selection.width;
    result->height = selection.height;
    result->frame_rate_millihz = selection.frame_rate_millihz;
    result->has_separate_audio = selection.has_separate_audio;
    copy_text(result->codecs, sizeof(result->codecs), selection.codecs);
    copy_text(result->media_url, sizeof(result->media_url), selection.url);
    if (metadata_is_heavy(result)) {
        result->status = MINIIPTV_SCAN_HEAVY;
        return 0;
    }
    if (selection.type == HLS_MEDIA_PLAYLIST) {
        miniiptv_channel_scan_classify_media(manifest, manifest_url, result);
        return 0;
    }
    result->status = MINIIPTV_SCAN_CHECKING;
    return 1;
}

void miniiptv_channel_scan_classify_media(const char *manifest,
                                          const char *manifest_url,
                                          MiniIptvScanResult *result) {
    HlsMediaPlaylist media;
    int parse_result;
    if (!result) return;
    parse_result = hls_parse_media_playlist(manifest, manifest_url, &media);
    result->detail = parse_result;
    if (parse_result != 0) {
        result->status = MINIIPTV_SCAN_UNSUPPORTED;
        return;
    }
    result->target_duration = media.target_duration;
    if (media.encrypted || media.uses_byte_ranges || media.uses_init_map ||
        !media.is_live) {
        result->status = MINIIPTV_SCAN_UNSUPPORTED;
        return;
    }
    result->status = result->width && result->height && result->codecs[0]
        ? MINIIPTV_SCAN_READY : MINIIPTV_SCAN_UNKNOWN;
}

const char *miniiptv_channel_scan_status_label(MiniIptvScanStatus status) {
    switch (status) {
        case MINIIPTV_SCAN_CHECKING: return "CHECKING";
        case MINIIPTV_SCAN_READY: return "READY";
        case MINIIPTV_SCAN_UNKNOWN: return "MAYBE";
        case MINIIPTV_SCAN_HEAVY: return "TOO HEAVY";
        case MINIIPTV_SCAN_OFFLINE: return "OFF AIR";
        case MINIIPTV_SCAN_UNSUPPORTED: return "UNSUPPORTED";
        case MINIIPTV_SCAN_UNCHECKED:
        default: return "WAITING";
    }
}

#ifndef MINIIPTV_CHANNEL_SCAN_H
#define MINIIPTV_CHANNEL_SCAN_H

#include "miniiptv/hls.h"

typedef enum {
    MINIIPTV_SCAN_UNCHECKED = 0,
    MINIIPTV_SCAN_CHECKING,
    MINIIPTV_SCAN_READY,
    MINIIPTV_SCAN_UNKNOWN,
    MINIIPTV_SCAN_HEAVY,
    MINIIPTV_SCAN_OFFLINE,
    MINIIPTV_SCAN_UNSUPPORTED
} MiniIptvScanStatus;

typedef struct {
    MiniIptvScanStatus status;
    int detail;
    HlsPlaylistType playlist_type;
    unsigned long bandwidth;
    unsigned int width;
    unsigned int height;
    unsigned int frame_rate_millihz;
    unsigned int target_duration;
    int has_separate_audio;
    char codecs[96];
    char media_url[MINIIPTV_HLS_URL_MAX];
} MiniIptvScanResult;

/* Returns 1 when the selected media playlist must be fetched, otherwise 0. */
int miniiptv_channel_scan_classify_root(const char *manifest,
                                        const char *manifest_url,
                                        MiniIptvScanResult *result);
void miniiptv_channel_scan_classify_media(const char *manifest,
                                          const char *manifest_url,
                                          MiniIptvScanResult *result);
const char *miniiptv_channel_scan_status_label(MiniIptvScanStatus status);

#endif

#ifndef MINIIPTV_HLS_PREFETCH_H
#define MINIIPTV_HLS_PREFETCH_H

#include <stddef.h>

#include "miniiptv/hls.h"
#include "miniiptv/network.h"
#include "miniiptv/playlist.h"

#define MINIIPTV_PREFETCH_SEGMENTS 3
#define MINIIPTV_SEGMENT_LIMIT (4u * 1024u * 1024u)
#define MINIIPTV_PREFETCH_LIMIT (12u * 1024u * 1024u)

typedef int (*MiniIptvFetchFunction)(const char *url, const char *user_agent,
                                    const char *referrer, size_t maximum_size,
                                    NetworkTextResponse *response);

typedef enum {
    MINIIPTV_STAGE_OK = 0,
    MINIIPTV_STAGE_INVALID_ARGUMENT = -1,
    MINIIPTV_STAGE_MANIFEST_FETCH_FAILED = -2,
    MINIIPTV_STAGE_MASTER_INVALID = -3,
    MINIIPTV_STAGE_MEDIA_FETCH_FAILED = -4,
    MINIIPTV_STAGE_MEDIA_INVALID = -5,
    MINIIPTV_STAGE_UNSUPPORTED_HLS = -6,
    MINIIPTV_STAGE_SEGMENT_FETCH_FAILED = -7,
    MINIIPTV_STAGE_NOT_MPEG_TS = -8,
    MINIIPTV_STAGE_TOO_LARGE = -9,
    MINIIPTV_STAGE_FILE_FAILED = -10,
    MINIIPTV_STAGE_DISCONTINUITY = -11,
    MINIIPTV_STAGE_CANCELLED = -12,
    MINIIPTV_STAGE_TUNE_TIMEOUT = -13,
    MINIIPTV_STAGE_PLAYER_OPEN_TIMEOUT = -14,
    MINIIPTV_STAGE_MVD_INIT_TIMEOUT = -15,
    MINIIPTV_STAGE_FIRST_FRAME_TIMEOUT = -16
} MiniIptvStageResult;

typedef struct {
    size_t segments_staged;
    size_t bytes_staged;
    size_t attempted_segment_bytes;
    size_t reported_segment_bytes;
    size_t segment_limit_bytes;
    double duration_staged;
    unsigned long first_sequence;
    unsigned long last_sequence;
    int last_network_result;
    char media_url[MINIIPTV_HLS_URL_MAX];
} MiniIptvStageInfo;

int miniiptv_stage_hls(const MiniIptvChannel *channel, const char *output_path,
                       MiniIptvFetchFunction fetch, MiniIptvStageInfo *info);

#endif

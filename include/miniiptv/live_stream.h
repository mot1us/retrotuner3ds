#ifndef MINIIPTV_LIVE_STREAM_H
#define MINIIPTV_LIVE_STREAM_H

#include <stddef.h>

#include "miniiptv/hls_prefetch.h"
#include "miniiptv/playlist.h"

#define MINIIPTV_LIVE_STREAM_URL "miniiptv://live.ts"

typedef struct {
    char channel_name[MINIIPTV_NAME_MAX];
    char codecs[96];
    unsigned long bandwidth;
    unsigned long measured_bandwidth;
    unsigned long network_bandwidth;
    size_t last_segment_bytes;
    size_t attempted_segment_bytes;
    size_t reported_segment_bytes;
    size_t segment_limit_bytes;
    unsigned int last_download_milliseconds;
    unsigned int last_segment_milliseconds;
    size_t rebuffer_target_bytes;
    unsigned int buffered_milliseconds;
    unsigned int width;
    unsigned int height;
    int last_network_result;
    int rebuffering;
} MiniIptvLiveInfo;

int miniiptv_live_stream_start(const MiniIptvChannel *channel,
                               MiniIptvStageInfo *initial_info);
void miniiptv_live_stream_request_stop(void);
void miniiptv_live_stream_stop(void);
int miniiptv_live_stream_is_active(void);
int miniiptv_live_stream_read(unsigned char *buffer, int buffer_size);
void miniiptv_live_stream_get_stats(size_t *buffered_bytes,
                                    unsigned long *downloaded_segments,
                                    unsigned long *read_kib,
                                    unsigned long *underruns,
                                    int *last_error);
void miniiptv_live_stream_get_info(MiniIptvLiveInfo *info);

#endif

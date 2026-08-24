#ifndef MINIIPTV_LIVE_STREAM_H
#define MINIIPTV_LIVE_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include "miniiptv/buffer_shadow.h"
#include "miniiptv/hls_prefetch.h"
#include "miniiptv/playlist.h"

#define MINIIPTV_LIVE_STREAM_URL "miniiptv://live.ts"

typedef enum {
    MINIIPTV_TUNE_PHASE_IDLE = 0,
    MINIIPTV_TUNE_PHASE_OLD_STREAM_CLEANUP,
    MINIIPTV_TUNE_PHASE_ROOT_MANIFEST,
    MINIIPTV_TUNE_PHASE_MEDIA_MANIFEST,
    MINIIPTV_TUNE_PHASE_INITIAL_SEGMENT,
    MINIIPTV_TUNE_PHASE_PLAYER_OPEN,
    MINIIPTV_TUNE_PHASE_MVD_INIT,
    MINIIPTV_TUNE_PHASE_FIRST_FRAME,
    MINIIPTV_TUNE_PHASE_READY,
    MINIIPTV_TUNE_PHASE_FAILED,
    MINIIPTV_TUNE_PHASE_COUNT
} MiniIptvTunePhase;

typedef enum {
    MINIIPTV_PRODUCER_STOPPED = 0,
    MINIIPTV_PRODUCER_STARTING,
    MINIIPTV_PRODUCER_PLAYLIST,
    MINIIPTV_PRODUCER_SEGMENT,
    MINIIPTV_PRODUCER_LIVE_EDGE,
    MINIIPTV_PRODUCER_RING_HIGH,
    MINIIPTV_PRODUCER_ERROR
} MiniIptvProducerState;

typedef struct {
    MiniIptvTunePhase phase;
    MiniIptvTunePhase failure_phase;
    unsigned int phase_milliseconds[MINIIPTV_TUNE_PHASE_COUNT];
    unsigned int phase_elapsed_milliseconds;
    unsigned int total_elapsed_milliseconds;
    size_t initial_segment_received_bytes;
    size_t initial_segment_reported_bytes;
    int result;
    int phase_active;
} MiniIptvTuneTelemetry;

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
    int rendition_cache_hit;
    MiniIptvProducerState producer_state;
    MiniIptvBufferShadowSnapshot shadow;
} MiniIptvLiveInfo;

int miniiptv_live_stream_start(const MiniIptvChannel *channel,
                               MiniIptvStageInfo *initial_info,
                               MiniIptvCancelFunction should_cancel,
                               void *cancel_userdata);
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

/* A tune timeline is independent of LiveStream so stop/reset cannot erase the
 * phase that failed. These calls are safe across the network, decoder and draw
 * threads; the snapshot intentionally contains no channel URLs. */
void miniiptv_live_tune_telemetry_init(void);
void miniiptv_live_tune_reset(void);
void miniiptv_live_tune_phase_begin(MiniIptvTunePhase phase);
void miniiptv_live_tune_phase_complete(MiniIptvTunePhase phase);
void miniiptv_live_tune_fail(int result);
void miniiptv_live_tune_segment_progress(size_t received_bytes,
                                         size_t reported_bytes);
void miniiptv_live_tune_get_telemetry(MiniIptvTuneTelemetry *telemetry);
const char *miniiptv_live_tune_phase_label(MiniIptvTunePhase phase);
const char *miniiptv_live_producer_state_label(MiniIptvProducerState state);

#endif

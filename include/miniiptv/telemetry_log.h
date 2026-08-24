#ifndef MINIIPTV_TELEMETRY_LOG_H
#define MINIIPTV_TELEMETRY_LOG_H

#include <stdbool.h>
#include <stdint.h>

#define MINIIPTV_TELEMETRY_LOG_MAX_BYTES (512u * 1024u)

typedef struct {
    const char *channel_name;
    const char *app_state;
    const char *tune_phase;
    const char *shadow_state;
    const char *producer_state;
    uint64_t ring_bytes;
    uint64_t downloaded_segments;
    uint64_t global_underruns;
    uint32_t width;
    uint32_t height;
    uint32_t buffered_ms;
    uint32_t network_bps;
    uint32_t content_bps;
    uint32_t valid_samples;
    uint32_t headroom_permille;
    uint32_t desired_reserve_ms;
    uint32_t recommended_lag_segments;
    uint32_t segment_ms;
    uint32_t download_ms;
    uint32_t commit_gap_ms;
    uint32_t commit_gap_deviation_ms;
    uint32_t ring_min_bytes;
    uint32_t ring_max_bytes;
    uint32_t recent_underruns;
    uint32_t total_underruns;
    uint32_t no_new_poll_streak;
    uint32_t last_refill_ms;
    uint32_t last_refill_commits;
    int64_t last_error;
    bool rebuffering;
    bool periodic;
} MiniIptvTelemetrySample;

/* The logger owns one bounded, replace-on-launch CSV. Calls are expected from
 * the app/draw owner, never the network or decoder hot paths. */
int miniiptv_telemetry_log_open(const char *path, const char *version,
                                uint64_t now_ms);
void miniiptv_telemetry_log_record(
    uint64_t now_ms, const MiniIptvTelemetrySample *sample);
void miniiptv_telemetry_log_close(void);
bool miniiptv_telemetry_log_is_enabled(void);

#endif

#ifndef MINIIPTV_BUFFER_SHADOW_H
#define MINIIPTV_BUFFER_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#define MINIIPTV_BUFFER_SHADOW_UNDERRUN_SLOTS 4u

typedef enum {
    MINIIPTV_BUFFER_SHADOW_COLD = 0,
    MINIIPTV_BUFFER_SHADOW_HEALTHY,
    MINIIPTV_BUFFER_SHADOW_AT_RISK,
    MINIIPTV_BUFFER_SHADOW_MARGINAL,
    MINIIPTV_BUFFER_SHADOW_REFILL,
    MINIIPTV_BUFFER_SHADOW_UNSUSTAINABLE
} MiniIptvBufferShadowState;

/* Fixed-size observation state. The owner supplies clocks and synchronization;
 * this helper allocates nothing and has no playback or network side effects. */
typedef struct {
    uint32_t valid_samples;
    uint32_t content_bps_ewma;
    uint32_t network_bps_ewma;
    uint32_t segment_ms_ewma;
    uint32_t download_ms_ewma;
    uint32_t commit_gap_ms_ewma;
    uint32_t commit_gap_deviation_ms;
    uint32_t headroom_permille_ewma;
    uint32_t target_duration_ms;
    uint32_t largest_segment_bytes;
    uint32_t ring_current_bytes;
    uint32_t ring_min_bytes;
    uint32_t ring_max_bytes;
    uint32_t segment_samples;
    uint32_t network_samples;
    uint32_t commit_gap_samples;
    uint32_t no_new_poll_total;
    uint32_t no_new_poll_streak;
    uint32_t total_underruns;
    uint32_t last_refill_ms;
    uint32_t total_refill_ms;
    uint32_t current_refill_commits;
    uint32_t last_refill_commits;
    uint32_t planned_reserve_ms;
    uint32_t planned_reserve_bytes;
    uint64_t last_commit_ms;
    uint64_t refill_started_ms;
    uint64_t underrun_times_ms[MINIIPTV_BUFFER_SHADOW_UNDERRUN_SLOTS];
    uint8_t underrun_count;
    uint8_t underrun_next;
    bool last_commit_valid;
    bool ring_seen;
    bool reader_started;
    bool risk_latched;
    bool gap_disqualified;
    bool refill_active;
} MiniIptvBufferShadow;

typedef struct {
    MiniIptvBufferShadowState state;
    uint32_t valid_samples;
    uint32_t content_bps;
    uint32_t network_bps;
    uint32_t segment_ms;
    uint32_t download_ms;
    uint32_t commit_gap_ms;
    uint32_t commit_gap_deviation_ms;
    uint32_t headroom_permille;
    uint32_t desired_reserve_ms;
    uint32_t desired_reserve_bytes;
    uint32_t buffered_ms;
    uint32_t ring_current_bytes;
    uint32_t ring_min_bytes;
    uint32_t ring_max_bytes;
    uint32_t no_new_poll_total;
    uint32_t no_new_poll_streak;
    uint32_t recent_underruns;
    uint32_t total_underruns;
    uint32_t last_refill_ms;
    uint32_t total_refill_ms;
    uint32_t last_refill_commits;
    uint8_t recommended_lag_segments;
} MiniIptvBufferShadowSnapshot;

typedef struct {
    uint32_t target_bytes;
    uint32_t maximum_wait_ms;
} MiniIptvRecoveryPlan;

void miniiptv_buffer_shadow_reset(MiniIptvBufferShadow *shadow,
                                  uint32_t target_duration_ms);
void miniiptv_buffer_shadow_set_target(MiniIptvBufferShadow *shadow,
                                       uint32_t target_duration_ms,
                                       uint64_t now_ms);
void miniiptv_buffer_shadow_note_high_water(MiniIptvBufferShadow *shadow);
void miniiptv_buffer_shadow_note_segment_published(
    MiniIptvBufferShadow *shadow);
/* duration_ms==0 omits content/headroom sampling; download_ms==0 omits
 * network/headroom sampling. The caller maps a valid same-tick download to
 * 1 ms and reserves zero for a rejected/non-monotonic clock sample. */
void miniiptv_buffer_shadow_note_segment(MiniIptvBufferShadow *shadow,
                                         uint32_t segment_bytes,
                                         uint32_t duration_ms,
                                         uint32_t download_ms,
                                         uint64_t commit_ms);
void miniiptv_buffer_shadow_note_ring(MiniIptvBufferShadow *shadow,
                                      uint32_t ring_bytes,
                                      bool reader_started);
void miniiptv_buffer_shadow_note_playlist_poll(MiniIptvBufferShadow *shadow,
                                               bool found_new_segment);
void miniiptv_buffer_shadow_begin_refill(MiniIptvBufferShadow *shadow,
                                         uint64_t now_ms);
void miniiptv_buffer_shadow_end_refill(MiniIptvBufferShadow *shadow,
                                       uint64_t now_ms);
void miniiptv_buffer_shadow_snapshot(const MiniIptvBufferShadow *shadow,
                                     uint64_t now_ms,
                                     bool rebuffering,
                                     MiniIptvBufferShadowSnapshot *snapshot);
void miniiptv_buffer_shadow_recovery_plan(
    const MiniIptvBufferShadowSnapshot *snapshot,
    uint32_t last_segment_bytes,
    uint32_t target_duration_ms,
    MiniIptvRecoveryPlan *plan);
const char *miniiptv_buffer_shadow_state_label(
    MiniIptvBufferShadowState state);

#endif

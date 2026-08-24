#include "miniiptv/buffer_shadow.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "miniiptv/stream_limits.h"

#define SHADOW_DEFAULT_TARGET_MS 6000u
#define SHADOW_RECENT_UNDERRUN_MS 90000u
#define SHADOW_MAX_COMMIT_GAP_MS 300000u
#define SHADOW_SEGMENT_RESERVE_MIN_BYTES (1u * 1024u * 1024u)
#define SHADOW_SEGMENT_RESERVE_MAX_BYTES \
    MINIIPTV_STREAM_ATOMIC_SEGMENT_LIMIT_BYTES
#define SHADOW_RESERVE_GUARD_BYTES (64u * 1024u)
#define SHADOW_DESIRED_MIN_BYTES (128u * 1024u)
#define SHADOW_DESIRED_MAX_BYTES (3u * 1024u * 1024u)
#define SHADOW_DESIRED_MIN_MS 2500u
#define SHADOW_DESIRED_MAX_MS 12000u

static uint32_t saturating_add_u32(uint32_t left, uint32_t right) {
    return right > UINT32_MAX - left ? UINT32_MAX : left + right;
}

static uint32_t clamp_u64_u32(uint64_t value) {
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint32_t ewma_quarter(uint32_t average, uint32_t sample) {
    return (uint32_t)(((uint64_t)average * 3u + sample + 2u) / 4u);
}

static uint32_t recent_underruns(const MiniIptvBufferShadow *shadow,
                                 uint64_t now_ms) {
    uint32_t recent = 0;
    uint8_t index;
    if (!shadow) return 0;
    for (index = 0; index < shadow->underrun_count; index++) {
        uint64_t when = shadow->underrun_times_ms[index];
        if (now_ms >= when
        && now_ms - when <= SHADOW_RECENT_UNDERRUN_MS)
            recent++;
    }
    return recent;
}

static uint32_t desired_reserve_ms(const MiniIptvBufferShadow *shadow,
                                   uint64_t now_ms) {
    uint32_t fallback;
    uint32_t base;
    uint32_t penalty;
    uint32_t underrun_penalty;
    uint64_t desired;
    if (!shadow) return SHADOW_DEFAULT_TARGET_MS;
    fallback = shadow->target_duration_ms > 0
        ? shadow->target_duration_ms : SHADOW_DEFAULT_TARGET_MS;
    base = shadow->segment_ms_ewma > 0
        ? shadow->segment_ms_ewma : fallback;
    if ((shadow->commit_gap_samples > 0
        ? shadow->commit_gap_ms_ewma : fallback) > base)
        base = shadow->commit_gap_samples > 0
            ? shadow->commit_gap_ms_ewma : fallback;

    if (shadow->valid_samples == 0
    || shadow->headroom_permille_ewma >= 2000u)
        penalty = 0;
    else if (shadow->headroom_permille_ewma >= 1400u)
        penalty = 500u;
    else if (shadow->headroom_permille_ewma >= 1050u)
        penalty = 1500u;
    else
        penalty = 3000u;
    underrun_penalty = recent_underruns(shadow, now_ms) * 1000u;
    if (underrun_penalty > 4000u) underrun_penalty = 4000u;
    desired = (uint64_t)base
        + (uint64_t)shadow->commit_gap_deviation_ms * 2u
        + 500u + penalty + underrun_penalty;
    if (desired < SHADOW_DESIRED_MIN_MS) desired = SHADOW_DESIRED_MIN_MS;
    if (desired > SHADOW_DESIRED_MAX_MS) desired = SHADOW_DESIRED_MAX_MS;
    return (uint32_t)desired;
}

static uint32_t desired_reserve_bytes(const MiniIptvBufferShadow *shadow,
                                      uint32_t desired_ms) {
    uint32_t largest;
    uint32_t safe_max;
    uint64_t desired;
    if (!shadow) return SHADOW_DESIRED_MIN_BYTES;
    largest = shadow->largest_segment_bytes;
    if (largest < SHADOW_SEGMENT_RESERVE_MIN_BYTES)
        largest = SHADOW_SEGMENT_RESERVE_MIN_BYTES;
    if (largest > SHADOW_SEGMENT_RESERVE_MAX_BYTES)
        largest = SHADOW_SEGMENT_RESERVE_MAX_BYTES;
    safe_max = MINIIPTV_STREAM_RING_CAPACITY_BYTES - largest
        - SHADOW_RESERVE_GUARD_BYTES;
    if (safe_max > SHADOW_DESIRED_MAX_BYTES)
        safe_max = SHADOW_DESIRED_MAX_BYTES;
    desired = ((uint64_t)shadow->content_bps_ewma * desired_ms) / 8000u;
    if (desired < SHADOW_DESIRED_MIN_BYTES)
        desired = SHADOW_DESIRED_MIN_BYTES;
    if (desired > safe_max) desired = safe_max;
    return (uint32_t)desired;
}

static void update_risk_latch(MiniIptvBufferShadow *shadow,
                              uint32_t desired_bytes) {
    if (!shadow || !shadow->reader_started || shadow->valid_samples < 3u)
        return;
    if (!shadow->risk_latched
    && (uint64_t)shadow->ring_current_bytes * 4u
        < (uint64_t)desired_bytes * 3u)
        shadow->risk_latched = true;
    else if (shadow->risk_latched
    && shadow->ring_current_bytes >= desired_bytes)
        shadow->risk_latched = false;
}

static void refresh_plan(MiniIptvBufferShadow *shadow, uint64_t now_ms) {
    if (!shadow) return;
    shadow->planned_reserve_ms = desired_reserve_ms(shadow, now_ms);
    shadow->planned_reserve_bytes = desired_reserve_bytes(
        shadow, shadow->planned_reserve_ms);
    update_risk_latch(shadow, shadow->planned_reserve_bytes);
}

void miniiptv_buffer_shadow_reset(MiniIptvBufferShadow *shadow,
                                  uint32_t target_duration_ms) {
    if (!shadow) return;
    memset(shadow, 0, sizeof(*shadow));
    shadow->target_duration_ms = target_duration_ms > 0
        ? target_duration_ms : SHADOW_DEFAULT_TARGET_MS;
    refresh_plan(shadow, 0);
}

void miniiptv_buffer_shadow_set_target(MiniIptvBufferShadow *shadow,
                                       uint32_t target_duration_ms,
                                       uint64_t now_ms) {
    if (!shadow || target_duration_ms == 0) return;
    shadow->target_duration_ms = target_duration_ms;
    refresh_plan(shadow, now_ms);
}

void miniiptv_buffer_shadow_note_high_water(MiniIptvBufferShadow *shadow) {
    if (!shadow) return;
    shadow->gap_disqualified = true;
}

void miniiptv_buffer_shadow_note_segment_published(
    MiniIptvBufferShadow *shadow) {
    if (!shadow || !shadow->refill_active) return;
    shadow->current_refill_commits = saturating_add_u32(
        shadow->current_refill_commits, 1u);
}

void miniiptv_buffer_shadow_note_segment(MiniIptvBufferShadow *shadow,
                                         uint32_t segment_bytes,
                                         uint32_t duration_ms,
                                         uint32_t download_ms,
                                         uint64_t commit_ms) {
    uint32_t sample;
    if (!shadow) return;
    if (segment_bytes > shadow->largest_segment_bytes)
        shadow->largest_segment_bytes = segment_bytes;

    if (duration_ms > 0) {
        sample = clamp_u64_u32(((uint64_t)segment_bytes * 8000u)
                               / duration_ms);
        if (shadow->segment_samples == 0) {
            shadow->content_bps_ewma = sample;
            shadow->segment_ms_ewma = duration_ms;
        }
        else {
            shadow->content_bps_ewma = ewma_quarter(
                shadow->content_bps_ewma, sample);
            shadow->segment_ms_ewma = ewma_quarter(
                shadow->segment_ms_ewma, duration_ms);
        }
        shadow->segment_samples = saturating_add_u32(
            shadow->segment_samples, 1u);
    }
    if (download_ms > 0) {
        sample = clamp_u64_u32(((uint64_t)segment_bytes * 8000u)
                               / download_ms);
        if (shadow->network_samples == 0) {
            shadow->network_bps_ewma = sample;
            shadow->download_ms_ewma = download_ms;
        }
        else {
            shadow->network_bps_ewma = ewma_quarter(
                shadow->network_bps_ewma, sample);
            shadow->download_ms_ewma = ewma_quarter(
                shadow->download_ms_ewma, download_ms);
        }
        shadow->network_samples = saturating_add_u32(
            shadow->network_samples, 1u);
    }
    if (duration_ms > 0 && download_ms > 0) {
        sample = clamp_u64_u32(((uint64_t)duration_ms * 1000u)
                               / download_ms);
        if (sample > 10000u) sample = 10000u;
        if (shadow->valid_samples == 0)
            shadow->headroom_permille_ewma = sample;
        else
            shadow->headroom_permille_ewma = ewma_quarter(
                shadow->headroom_permille_ewma, sample);
        shadow->valid_samples = saturating_add_u32(
            shadow->valid_samples, 1u);
    }

    if (shadow->last_commit_valid) {
        if (shadow->gap_disqualified || commit_ms < shadow->last_commit_ms
        || commit_ms - shadow->last_commit_ms > SHADOW_MAX_COMMIT_GAP_MS) {
            shadow->gap_disqualified = false;
        } else {
            uint64_t elapsed = commit_ms - shadow->last_commit_ms;
            uint32_t gap = (uint32_t)elapsed;
            uint32_t old_gap = shadow->commit_gap_ms_ewma;
            uint32_t deviation = old_gap > gap ? old_gap - gap : gap - old_gap;
            if (shadow->commit_gap_samples == 0)
                shadow->commit_gap_ms_ewma = gap;
            else {
                shadow->commit_gap_ms_ewma = ewma_quarter(old_gap, gap);
                shadow->commit_gap_deviation_ms = ewma_quarter(
                    shadow->commit_gap_deviation_ms, deviation);
            }
            shadow->commit_gap_samples = saturating_add_u32(
                shadow->commit_gap_samples, 1u);
        }
    }
    shadow->last_commit_ms = commit_ms;
    shadow->last_commit_valid = true;
    refresh_plan(shadow, commit_ms);
}

void miniiptv_buffer_shadow_note_ring(MiniIptvBufferShadow *shadow,
                                      uint32_t ring_bytes,
                                      bool reader_started) {
    if (!shadow) return;
    if (ring_bytes > MINIIPTV_STREAM_RING_CAPACITY_BYTES)
        ring_bytes = MINIIPTV_STREAM_RING_CAPACITY_BYTES;
    shadow->ring_current_bytes = ring_bytes;
    if (ring_bytes > shadow->ring_max_bytes)
        shadow->ring_max_bytes = ring_bytes;
    if (reader_started) {
        shadow->reader_started = true;
        if (!shadow->ring_seen || ring_bytes < shadow->ring_min_bytes)
            shadow->ring_min_bytes = ring_bytes;
        shadow->ring_seen = true;
    }
    update_risk_latch(shadow, shadow->planned_reserve_bytes);
}

void miniiptv_buffer_shadow_note_playlist_poll(MiniIptvBufferShadow *shadow,
                                               bool found_new_segment) {
    if (!shadow) return;
    if (found_new_segment) {
        shadow->no_new_poll_streak = 0;
        return;
    }
    shadow->no_new_poll_total = saturating_add_u32(
        shadow->no_new_poll_total, 1u);
    shadow->no_new_poll_streak = saturating_add_u32(
        shadow->no_new_poll_streak, 1u);
}

void miniiptv_buffer_shadow_begin_refill(MiniIptvBufferShadow *shadow,
                                         uint64_t now_ms) {
    uint8_t slot;
    if (!shadow || shadow->refill_active) return;
    shadow->refill_active = true;
    shadow->refill_started_ms = now_ms;
    shadow->current_refill_commits = 0;
    shadow->total_underruns = saturating_add_u32(
        shadow->total_underruns, 1u);
    slot = shadow->underrun_next;
    shadow->underrun_times_ms[slot] = now_ms;
    shadow->underrun_next = (uint8_t)((slot + 1u)
        % MINIIPTV_BUFFER_SHADOW_UNDERRUN_SLOTS);
    if (shadow->underrun_count < MINIIPTV_BUFFER_SHADOW_UNDERRUN_SLOTS)
        shadow->underrun_count++;
    refresh_plan(shadow, now_ms);
}

void miniiptv_buffer_shadow_end_refill(MiniIptvBufferShadow *shadow,
                                       uint64_t now_ms) {
    uint32_t elapsed = 0;
    if (!shadow || !shadow->refill_active) return;
    if (now_ms >= shadow->refill_started_ms)
        elapsed = clamp_u64_u32(now_ms - shadow->refill_started_ms);
    shadow->last_refill_ms = elapsed;
    shadow->total_refill_ms = saturating_add_u32(
        shadow->total_refill_ms, elapsed);
    shadow->last_refill_commits = shadow->current_refill_commits;
    shadow->current_refill_commits = 0;
    shadow->refill_active = false;
    refresh_plan(shadow, now_ms);
}

void miniiptv_buffer_shadow_snapshot(const MiniIptvBufferShadow *shadow,
                                     uint64_t now_ms,
                                     bool rebuffering,
                                     MiniIptvBufferShadowSnapshot *snapshot) {
    uint32_t recent;
    uint32_t desired_ms;
    uint32_t desired_bytes;
    uint64_t buffered_ms = 0;
    bool risk_latched;
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!shadow) return;
    recent = recent_underruns(shadow, now_ms);
    desired_ms = desired_reserve_ms(shadow, now_ms);
    desired_bytes = desired_reserve_bytes(shadow, desired_ms);
    risk_latched = shadow->risk_latched;
    if (shadow->reader_started && shadow->valid_samples >= 3u) {
        if (!risk_latched
        && (uint64_t)shadow->ring_current_bytes * 4u
            < (uint64_t)desired_bytes * 3u)
            risk_latched = true;
        else if (risk_latched
        && shadow->ring_current_bytes >= desired_bytes)
            risk_latched = false;
    }

    if (rebuffering || shadow->refill_active)
        snapshot->state = MINIIPTV_BUFFER_SHADOW_REFILL;
    else if (shadow->valid_samples >= 3u
    && shadow->headroom_permille_ewma < 1050u)
        snapshot->state = MINIIPTV_BUFFER_SHADOW_UNSUSTAINABLE;
    else if (risk_latched)
        snapshot->state = MINIIPTV_BUFFER_SHADOW_AT_RISK;
    else if (shadow->valid_samples < 3u)
        snapshot->state = MINIIPTV_BUFFER_SHADOW_COLD;
    else if (shadow->headroom_permille_ewma < 1400u
    || (uint64_t)shadow->commit_gap_deviation_ms * 4u
        > shadow->segment_ms_ewma
    || recent > 0)
        snapshot->state = MINIIPTV_BUFFER_SHADOW_MARGINAL;
    else
        snapshot->state = MINIIPTV_BUFFER_SHADOW_HEALTHY;

    if (snapshot->state == MINIIPTV_BUFFER_SHADOW_UNSUSTAINABLE
    || recent >= 2u)
        snapshot->recommended_lag_segments = 3;
    else if (snapshot->state == MINIIPTV_BUFFER_SHADOW_HEALTHY)
        snapshot->recommended_lag_segments = 1;
    else
        snapshot->recommended_lag_segments = 2;

    if (shadow->content_bps_ewma > 0)
        buffered_ms = ((uint64_t)shadow->ring_current_bytes * 8000u)
            / shadow->content_bps_ewma;
    snapshot->valid_samples = shadow->valid_samples;
    snapshot->content_bps = shadow->content_bps_ewma;
    snapshot->network_bps = shadow->network_bps_ewma;
    snapshot->segment_ms = shadow->segment_ms_ewma;
    snapshot->download_ms = shadow->download_ms_ewma;
    snapshot->commit_gap_ms = shadow->commit_gap_ms_ewma;
    snapshot->commit_gap_deviation_ms =
        shadow->commit_gap_deviation_ms;
    snapshot->headroom_permille = shadow->headroom_permille_ewma;
    snapshot->desired_reserve_ms = desired_ms;
    snapshot->desired_reserve_bytes = desired_bytes;
    snapshot->buffered_ms = clamp_u64_u32(buffered_ms);
    snapshot->ring_current_bytes = shadow->ring_current_bytes;
    snapshot->ring_min_bytes = shadow->ring_seen
        ? shadow->ring_min_bytes : 0;
    snapshot->ring_max_bytes = shadow->ring_max_bytes;
    snapshot->no_new_poll_total = shadow->no_new_poll_total;
    snapshot->no_new_poll_streak = shadow->no_new_poll_streak;
    snapshot->recent_underruns = recent;
    snapshot->total_underruns = shadow->total_underruns;
    snapshot->last_refill_ms = shadow->last_refill_ms;
    snapshot->total_refill_ms = shadow->total_refill_ms;
    snapshot->last_refill_commits = shadow->last_refill_commits;
}

const char *miniiptv_buffer_shadow_state_label(
    MiniIptvBufferShadowState state) {
    switch (state) {
        case MINIIPTV_BUFFER_SHADOW_HEALTHY: return "GOOD";
        case MINIIPTV_BUFFER_SHADOW_AT_RISK: return "RISK";
        case MINIIPTV_BUFFER_SHADOW_MARGINAL: return "MARG";
        case MINIIPTV_BUFFER_SHADOW_REFILL: return "FILL";
        case MINIIPTV_BUFFER_SHADOW_UNSUSTAINABLE: return "SLOW";
        case MINIIPTV_BUFFER_SHADOW_COLD:
        default: return "COLD";
    }
}

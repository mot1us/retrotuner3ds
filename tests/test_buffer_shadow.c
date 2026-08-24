#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "miniiptv/buffer_shadow.h"
#include "miniiptv/stream_limits.h"

#define KIB(value) ((uint32_t)(value) * 1024u)

static MiniIptvBufferShadowSnapshot snapshot_at(
    MiniIptvBufferShadow *shadow, uint64_t now_ms, int rebuffering) {
    MiniIptvBufferShadowSnapshot snapshot;
    miniiptv_buffer_shadow_snapshot(shadow, now_ms, rebuffering != 0,
                                    &snapshot);
    return snapshot;
}

static void add_sample(MiniIptvBufferShadow *shadow, uint64_t commit_ms,
                       uint32_t duration_ms, uint32_t download_ms) {
    miniiptv_buffer_shadow_note_segment(shadow, KIB(256), duration_ms,
                                        download_ms, commit_ms);
    miniiptv_buffer_shadow_note_ring(shadow, KIB(2048), true);
}

static void test_cold_and_known_sample(void) {
    MiniIptvBufferShadow shadow;
    MiniIptvBufferShadowSnapshot value;
    miniiptv_buffer_shadow_reset(&shadow, 6000);
    value = snapshot_at(&shadow, 0, 0);
    assert(value.state == MINIIPTV_BUFFER_SHADOW_COLD);
    assert(value.recommended_lag_segments == 2);
    assert(value.desired_reserve_ms == 6500);

    add_sample(&shadow, 1000, 4000, 1000);
    value = snapshot_at(&shadow, 1000, 0);
    assert(value.valid_samples == 1);
    assert(value.content_bps == 524288u);
    assert(value.network_bps == 2097152u);
    assert(value.headroom_permille == 4000u);
    assert(value.state == MINIIPTV_BUFFER_SHADOW_COLD);
    assert(value.desired_reserve_ms == 6500u);
}

static void test_states_and_risk_hysteresis(void) {
    MiniIptvBufferShadow shadow;
    MiniIptvBufferShadowSnapshot value;
    uint32_t target;
    miniiptv_buffer_shadow_reset(&shadow, 4000);
    add_sample(&shadow, 1000, 4000, 1000);
    add_sample(&shadow, 5000, 4000, 1000);
    add_sample(&shadow, 9000, 4000, 1000);
    value = snapshot_at(&shadow, 9000, 0);
    assert(value.state == MINIIPTV_BUFFER_SHADOW_HEALTHY);
    assert(value.recommended_lag_segments == 1);
    target = value.desired_reserve_bytes;

    miniiptv_buffer_shadow_note_ring(&shadow, (target * 3u) / 4u, true);
    value = snapshot_at(&shadow, 9001, 0);
    assert(value.state == MINIIPTV_BUFFER_SHADOW_HEALTHY);
    miniiptv_buffer_shadow_note_ring(&shadow,
                                     (target * 3u) / 4u - 1u, true);
    value = snapshot_at(&shadow, 9002, 0);
    assert(value.state == MINIIPTV_BUFFER_SHADOW_AT_RISK);
    miniiptv_buffer_shadow_note_ring(&shadow, target - 1u, true);
    assert(snapshot_at(&shadow, 9003, 0).state
           == MINIIPTV_BUFFER_SHADOW_AT_RISK);
    miniiptv_buffer_shadow_note_ring(&shadow, target, true);
    assert(snapshot_at(&shadow, 9004, 0).state
           == MINIIPTV_BUFFER_SHADOW_HEALTHY);

    miniiptv_buffer_shadow_reset(&shadow, 4000);
    add_sample(&shadow, 1000, 4000, 3200);
    add_sample(&shadow, 5000, 4000, 3200);
    add_sample(&shadow, 9000, 4000, 3200);
    assert(snapshot_at(&shadow, 9000, 0).state
           == MINIIPTV_BUFFER_SHADOW_MARGINAL);

    miniiptv_buffer_shadow_reset(&shadow, 4000);
    add_sample(&shadow, 1000, 4000, 4100);
    add_sample(&shadow, 5000, 4000, 4100);
    add_sample(&shadow, 9000, 4000, 4100);
    value = snapshot_at(&shadow, 9000, 0);
    assert(value.state == MINIIPTV_BUFFER_SHADOW_UNSUSTAINABLE);
    assert(value.recommended_lag_segments == 3);
}

static void test_gap_and_high_water_suppression(void) {
    MiniIptvBufferShadow shadow;
    MiniIptvBufferShadowSnapshot value;
    miniiptv_buffer_shadow_reset(&shadow, 4000);
    add_sample(&shadow, 1000, 4000, 1000);
    add_sample(&shadow, 5000, 4000, 1000);
    value = snapshot_at(&shadow, 5000, 0);
    assert(value.commit_gap_ms == 4000u);
    miniiptv_buffer_shadow_note_high_water(&shadow);
    add_sample(&shadow, 25000, 4000, 1000);
    assert(snapshot_at(&shadow, 25000, 0).commit_gap_ms == 4000u);
    add_sample(&shadow, 29000, 4000, 1000);
    assert(snapshot_at(&shadow, 29000, 0).commit_gap_ms == 4000u);

    /* Equal clocks preserve the legitimate zero gap. Backwards and
     * suspend-sized gaps only reset the baseline. */
    add_sample(&shadow, 29000, 4000, 1000);
    value = snapshot_at(&shadow, 29000, 0);
    assert(value.commit_gap_ms == 3000u);
    assert(value.commit_gap_deviation_ms == 1000u);
    add_sample(&shadow, 10, 4000, 1000);
    assert(snapshot_at(&shadow, 10, 0).commit_gap_ms
           == value.commit_gap_ms);
    add_sample(&shadow, 400000, 4000, 1000);
    assert(snapshot_at(&shadow, 400000, 0).commit_gap_ms
           == value.commit_gap_ms);
}

static void test_refill_and_recent_window(void) {
    MiniIptvBufferShadow shadow;
    MiniIptvBufferShadowSnapshot value;
    miniiptv_buffer_shadow_reset(&shadow, 4000);
    add_sample(&shadow, 1000, 4000, 1000);
    add_sample(&shadow, 5000, 4000, 1000);
    add_sample(&shadow, 9000, 4000, 1000);
    miniiptv_buffer_shadow_begin_refill(&shadow, 10000);
    miniiptv_buffer_shadow_begin_refill(&shadow, 10001);
    value = snapshot_at(&shadow, 11000, 1);
    assert(value.state == MINIIPTV_BUFFER_SHADOW_REFILL);
    assert(value.total_underruns == 1u);
    assert(value.recent_underruns == 1u);
    /* Publication can wake the reader before the producer records the
     * segment's timing metrics. Count it at publication so that race cannot
     * turn a one-segment refill into zero. */
    miniiptv_buffer_shadow_note_segment_published(&shadow);
    miniiptv_buffer_shadow_end_refill(&shadow, 14000);
    miniiptv_buffer_shadow_note_segment(&shadow, KIB(256), 4000, 1000,
                                        14001);
    miniiptv_buffer_shadow_end_refill(&shadow, 15000);
    value = snapshot_at(&shadow, 14001, 0);
    assert(value.last_refill_ms == 4000u);
    assert(value.total_refill_ms == 4000u);
    assert(value.last_refill_commits == 1u);
    assert(value.state == MINIIPTV_BUFFER_SHADOW_MARGINAL);

    miniiptv_buffer_shadow_begin_refill(&shadow, 20000);
    miniiptv_buffer_shadow_end_refill(&shadow, 21000);
    value = snapshot_at(&shadow, 21000, 0);
    assert(value.recent_underruns == 2u);
    assert(value.recommended_lag_segments == 3);
    value = snapshot_at(&shadow, 120001, 0);
    assert(value.recent_underruns == 0u);
}

static void test_bounds_invalid_samples_and_polls(void) {
    MiniIptvBufferShadow shadow;
    MiniIptvBufferShadow before;
    MiniIptvBufferShadowSnapshot value;
    miniiptv_buffer_shadow_reset(&shadow, 0);
    miniiptv_buffer_shadow_note_segment(&shadow, UINT32_MAX, 0, 0,
                                        UINT64_MAX);
    value = snapshot_at(&shadow, UINT64_MAX, 0);
    assert(value.valid_samples == 0u);
    assert(value.desired_reserve_ms <= 12000u);
    assert(value.desired_reserve_bytes >= KIB(128));
    assert(value.desired_reserve_bytes <= KIB(1984));

    miniiptv_buffer_shadow_note_ring(&shadow, UINT32_MAX, true);
    value = snapshot_at(&shadow, UINT64_MAX, 0);
    assert(value.ring_current_bytes
           == MINIIPTV_STREAM_RING_CAPACITY_BYTES);
    assert(value.ring_max_bytes == MINIIPTV_STREAM_RING_CAPACITY_BYTES);

    miniiptv_buffer_shadow_note_playlist_poll(&shadow, false);
    miniiptv_buffer_shadow_note_playlist_poll(&shadow, false);
    value = snapshot_at(&shadow, UINT64_MAX, 0);
    assert(value.no_new_poll_total == 2u);
    assert(value.no_new_poll_streak == 2u);
    miniiptv_buffer_shadow_note_playlist_poll(&shadow, true);
    value = snapshot_at(&shadow, UINT64_MAX, 0);
    assert(value.no_new_poll_total == 2u);
    assert(value.no_new_poll_streak == 0u);
    assert(miniiptv_buffer_shadow_state_label(value.state) != NULL);

    before = shadow;
    (void)snapshot_at(&shadow, UINT64_MAX, 0);
    assert(memcmp(&before, &shadow, sizeof(shadow)) == 0);
}

static void test_zero_download_and_rate_extremes(void) {
    MiniIptvBufferShadow shadow;
    MiniIptvBufferShadowSnapshot value;
    miniiptv_buffer_shadow_reset(&shadow, 4000);
    miniiptv_buffer_shadow_note_segment(&shadow, KIB(256), 4000, 0, 1000);
    value = snapshot_at(&shadow, 1000, 0);
    assert(value.valid_samples == 0u);
    assert(value.content_bps == 524288u);
    assert(value.network_bps == 0u);

    miniiptv_buffer_shadow_reset(&shadow, 4000);
    miniiptv_buffer_shadow_note_segment(&shadow, 1u, 8000u, 8000u, 1000);
    miniiptv_buffer_shadow_note_segment(&shadow, UINT32_MAX, 1u, 1u, 2000);
    value = snapshot_at(&shadow, 2000, 0);
    assert(value.content_bps > 1000000000u);
    assert(value.network_bps > 1000000000u);
}

static MiniIptvBufferShadowSnapshot snapshot_for_headroom(
    uint32_t headroom_permille) {
    MiniIptvBufferShadow shadow;
    uint32_t duration_ms = headroom_permille;
    miniiptv_buffer_shadow_reset(&shadow, 1000);
    add_sample(&shadow, 1000, duration_ms, 1000);
    add_sample(&shadow, 2000, duration_ms, 1000);
    add_sample(&shadow, 3000, duration_ms, 1000);
    return snapshot_at(&shadow, 3000, 0);
}

static void test_exact_thresholds_and_caps(void) {
    MiniIptvBufferShadow shadow;
    MiniIptvBufferShadowSnapshot value;
    MiniIptvBufferShadowSnapshot below;
    MiniIptvBufferShadowSnapshot at;

    assert(snapshot_for_headroom(1049).state
           == MINIIPTV_BUFFER_SHADOW_UNSUSTAINABLE);
    assert(snapshot_for_headroom(1050).state
           == MINIIPTV_BUFFER_SHADOW_MARGINAL);
    assert(snapshot_for_headroom(1399).state
           == MINIIPTV_BUFFER_SHADOW_MARGINAL);
    assert(snapshot_for_headroom(1400).state
           == MINIIPTV_BUFFER_SHADOW_HEALTHY);
    below = snapshot_for_headroom(1999);
    at = snapshot_for_headroom(2000);
    assert(below.desired_reserve_ms > at.desired_reserve_ms);

    miniiptv_buffer_shadow_reset(&shadow, 4000);
    shadow.segment_samples = 3;
    shadow.valid_samples = 3;
    shadow.content_bps_ewma = 1000000;
    shadow.segment_ms_ewma = 4000;
    shadow.headroom_permille_ewma = 2000;
    shadow.commit_gap_samples = 1;
    shadow.commit_gap_ms_ewma = 4000;
    shadow.commit_gap_deviation_ms = 1000;
    miniiptv_buffer_shadow_note_ring(&shadow,
        MINIIPTV_STREAM_RING_CAPACITY_BYTES, true);
    assert(snapshot_at(&shadow, 1000, 0).state
           == MINIIPTV_BUFFER_SHADOW_HEALTHY);
    shadow.commit_gap_deviation_ms = 1001;
    assert(snapshot_at(&shadow, 1000, 0).state
           == MINIIPTV_BUFFER_SHADOW_MARGINAL);

    miniiptv_buffer_shadow_reset(&shadow, 1000);
    shadow.segment_samples = 3;
    shadow.valid_samples = 3;
    shadow.content_bps_ewma = UINT32_MAX;
    shadow.segment_ms_ewma = 1000;
    shadow.headroom_permille_ewma = 2000;
    shadow.largest_segment_bytes = KIB(1024);
    value = snapshot_at(&shadow, 1000, 0);
    assert(value.desired_reserve_ms == 2500u);
    assert(value.desired_reserve_bytes == KIB(3072));
    shadow.largest_segment_bytes = KIB(4096);
    value = snapshot_at(&shadow, 1000, 0);
    assert(value.desired_reserve_bytes == KIB(1984));
}

int main(void) {
    test_cold_and_known_sample();
    test_states_and_risk_hysteresis();
    test_gap_and_high_water_suppression();
    test_refill_and_recent_window();
    test_bounds_invalid_samples_and_polls();
    test_zero_download_and_rate_extremes();
    test_exact_thresholds_and_caps();
    puts("buffer shadow tests passed");
    return 0;
}

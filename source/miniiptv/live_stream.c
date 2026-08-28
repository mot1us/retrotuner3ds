#include "miniiptv/live_stream.h"

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniiptv/hls.h"
#include "miniiptv/network.h"
#include "miniiptv/stream_limits.h"
#include "miniiptv/ts_mux.h"
#include "system/util/thread_types.h"
#include "system/util/util.h"

#define STREAM_RING_SIZE MINIIPTV_STREAM_RING_CAPACITY_BYTES
#define MINIIPTV_SEGMENT_LIMIT MINIIPTV_STREAM_ATOMIC_SEGMENT_LIMIT_BYTES
#define STREAM_COLD_INITIAL_SEGMENTS 2u
#define STREAM_WARM_INITIAL_SEGMENTS 1u
#define STREAM_MAX_CONSECUTIVE_PRODUCER_FAILURES 3u
#define STREAM_NETWORK_RETRY_COUNT 2u
#define STREAM_NETWORK_RETRY_SHORT_PAUSE_US 250000ULL
#define STREAM_NETWORK_RETRY_LONG_PAUSE_US 1000000ULL
#define STREAM_REBUFFER_TARGET_MS 3000u
#define STREAM_REBUFFER_MIN_BYTES (128u * 1024u)
#define STREAM_REBUFFER_MAX_BYTES (768u * 1024u)
#define STREAM_HIGH_WATER_BYTES (5u * 1024u * 1024u)
#define STREAM_SLEEP_US 10000ULL
#define STREAM_INITIAL_TUNE_TIMEOUT_MS 30000ULL
#define STREAM_SHADOW_SAMPLE_MAX_DOWNLOAD_MS 300000ULL
#define STREAM_RENDITION_CACHE_ENTRIES 4u
#define STREAM_RENDITION_CACHE_TTL_MS 60000ULL
#define STREAM_ADAPTIVE_PROFILE_ENTRIES 32u
#define STREAM_AUDIO_SEGMENT_LIMIT (1u * 1024u * 1024u)
#define TS_PACKET_SIZE 188u

#if MINIIPTV_SEGMENT_LIMIT >= STREAM_RING_SIZE
#error "The atomic HLS segment limit must be smaller than the live ring"
#endif

#if STREAM_HIGH_WATER_BYTES >= STREAM_RING_SIZE
#error "The live high-water mark must be smaller than the live ring"
#endif

typedef struct {
    MiniIptvChannel channel;
    char media_url[MINIIPTV_HLS_URL_MAX];
    char audio_media_url[MINIIPTV_HLS_URL_MAX];
    Thread producer;
    uint32_t generation;
    bool starting;
    bool stopping;
    bool initialized;
    bool stop_requested;
    bool producer_running;
    bool rebuffering;
    size_t rebuffer_target_bytes;
    uint64_t rebuffer_deadline_ms;
    uint32_t rebuffer_wait_limit_ms;
    bool reader_started;
    bool rendition_cache_hit;
    bool adaptive_profile_hit;
    bool initial_tune_active;
    bool initial_tune_timed_out;
    uint64_t initial_tune_deadline_ms;
    MiniIptvCancelFunction initial_tune_should_cancel;
    void *initial_tune_cancel_userdata;
    unsigned int target_duration;
    uint8_t startup_lag_segments;
    unsigned long variant_bandwidth;
    unsigned int variant_width;
    unsigned int variant_height;
    char variant_codecs[96];
    /* The initial tune has already downloaded and parsed the current media
     * playlist.  Keep one bounded, owned snapshot so the producer can stage
     * its still-new segments before issuing another manifest request. */
    HlsMediaPlaylist producer_seed;
    HlsMediaPlaylist audio_producer_seed;
    bool producer_seed_available;
    bool separate_audio;
    unsigned long last_sequence;
    unsigned long last_audio_sequence;
    size_t ring_read;
    size_t ring_write;
    size_t ring_count;
    uint64_t bytes_written;
    uint64_t bytes_read;
    uint64_t measured_segment_bytes;
    uint64_t measured_segment_milliseconds;
    size_t last_segment_bytes;
    unsigned int last_download_milliseconds;
    unsigned int last_segment_milliseconds;
    unsigned long network_bandwidth;
    unsigned long downloaded_segments;
    unsigned long sequence_resyncs;
    unsigned long oversized_segment_skips;
    unsigned long automatic_relocks;
    unsigned long underruns;
    size_t attempted_segment_bytes;
    size_t reported_segment_bytes;
    int last_network_result;
    int last_error;
    MiniIptvBoundaryReason boundary_reason;
    MiniIptvBoundaryReason relock_reason;
    MiniIptvProducerState producer_state;
    MiniIptvBufferShadow buffer_shadow;
} LiveStream;

typedef struct {
    bool valid;
    uint64_t stored_ms;
    char channel_url[MINIIPTV_URL_MAX];
    char user_agent[MINIIPTV_HEADER_MAX];
    char referrer[MINIIPTV_HEADER_MAX];
    char media_url[MINIIPTV_HLS_URL_MAX];
    char audio_media_url[MINIIPTV_HLS_URL_MAX];
    unsigned long bandwidth;
    unsigned int width;
    unsigned int height;
    char codecs[96];
} RenditionCacheEntry;

typedef struct {
    bool valid;
    uint64_t channel_key;
    uint64_t updated_ms;
    uint32_t headroom_permille;
    uint32_t desired_reserve_ms;
    uint32_t total_underruns;
    uint8_t recommended_lag_segments;
} AdaptiveProfileEntry;

typedef struct {
    unsigned char *destination;
    size_t capacity;
    size_t total_size;
    size_t progress_base_size;
    size_t progress_base_reported;
    bool report_tune_progress;
} SegmentWriter;

typedef struct {
    LightLock lock;
    bool initialized;
    bool phase_active;
    uint64_t tune_started_ms;
    uint64_t phase_started_ms;
    uint64_t tune_finished_ms;
    MiniIptvTuneTelemetry value;
} TuneTimeline;

/* Static BSS storage uses ordinary application RAM, not scarce linear RAM. */
static unsigned char stream_ring[STREAM_RING_SIZE];
/* Commit complete segments atomically. A failed/truncated HTTP transfer must
 * never leave a partial access unit in the ring where FFmpeg/MVD can see it. */
static unsigned char segment_staging[MINIIPTV_SEGMENT_LIMIT];
static unsigned char audio_segment_staging[STREAM_AUDIO_SEGMENT_LIMIT];
static unsigned char mux_segment_staging[MINIIPTV_SEGMENT_LIMIT];
static LiveStream stream;
/* This lock is deliberately outside LiveStream. Session state is reset between
 * channels, but the synchronization object must keep a stable address and
 * lifetime while draw, decoder, producer, and tune threads overlap. */
static LightLock stream_lock;
/* Starting and destroying a session are whole-lifecycle operations. They
 * cannot share stream_lock because network callbacks need that lock while a
 * tune is in progress, so a second stable lock serializes only start/stop. */
static LightLock stream_lifecycle_lock;
static int stream_lock_state;
static uint32_t stream_generation;
static bool stream_active;
static TuneTimeline tune_timeline;
/* Repeat tunes can bypass one unchanged master-manifest request. This cache is
 * deliberately tiny, short lived, and stored in ordinary BSS RAM. The media
 * manifest itself is never cached: every tune still fetches current segment
 * sequences before staging a complete transport-stream segment. Only the
 * serialized tune worker accesses this table. */
static RenditionCacheEntry
    rendition_cache[STREAM_RENDITION_CACHE_ENTRIES];
/* Session-only measurements survive LiveStream teardown without retaining
 * playlist URLs. A 64-bit hash identifies the channel; a collision can only
 * choose a conservative 1--3 segment lag and cannot cross memory bounds. */
static AdaptiveProfileEntry
    adaptive_profiles[STREAM_ADAPTIVE_PROFILE_ENTRIES];
static uint8_t adaptive_profile_next;
static unsigned long automatic_relock_count;

enum {
    STREAM_LOCK_UNINITIALIZED = 0,
    STREAM_LOCK_INITIALIZING,
    STREAM_LOCK_READY
};

static void ensure_stream_lock(void) {
    int expected = STREAM_LOCK_UNINITIALIZED;
    if (__atomic_load_n(&stream_lock_state, __ATOMIC_ACQUIRE) ==
        STREAM_LOCK_READY)
        return;
    if (__atomic_compare_exchange_n(&stream_lock_state, &expected,
                                    STREAM_LOCK_INITIALIZING, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        LightLock_Init(&stream_lock);
        LightLock_Init(&stream_lifecycle_lock);
        __atomic_store_n(&stream_lock_state, STREAM_LOCK_READY,
                         __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&stream_lock_state, __ATOMIC_ACQUIRE) !=
           STREAM_LOCK_READY)
        Util_sleep(1000);
}

static bool stream_session_exists_locked(void) {
    return stream.starting || stream.stopping || stream.initialized;
}

static void live_stream_stop_serialized(void);

static uint64_t adaptive_hash_bytes(uint64_t hash, const char *text) {
    const unsigned char *bytes = (const unsigned char *)(text ? text : "");
    while (*bytes) {
        hash ^= *bytes++;
        hash *= UINT64_C(1099511628211);
    }
    /* Separate fields so concatenated header values cannot alias trivially. */
    hash ^= 0xffu;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static uint64_t adaptive_channel_key(const MiniIptvChannel *channel) {
    uint64_t hash = UINT64_C(14695981039346656037);
    if (!channel) return 0;
    hash = adaptive_hash_bytes(hash, channel->url);
    hash = adaptive_hash_bytes(hash, channel->user_agent);
    return adaptive_hash_bytes(hash, channel->referrer);
}

static const AdaptiveProfileEntry *adaptive_profile_lookup(
    const MiniIptvChannel *channel) {
    uint64_t key = adaptive_channel_key(channel);
    if (key == 0) return NULL;
    for (size_t i = 0; i < STREAM_ADAPTIVE_PROFILE_ENTRIES; i++) {
        if (adaptive_profiles[i].valid &&
            adaptive_profiles[i].channel_key == key)
            return &adaptive_profiles[i];
    }
    return NULL;
}

static void adaptive_profile_store(
    const MiniIptvChannel *channel,
    const MiniIptvBufferShadowSnapshot *snapshot,
    uint64_t now_ms) {
    size_t slot = STREAM_ADAPTIVE_PROFILE_ENTRIES;
    uint64_t key;
    if (!channel || !snapshot || snapshot->valid_samples < 3u) return;
    key = adaptive_channel_key(channel);
    if (key == 0) return;
    for (size_t i = 0; i < STREAM_ADAPTIVE_PROFILE_ENTRIES; i++) {
        if (adaptive_profiles[i].valid &&
            adaptive_profiles[i].channel_key == key) {
            slot = i;
            break;
        }
        if (!adaptive_profiles[i].valid &&
            slot == STREAM_ADAPTIVE_PROFILE_ENTRIES)
            slot = i;
    }
    if (slot == STREAM_ADAPTIVE_PROFILE_ENTRIES) {
        slot = adaptive_profile_next;
        adaptive_profile_next = (uint8_t)((adaptive_profile_next + 1u) %
            STREAM_ADAPTIVE_PROFILE_ENTRIES);
    }
    adaptive_profiles[slot].valid = true;
    adaptive_profiles[slot].channel_key = key;
    adaptive_profiles[slot].updated_ms = now_ms;
    adaptive_profiles[slot].headroom_permille = snapshot->headroom_permille;
    adaptive_profiles[slot].desired_reserve_ms =
        snapshot->desired_reserve_ms;
    adaptive_profiles[slot].total_underruns = snapshot->total_underruns;
    adaptive_profiles[slot].recommended_lag_segments =
        snapshot->recommended_lag_segments < 1u ? 1u :
        (snapshot->recommended_lag_segments > 3u ? 3u :
         snapshot->recommended_lag_segments);
}

static unsigned int tune_elapsed_milliseconds(uint64_t end, uint64_t start) {
    uint64_t elapsed = end >= start ? end - start : 0;
    return elapsed > 0xffffffffu ? 0xffffffffu : (unsigned int)elapsed;
}

const char *miniiptv_live_tune_phase_label(MiniIptvTunePhase phase) {
    switch (phase) {
        case MINIIPTV_TUNE_PHASE_OLD_STREAM_CLEANUP: return "CLEANING OLD SIGNAL";
        case MINIIPTV_TUNE_PHASE_ROOT_MANIFEST: return "ROOT MANIFEST";
        case MINIIPTV_TUNE_PHASE_MEDIA_MANIFEST: return "MEDIA MANIFEST";
        case MINIIPTV_TUNE_PHASE_INITIAL_SEGMENT: return "INITIAL SEGMENT";
        case MINIIPTV_TUNE_PHASE_PLAYER_OPEN: return "PLAYER / FFMPEG";
        case MINIIPTV_TUNE_PHASE_MVD_INIT: return "MVD INIT";
        case MINIIPTV_TUNE_PHASE_FIRST_FRAME: return "FIRST FRAME";
        case MINIIPTV_TUNE_PHASE_READY: return "ON AIR";
        case MINIIPTV_TUNE_PHASE_FAILED: return "TUNE FAILED";
        case MINIIPTV_TUNE_PHASE_IDLE:
        default: return "PREPARING";
    }
}

const char *miniiptv_live_producer_state_label(MiniIptvProducerState state) {
    switch (state) {
        case MINIIPTV_PRODUCER_STARTING: return "START";
        case MINIIPTV_PRODUCER_PLAYLIST: return "LIST";
        case MINIIPTV_PRODUCER_SEGMENT: return "FETCH";
        case MINIIPTV_PRODUCER_LIVE_EDGE: return "EDGE";
        case MINIIPTV_PRODUCER_RING_HIGH: return "FULL";
        case MINIIPTV_PRODUCER_ERROR: return "ERROR";
        case MINIIPTV_PRODUCER_STOPPED:
        default: return "STOP";
    }
}

void miniiptv_live_tune_telemetry_init(void) {
    if (!tune_timeline.initialized) {
        LightLock_Init(&tune_timeline.lock);
        tune_timeline.initialized = true;
    }
    miniiptv_live_tune_reset();
}

void miniiptv_live_tune_reset(void) {
    uint64_t now;
    if (!tune_timeline.initialized) return;
    now = osGetTime();
    LightLock_Lock(&tune_timeline.lock);
    memset(&tune_timeline.value, 0, sizeof(tune_timeline.value));
    tune_timeline.value.phase = MINIIPTV_TUNE_PHASE_IDLE;
    tune_timeline.value.failure_phase = MINIIPTV_TUNE_PHASE_IDLE;
    tune_timeline.phase_active = false;
    tune_timeline.tune_started_ms = now;
    tune_timeline.phase_started_ms = now;
    tune_timeline.tune_finished_ms = 0;
    LightLock_Unlock(&tune_timeline.lock);
}

void miniiptv_live_tune_phase_begin(MiniIptvTunePhase phase) {
    uint64_t now;
    if (!tune_timeline.initialized || phase <= MINIIPTV_TUNE_PHASE_IDLE ||
        phase >= MINIIPTV_TUNE_PHASE_COUNT)
        return;
    now = osGetTime();
    LightLock_Lock(&tune_timeline.lock);
    /* Failure is terminal for this tune. Decoder/draw phase notifications can
     * race an error callback; only the next explicit reset may clear it. */
    if (tune_timeline.value.phase == MINIIPTV_TUNE_PHASE_FAILED) {
        LightLock_Unlock(&tune_timeline.lock);
        return;
    }
    if (tune_timeline.phase_active &&
        tune_timeline.value.phase > MINIIPTV_TUNE_PHASE_IDLE &&
        tune_timeline.value.phase < MINIIPTV_TUNE_PHASE_COUNT) {
        tune_timeline.value.phase_milliseconds[tune_timeline.value.phase] =
            tune_elapsed_milliseconds(now, tune_timeline.phase_started_ms);
    }
    tune_timeline.value.phase = phase;
    tune_timeline.value.result = 0;
    tune_timeline.value.phase_elapsed_milliseconds = 0;
    tune_timeline.phase_started_ms = now;
    tune_timeline.phase_active = phase != MINIIPTV_TUNE_PHASE_READY &&
                                 phase != MINIIPTV_TUNE_PHASE_FAILED;
    if (phase == MINIIPTV_TUNE_PHASE_READY)
        tune_timeline.tune_finished_ms = now;
    LightLock_Unlock(&tune_timeline.lock);
}

void miniiptv_live_tune_phase_complete(MiniIptvTunePhase phase) {
    uint64_t now;
    if (!tune_timeline.initialized) return;
    now = osGetTime();
    LightLock_Lock(&tune_timeline.lock);
    if (tune_timeline.phase_active && tune_timeline.value.phase == phase) {
        tune_timeline.value.phase_milliseconds[phase] =
            tune_elapsed_milliseconds(now, tune_timeline.phase_started_ms);
        tune_timeline.value.phase_elapsed_milliseconds =
            tune_timeline.value.phase_milliseconds[phase];
        tune_timeline.phase_active = false;
    }
    LightLock_Unlock(&tune_timeline.lock);
}

void miniiptv_live_tune_fail(int result) {
    uint64_t now;
    if (!tune_timeline.initialized) return;
    now = osGetTime();
    LightLock_Lock(&tune_timeline.lock);
    if (tune_timeline.value.phase == MINIIPTV_TUNE_PHASE_FAILED) {
        tune_timeline.value.result = result;
        LightLock_Unlock(&tune_timeline.lock);
        return;
    }
    tune_timeline.value.failure_phase = tune_timeline.value.phase;
    if (tune_timeline.phase_active &&
        tune_timeline.value.phase > MINIIPTV_TUNE_PHASE_IDLE &&
        tune_timeline.value.phase < MINIIPTV_TUNE_PHASE_COUNT) {
        tune_timeline.value.phase_milliseconds[tune_timeline.value.phase] =
            tune_elapsed_milliseconds(now, tune_timeline.phase_started_ms);
        tune_timeline.value.phase_elapsed_milliseconds =
            tune_timeline.value.phase_milliseconds[tune_timeline.value.phase];
    }
    tune_timeline.value.phase = MINIIPTV_TUNE_PHASE_FAILED;
    tune_timeline.value.result = result;
    tune_timeline.phase_active = false;
    if (tune_timeline.tune_finished_ms == 0)
        tune_timeline.tune_finished_ms = now;
    LightLock_Unlock(&tune_timeline.lock);
}

void miniiptv_live_tune_segment_progress(size_t received_bytes,
                                         size_t reported_bytes) {
    if (!tune_timeline.initialized) return;
    LightLock_Lock(&tune_timeline.lock);
    if (tune_timeline.value.phase == MINIIPTV_TUNE_PHASE_INITIAL_SEGMENT) {
        tune_timeline.value.initial_segment_received_bytes = received_bytes;
        if (reported_bytes > 0)
            tune_timeline.value.initial_segment_reported_bytes = reported_bytes;
    }
    LightLock_Unlock(&tune_timeline.lock);
}

static void tune_segment_plan(unsigned int completed, unsigned int target) {
    if (!tune_timeline.initialized) return;
    LightLock_Lock(&tune_timeline.lock);
    if (tune_timeline.value.phase == MINIIPTV_TUNE_PHASE_INITIAL_SEGMENT) {
        tune_timeline.value.initial_segments_completed = completed;
        tune_timeline.value.initial_segments_target = target;
        tune_timeline.value.initial_segment_received_bytes = 0;
        tune_timeline.value.initial_segment_reported_bytes = 0;
    }
    LightLock_Unlock(&tune_timeline.lock);
}

void miniiptv_live_tune_get_telemetry(MiniIptvTuneTelemetry *telemetry) {
    uint64_t now;
    uint64_t end;
    if (!telemetry) return;
    memset(telemetry, 0, sizeof(*telemetry));
    if (!tune_timeline.initialized) return;
    now = osGetTime();
    LightLock_Lock(&tune_timeline.lock);
    *telemetry = tune_timeline.value;
    telemetry->phase_active = tune_timeline.phase_active;
    if (tune_timeline.phase_active) {
        telemetry->phase_elapsed_milliseconds =
            tune_elapsed_milliseconds(now, tune_timeline.phase_started_ms);
        telemetry->phase_milliseconds[telemetry->phase] =
            telemetry->phase_elapsed_milliseconds;
    }
    end = tune_timeline.tune_finished_ms ? tune_timeline.tune_finished_ms : now;
    telemetry->total_elapsed_milliseconds =
        tune_elapsed_milliseconds(end, tune_timeline.tune_started_ms);
    LightLock_Unlock(&tune_timeline.lock);
}

unsigned int miniiptv_live_tune_progress_permille(
    const MiniIptvTuneTelemetry *telemetry) {
    MiniIptvTunePhase phase;
    uint64_t segment_permille = 0;
    uint64_t staged_permille;
    unsigned int target;

    if (!telemetry) return 0;
    phase = telemetry->phase == MINIIPTV_TUNE_PHASE_FAILED
        ? telemetry->failure_phase : telemetry->phase;
    switch (phase) {
        case MINIIPTV_TUNE_PHASE_OLD_STREAM_CLEANUP: return 30;
        case MINIIPTV_TUNE_PHASE_ROOT_MANIFEST: return 70;
        case MINIIPTV_TUNE_PHASE_MEDIA_MANIFEST: return 120;
        case MINIIPTV_TUNE_PHASE_INITIAL_SEGMENT:
            target = telemetry->initial_segments_target
                ? telemetry->initial_segments_target : 1u;
            if (telemetry->initial_segment_reported_bytes > 0) {
                segment_permille =
                    (uint64_t)telemetry->initial_segment_received_bytes * 1000u /
                    telemetry->initial_segment_reported_bytes;
                if (segment_permille > 1000u) segment_permille = 1000u;
            }
            staged_permille =
                ((uint64_t)telemetry->initial_segments_completed * 1000u +
                 segment_permille) / target;
            if (staged_permille > 1000u) staged_permille = 1000u;
            return 120u + (unsigned int)(staged_permille * 730u / 1000u);
        case MINIIPTV_TUNE_PHASE_PLAYER_OPEN: return 890;
        case MINIIPTV_TUNE_PHASE_MVD_INIT: return 940;
        case MINIIPTV_TUNE_PHASE_FIRST_FRAME: return 980;
        case MINIIPTV_TUNE_PHASE_READY: return 1000;
        case MINIIPTV_TUNE_PHASE_FAILED:
        case MINIIPTV_TUNE_PHASE_IDLE:
        case MINIIPTV_TUNE_PHASE_COUNT:
        default: return 0;
    }
}

static unsigned long measured_bandwidth_locked(void) {
    uint64_t bits_per_second;
    if (stream.measured_segment_milliseconds == 0) return 0;
    bits_per_second = (stream.measured_segment_bytes * 8000u) /
                      stream.measured_segment_milliseconds;
    return bits_per_second > 0xffffffffu ? 0xffffffffu
                                         : (unsigned long)bits_per_second;
}

static unsigned long effective_bandwidth_locked(void) {
    unsigned long measured = measured_bandwidth_locked();
    return measured ? measured : stream.variant_bandwidth;
}

static size_t rebuffer_target_bytes_locked(void) {
    unsigned long bandwidth = effective_bandwidth_locked();
    uint64_t target;
    if (stream.rebuffering && stream.rebuffer_target_bytes > 0)
        return stream.rebuffer_target_bytes;
    if (bandwidth == 0) return STREAM_REBUFFER_MAX_BYTES;
    target = ((uint64_t)bandwidth * STREAM_REBUFFER_TARGET_MS) / 8000u;
    if (target < STREAM_REBUFFER_MIN_BYTES) target = STREAM_REBUFFER_MIN_BYTES;
    if (target > STREAM_REBUFFER_MAX_BYTES) target = STREAM_REBUFFER_MAX_BYTES;
    return (size_t)target;
}

static unsigned int buffered_milliseconds_locked(void) {
    unsigned long bandwidth = effective_bandwidth_locked();
    uint64_t milliseconds;
    if (bandwidth == 0) return 0;
    milliseconds = ((uint64_t)stream.ring_count * 8000u) / bandwidth;
    return milliseconds > 0xffffffffu ? 0xffffffffu
                                      : (unsigned int)milliseconds;
}

static bool stop_was_requested(void) {
    bool stopped;
    LightLock_Lock(&stream_lock);
    stopped = !stream_session_exists_locked() || stream.stop_requested;
    LightLock_Unlock(&stream_lock);
    return stopped;
}

static int curl_should_cancel(void *unused) {
    bool cancel;
    bool initial_tune_active;
    MiniIptvCancelFunction should_cancel;
    void *cancel_userdata;
    (void)unused;
    LightLock_Lock(&stream_lock);
    cancel = !stream_session_exists_locked() || stream.stop_requested;
    initial_tune_active = stream.initial_tune_active;
    should_cancel = stream.initial_tune_should_cancel;
    cancel_userdata = stream.initial_tune_cancel_userdata;
    if (!cancel && initial_tune_active &&
        osGetTime() >= stream.initial_tune_deadline_ms) {
        stream.initial_tune_timed_out = true;
        cancel = true;
    }
    LightLock_Unlock(&stream_lock);
    if (!cancel && initial_tune_active && should_cancel)
        cancel = should_cancel(cancel_userdata) != 0;
    return cancel;
}

static int cancellation_result(void) {
    int result = MINIIPTV_STAGE_OK;
    bool initial_tune_active;
    bool initial_tune_timed_out;
    MiniIptvCancelFunction should_cancel;
    void *cancel_userdata;
    LightLock_Lock(&stream_lock);
    if (!stream_session_exists_locked()) {
        LightLock_Unlock(&stream_lock);
        return MINIIPTV_STAGE_CANCELLED;
    }
    initial_tune_active = stream.initial_tune_active;
    initial_tune_timed_out = stream.initial_tune_timed_out;
    should_cancel = stream.initial_tune_should_cancel;
    cancel_userdata = stream.initial_tune_cancel_userdata;
    if (stream.stop_requested) {
        result = MINIIPTV_STAGE_CANCELLED;
    } else if (initial_tune_timed_out ||
               (initial_tune_active &&
                osGetTime() >= stream.initial_tune_deadline_ms)) {
        stream.initial_tune_timed_out = true;
        result = MINIIPTV_STAGE_TUNE_TIMEOUT;
    }
    LightLock_Unlock(&stream_lock);
    if (result == MINIIPTV_STAGE_OK && initial_tune_active && should_cancel &&
        should_cancel(cancel_userdata))
        result = MINIIPTV_STAGE_CANCELLED;
    return result;
}

static bool retryable_network_stage_error(int result) {
    return result == MINIIPTV_STAGE_MANIFEST_FETCH_FAILED ||
           result == MINIIPTV_STAGE_MEDIA_FETCH_FAILED ||
           result == MINIIPTV_STAGE_SEGMENT_FETCH_FAILED;
}

static int wait_before_network_retry(unsigned int retry) {
    const unsigned int pause_us = retry == 0u
        ? STREAM_NETWORK_RETRY_SHORT_PAUSE_US
        : STREAM_NETWORK_RETRY_LONG_PAUSE_US;
    unsigned int elapsed_us = 0;
    while (elapsed_us < pause_us) {
        int result = cancellation_result();
        if (result != MINIIPTV_STAGE_OK) return result;
        Util_sleep(50000);
        elapsed_us += 50000u;
    }
    return cancellation_result();
}

static bool is_complete_mpeg_ts(const unsigned char *data, size_t size) {
    if (!data || size < TS_PACKET_SIZE || size % TS_PACKET_SIZE != 0)
        return false;
    for (size_t offset = 0; offset < size; offset += TS_PACKET_SIZE) {
        if (data[offset] != 0x47) return false;
    }
    return true;
}

static bool ring_commit_segment(const unsigned char *data, size_t size) {
    if (!data || size == 0 || size > MINIIPTV_SEGMENT_LIMIT ||
        size > STREAM_RING_SIZE)
        return false;
    while (true) {
        size_t free_space;
        size_t contiguous;
        size_t first;
        bool can_wait;
        LightLock_Lock(&stream_lock);
        if (stream.stop_requested) {
            LightLock_Unlock(&stream_lock);
            return false;
        }
        free_space = STREAM_RING_SIZE - stream.ring_count;
        if (free_space >= size) {
            contiguous = STREAM_RING_SIZE - stream.ring_write;
            first = size < contiguous ? size : contiguous;
            memcpy(stream_ring + stream.ring_write, data, first);
            if (first < size)
                memcpy(stream_ring, data + first, size - first);
            /* Publish only after both halves have been copied. The reader
             * cannot observe a segment prefix because it uses this lock. */
            stream.ring_write = (stream.ring_write + size) % STREAM_RING_SIZE;
            stream.ring_count += size;
            stream.bytes_written += size;
            miniiptv_buffer_shadow_note_segment_published(
                &stream.buffer_shadow);
            miniiptv_buffer_shadow_note_ring(&stream.buffer_shadow,
                (uint32_t)stream.ring_count, stream.reader_started);
            LightLock_Unlock(&stream_lock);
            return true;
        }
        can_wait = stream.reader_started;
        /* A segment can require more free space before the nominal 5 MiB
         * high-water threshold. Exclude that deliberate producer wait from
         * the next delivery-gap sample too. */
        miniiptv_buffer_shadow_note_high_water(&stream.buffer_shadow);
        LightLock_Unlock(&stream_lock);
        /* Before playback starts there is no consumer that can free space.
         * Fail cleanly instead of deadlocking during initial buffering. */
        if (!can_wait) return false;
        Util_sleep(STREAM_SLEEP_US);
    }
}

static size_t segment_write_callback(const unsigned char *data, size_t size,
                                     void *userdata) {
    SegmentWriter *writer = userdata;
    if (!writer || !writer->destination || (!data && size)) return 0;
    if (writer->total_size > writer->capacity) return 0;
    if (size > writer->capacity - writer->total_size) return 0;
    memcpy(writer->destination + writer->total_size, data, size);
    writer->total_size += size;
    return size;
}

static void segment_progress_callback(size_t received_size,
                                      size_t reported_size,
                                      void *userdata) {
    SegmentWriter *writer = userdata;
    if (!writer || !writer->report_tune_progress) return;
    miniiptv_live_tune_segment_progress(
        writer->progress_base_size + received_size,
        reported_size > 0
            ? writer->progress_base_reported + reported_size : 0);
}

static int stream_segment(const HlsSegment *segment,
                          const HlsSegment *audio_segment,
                          size_t *downloaded_size) {
    SegmentWriter writer = {0};
    SegmentWriter audio_writer = {0};
    NetworkStreamMetrics metrics = {0};
    NetworkStreamMetrics audio_metrics = {0};
    uint64_t download_started;
    uint64_t download_finished;
    uint64_t download_elapsed;
    uint64_t commit_ms = 0;
    uint32_t duration_ms = 0;
    uint32_t download_ms = 0;
    uint32_t shadow_download_ms = 0;
    bool download_clock_valid;
    bool too_large = false;
    bool valid_ts = false;
    bool committed = false;
    bool mux_failed = false;
    size_t committed_size = 0;
    int result;
    if (!segment) return MINIIPTV_STAGE_INVALID_ARGUMENT;
    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) return result;
    writer.destination = segment_staging;
    writer.capacity = sizeof(segment_staging);
    LightLock_Lock(&stream_lock);
    writer.report_tune_progress = stream.initial_tune_active;
    LightLock_Unlock(&stream_lock);
    download_started = osGetTime();
    result = network_stream_data(segment->url, stream.channel.user_agent,
                                 stream.channel.referrer,
                                 MINIIPTV_SEGMENT_LIMIT,
                                 segment_write_callback, &writer,
                                 segment_progress_callback, &writer,
                                 curl_should_cancel, NULL, &metrics);
    download_finished = osGetTime();
    download_clock_valid = download_finished >= download_started;
    download_elapsed = download_clock_valid
        ? download_finished - download_started : 0;
    if (download_clock_valid && download_elapsed == 0) download_elapsed = 1;
    if (result == -5) {
        int cancelled = cancellation_result();
        if (cancelled != MINIIPTV_STAGE_OK) result = cancelled;
    }
    too_large = result == MINIIPTV_NETWORK_TOO_LARGE;
    if (result == 0 && metrics.received_size == writer.total_size)
        valid_ts = is_complete_mpeg_ts(segment_staging, writer.total_size);
    if (result == 0 && valid_ts && stream.separate_audio) {
        size_t mux_size = 0;
        if (!audio_segment) {
            result = MINIIPTV_STAGE_MEDIA_INVALID;
        } else {
            audio_writer.destination = audio_segment_staging;
            audio_writer.capacity = sizeof(audio_segment_staging);
            audio_writer.report_tune_progress = writer.report_tune_progress;
            audio_writer.progress_base_size = writer.total_size;
            audio_writer.progress_base_reported = metrics.reported_size;
            result = network_stream_data(
                audio_segment->url, stream.channel.user_agent,
                stream.channel.referrer, STREAM_AUDIO_SEGMENT_LIMIT,
                segment_write_callback, &audio_writer,
                segment_progress_callback, &audio_writer,
                curl_should_cancel, NULL, &audio_metrics);
            if (result == -5) {
                int cancelled = cancellation_result();
                if (cancelled != MINIIPTV_STAGE_OK) result = cancelled;
            }
            if (result == MINIIPTV_NETWORK_TOO_LARGE)
                too_large = true;
            if (result == 0 &&
                audio_metrics.received_size == audio_writer.total_size &&
                is_complete_mpeg_ts(audio_segment_staging,
                                    audio_writer.total_size)) {
                if (miniiptv_ts_mux_av(
                        segment_staging, writer.total_size,
                        audio_segment_staging, audio_writer.total_size,
                        mux_segment_staging, sizeof(mux_segment_staging),
                        &mux_size) == 0) {
                    committed = ring_commit_segment(mux_segment_staging,
                                                    mux_size);
                    committed_size = committed ? mux_size : 0u;
                } else {
                    mux_failed = true;
                }
            } else if (result == 0) {
                result = MINIIPTV_STAGE_NOT_MPEG_TS;
            }
        }
    } else if (result == 0 && valid_ts) {
        committed = ring_commit_segment(segment_staging, writer.total_size);
        committed_size = committed ? writer.total_size : 0u;
    }
    if (stream.separate_audio) {
        download_finished = osGetTime();
        download_clock_valid = download_finished >= download_started;
        download_elapsed = download_clock_valid
            ? download_finished - download_started : 0;
        if (download_clock_valid && download_elapsed == 0)
            download_elapsed = 1;
    }
    if (committed) commit_ms = osGetTime();
    if (downloaded_size) *downloaded_size = committed_size;

    LightLock_Lock(&stream_lock);
    stream.attempted_segment_bytes = metrics.received_size +
        audio_metrics.received_size;
    stream.reported_segment_bytes = metrics.reported_size +
        audio_metrics.reported_size;
    stream.last_network_result = result;
    if (result == 0 && valid_ts && committed) {
        if (segment->duration > 0.0) {
            double scaled_duration = segment->duration * 1000.0;
            if (scaled_duration <= (double)UINT32_MAX - 0.5)
                duration_ms = (uint32_t)(scaled_duration + 0.5);
        }
        if (download_clock_valid)
            download_ms = download_elapsed > UINT32_MAX
                ? UINT32_MAX : (uint32_t)download_elapsed;
        if (download_clock_valid
        && download_elapsed <= STREAM_SHADOW_SAMPLE_MAX_DOWNLOAD_MS)
            shadow_download_ms = download_ms;
        stream.downloaded_segments++;
        stream.last_segment_bytes = committed_size;
        stream.last_download_milliseconds = download_ms;
        stream.last_segment_milliseconds = duration_ms;
        if (download_ms > 0) {
            uint64_t network_bps =
                ((uint64_t)committed_size * 8000u) / download_ms;
            stream.network_bandwidth = network_bps > 0xffffffffu
                ? 0xffffffffu : (unsigned long)network_bps;
        }
        else {
            stream.network_bandwidth = 0;
        }
        if (duration_ms > 0) {
            stream.measured_segment_bytes += committed_size;
            stream.measured_segment_milliseconds += duration_ms;
        }
        miniiptv_buffer_shadow_note_segment(&stream.buffer_shadow,
            (uint32_t)committed_size, duration_ms, shadow_download_ms,
            commit_ms);
        stream.last_sequence = segment->sequence;
        if (audio_segment)
            stream.last_audio_sequence = audio_segment->sequence;
        stream.last_error = 0;
    } else {
        if (too_large) {
            stream.last_error = MINIIPTV_STAGE_TOO_LARGE;
            /* The producer terminates this decoder generation. Skipping ahead
             * inside one MVD session could cross a non-independent H.264
             * boundary. */
        } else if (mux_failed) {
            stream.last_error = MINIIPTV_STAGE_TS_MUX_FAILED;
            stream.stop_requested = true;
        } else if (result == 0 && !valid_ts) {
            stream.last_error = MINIIPTV_STAGE_NOT_MPEG_TS;
            /* A successful HTTP body that is not a complete TS segment is a
             * permanent compatibility/safety failure, not a network retry. */
            stream.stop_requested = true;
        } else if (result == MINIIPTV_STAGE_CANCELLED ||
                   result == MINIIPTV_STAGE_TUNE_TIMEOUT) {
            stream.last_error = result;
        } else if (result == MINIIPTV_STAGE_NOT_MPEG_TS) {
            stream.last_error = MINIIPTV_STAGE_NOT_MPEG_TS;
            stream.stop_requested = true;
        } else if (result != 0 || !committed) {
            stream.last_error = MINIIPTV_STAGE_SEGMENT_FETCH_FAILED;
        } else {
            stream.last_error = MINIIPTV_STAGE_NOT_MPEG_TS;
        }
    }
    result = (result == 0 && valid_ts && committed) ? MINIIPTV_STAGE_OK
                                                    : stream.last_error;
    LightLock_Unlock(&stream_lock);
    if (writer.report_tune_progress)
        miniiptv_live_tune_segment_progress(
            metrics.received_size + audio_metrics.received_size,
            metrics.reported_size + audio_metrics.reported_size);
    return result;
}

static bool rendition_cache_lookup(const MiniIptvChannel *channel,
                                    RenditionCacheEntry *entry) {
    uint64_t now;
    if (!channel || !channel->url[0] || !entry) return false;
    now = osGetTime();
    for (size_t i = 0; i < STREAM_RENDITION_CACHE_ENTRIES; i++) {
        RenditionCacheEntry *candidate = &rendition_cache[i];
        if (!candidate->valid) continue;
        if (now < candidate->stored_ms ||
            now - candidate->stored_ms > STREAM_RENDITION_CACHE_TTL_MS) {
            memset(candidate, 0, sizeof(*candidate));
            continue;
        }
        if (strcmp(candidate->channel_url, channel->url) == 0 &&
            strcmp(candidate->user_agent, channel->user_agent) == 0 &&
            strcmp(candidate->referrer, channel->referrer) == 0) {
            *entry = *candidate;
            return true;
        }
    }
    return false;
}

static void rendition_cache_invalidate(const MiniIptvChannel *channel) {
    if (!channel || !channel->url[0]) return;
    for (size_t i = 0; i < STREAM_RENDITION_CACHE_ENTRIES; i++) {
        if (rendition_cache[i].valid &&
            strcmp(rendition_cache[i].channel_url, channel->url) == 0 &&
            strcmp(rendition_cache[i].user_agent, channel->user_agent) == 0 &&
            strcmp(rendition_cache[i].referrer, channel->referrer) == 0) {
            memset(&rendition_cache[i], 0, sizeof(rendition_cache[i]));
            return;
        }
    }
}

static void rendition_cache_store(const MiniIptvChannel *channel,
                                  const char *media_url,
                                  const HlsSelection *selection) {
    size_t slot = STREAM_RENDITION_CACHE_ENTRIES;
    size_t oldest_slot = STREAM_RENDITION_CACHE_ENTRIES;
    uint64_t oldest = UINT64_MAX;
    uint64_t now;
    if (!channel || !channel->url[0] || !media_url || !media_url[0] ||
        !selection || selection->type != HLS_MASTER_PLAYLIST)
        return;
    now = osGetTime();
    for (size_t i = 0; i < STREAM_RENDITION_CACHE_ENTRIES; i++) {
        if (rendition_cache[i].valid &&
            strcmp(rendition_cache[i].channel_url, channel->url) == 0 &&
            strcmp(rendition_cache[i].user_agent, channel->user_agent) == 0 &&
            strcmp(rendition_cache[i].referrer, channel->referrer) == 0) {
            slot = i;
            break;
        }
        if (!rendition_cache[i].valid ||
            now < rendition_cache[i].stored_ms ||
            now - rendition_cache[i].stored_ms >
                STREAM_RENDITION_CACHE_TTL_MS) {
            if (slot == STREAM_RENDITION_CACHE_ENTRIES) slot = i;
            continue;
        }
        if (rendition_cache[i].stored_ms < oldest) {
            oldest = rendition_cache[i].stored_ms;
            oldest_slot = i;
        }
    }
    if (slot == STREAM_RENDITION_CACHE_ENTRIES)
        slot = oldest_slot < STREAM_RENDITION_CACHE_ENTRIES ? oldest_slot : 0;
    memset(&rendition_cache[slot], 0, sizeof(rendition_cache[slot]));
    rendition_cache[slot].valid = true;
    rendition_cache[slot].stored_ms = now;
    snprintf(rendition_cache[slot].channel_url,
             sizeof(rendition_cache[slot].channel_url), "%s", channel->url);
    snprintf(rendition_cache[slot].user_agent,
             sizeof(rendition_cache[slot].user_agent), "%s",
             channel->user_agent);
    snprintf(rendition_cache[slot].referrer,
             sizeof(rendition_cache[slot].referrer), "%s",
             channel->referrer);
    snprintf(rendition_cache[slot].media_url,
             sizeof(rendition_cache[slot].media_url), "%s", media_url);
    snprintf(rendition_cache[slot].audio_media_url,
             sizeof(rendition_cache[slot].audio_media_url), "%s",
             selection->audio_url);
    rendition_cache[slot].bandwidth = selection->bandwidth;
    rendition_cache[slot].width = selection->width;
    rendition_cache[slot].height = selection->height;
    snprintf(rendition_cache[slot].codecs,
             sizeof(rendition_cache[slot].codecs), "%s", selection->codecs);
}

static int fetch_playlist_url(char *playlist_url, size_t playlist_url_size,
                              HlsMediaPlaylist *media) {
    NetworkTextResponse response = {0};
    const char *effective_url;
    int result;
    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) return result;
    result = network_get_data_cancelable(
        playlist_url, stream.channel.user_agent, stream.channel.referrer,
        MINIIPTV_MANIFEST_LIMIT, curl_should_cancel, NULL, &response);
    if (result == -5) {
        result = cancellation_result();
        return result == MINIIPTV_STAGE_OK ? MINIIPTV_STAGE_MEDIA_FETCH_FAILED
                                           : result;
    }
    if (result != 0) return MINIIPTV_STAGE_MEDIA_FETCH_FAILED;
    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) {
        network_response_free(&response);
        return result;
    }
    effective_url = response.final_url[0] ? response.final_url : playlist_url;
    result = hls_parse_media_playlist(response.data, effective_url, media);
    if (result == 0 && response.final_url[0])
        snprintf(playlist_url, playlist_url_size, "%s", response.final_url);
    network_response_free(&response);
    if (result != 0) return MINIIPTV_STAGE_MEDIA_INVALID;
    if (!media->is_live || media->encrypted || media->uses_byte_ranges ||
        media->uses_init_map)
        return MINIIPTV_STAGE_UNSUPPORTED_HLS;
    return MINIIPTV_STAGE_OK;
}

static int fetch_media_playlist(HlsMediaPlaylist *media) {
    return fetch_playlist_url(stream.media_url, sizeof(stream.media_url),
                              media);
}

static int fetch_audio_playlist(HlsMediaPlaylist *media) {
    if (!stream.separate_audio || !stream.audio_media_url[0])
        return MINIIPTV_STAGE_INVALID_ARGUMENT;
    return fetch_playlist_url(stream.audio_media_url,
                              sizeof(stream.audio_media_url), media);
}

static const HlsSegment *matching_audio_segment(
    const HlsMediaPlaylist *video, size_t video_index,
    const HlsMediaPlaylist *audio) {
    const HlsSegment *video_segment;
    const HlsSegment *closest = NULL;
    uint64_t closest_delta = UINT64_MAX;
    size_t distance_from_edge;
    if (!video || !audio || video_index >= video->count || audio->count == 0u)
        return NULL;
    video_segment = &video->segments[video_index];
    if (video_segment->has_program_date_time) {
        for (size_t i = 0; i < audio->count; i++) {
            const HlsSegment *candidate = &audio->segments[i];
            uint64_t delta;
            if (!candidate->has_program_date_time) continue;
            delta = video_segment->program_date_time_ms >=
                    candidate->program_date_time_ms
                ? (uint64_t)(video_segment->program_date_time_ms -
                             candidate->program_date_time_ms)
                : (uint64_t)(candidate->program_date_time_ms -
                             video_segment->program_date_time_ms);
            if (delta < closest_delta) {
                closest = candidate;
                closest_delta = delta;
            }
        }
        /* Rendition packagers normally align these within a few milliseconds.
         * Two seconds tolerates rounding without pairing different segments. */
        if (closest && closest_delta <= 2000u) return closest;
        return NULL;
    }
    distance_from_edge = video->count - 1u - video_index;
    if (distance_from_edge >= audio->count) return NULL;
    return &audio->segments[audio->count - 1u - distance_from_edge];
}

static int resolve_initial_playlist_uncached(const MiniIptvChannel *channel,
                                             HlsMediaPlaylist *media) {
    NetworkTextResponse root = {0};
    NetworkTextResponse media_response = {0};
    HlsSelection selection;
    const char *root_url;
    const char *media_text;
    const char *media_url;
    int result = MINIIPTV_STAGE_MANIFEST_FETCH_FAILED;

    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) goto cleanup;
    miniiptv_live_tune_phase_begin(MINIIPTV_TUNE_PHASE_ROOT_MANIFEST);
    result = network_get_data_cancelable(
        channel->url, channel->user_agent, channel->referrer,
        MINIIPTV_MANIFEST_LIMIT, curl_should_cancel, NULL, &root);
    if (result == -5) {
        result = cancellation_result();
        if (result == MINIIPTV_STAGE_OK)
            result = MINIIPTV_STAGE_MANIFEST_FETCH_FAILED;
        goto cleanup;
    }
    if (result != 0) {
        result = MINIIPTV_STAGE_MANIFEST_FETCH_FAILED;
        goto cleanup;
    }
    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) goto cleanup;
    root_url = root.final_url[0] ? root.final_url : channel->url;
    if (hls_select_stream(root.data, root_url, &selection) != 0) {
        result = MINIIPTV_STAGE_MASTER_INVALID;
        goto cleanup;
    }
    stream.variant_bandwidth = selection.bandwidth;
    stream.variant_width = selection.width;
    stream.variant_height = selection.height;
    snprintf(stream.variant_codecs, sizeof(stream.variant_codecs), "%s",
             selection.codecs);
    stream.separate_audio = selection.has_separate_audio != 0;
    snprintf(stream.audio_media_url, sizeof(stream.audio_media_url), "%s",
             selection.audio_url);
    miniiptv_live_tune_phase_complete(MINIIPTV_TUNE_PHASE_ROOT_MANIFEST);
    miniiptv_live_tune_phase_begin(MINIIPTV_TUNE_PHASE_MEDIA_MANIFEST);
    if (selection.type == HLS_MASTER_PLAYLIST) {
        result = network_get_data_cancelable(
            selection.url, channel->user_agent, channel->referrer,
            MINIIPTV_MANIFEST_LIMIT, curl_should_cancel, NULL,
            &media_response);
        if (result == -5) {
            result = cancellation_result();
            if (result == MINIIPTV_STAGE_OK)
                result = MINIIPTV_STAGE_MEDIA_FETCH_FAILED;
            goto cleanup;
        }
        if (result != 0) {
            result = MINIIPTV_STAGE_MEDIA_FETCH_FAILED;
            goto cleanup;
        }
        result = cancellation_result();
        if (result != MINIIPTV_STAGE_OK) goto cleanup;
        media_text = media_response.data;
        media_url = media_response.final_url[0] ? media_response.final_url
                                                : selection.url;
    } else {
        media_text = root.data;
        media_url = root_url;
    }
    if (hls_parse_media_playlist(media_text, media_url, media) != 0) {
        result = MINIIPTV_STAGE_MEDIA_INVALID;
        goto cleanup;
    }
    if (!media->is_live || media->encrypted || media->uses_byte_ranges ||
        media->uses_init_map) {
        result = MINIIPTV_STAGE_UNSUPPORTED_HLS;
        goto cleanup;
    }
    miniiptv_live_tune_phase_complete(MINIIPTV_TUNE_PHASE_MEDIA_MANIFEST);
    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) goto cleanup;
    snprintf(stream.media_url, sizeof(stream.media_url), "%s", media_url);
    rendition_cache_store(channel, stream.media_url, &selection);
    result = MINIIPTV_STAGE_OK;

cleanup:
    network_response_free(&media_response);
    network_response_free(&root);
    return result;
}

static int resolve_initial_playlist(const MiniIptvChannel *channel,
                                    HlsMediaPlaylist *media) {
    RenditionCacheEntry cached;
    int result;
    if (rendition_cache_lookup(channel, &cached)) {
        LightLock_Lock(&stream_lock);
        stream.rendition_cache_hit = true;
        LightLock_Unlock(&stream_lock);
        stream.variant_bandwidth = cached.bandwidth;
        stream.variant_width = cached.width;
        stream.variant_height = cached.height;
        snprintf(stream.variant_codecs, sizeof(stream.variant_codecs), "%s",
                 cached.codecs);
        snprintf(stream.media_url, sizeof(stream.media_url), "%s",
                 cached.media_url);
        snprintf(stream.audio_media_url, sizeof(stream.audio_media_url), "%s",
                 cached.audio_media_url);
        stream.separate_audio = cached.audio_media_url[0] != '\0';
        miniiptv_live_tune_phase_begin(MINIIPTV_TUNE_PHASE_MEDIA_MANIFEST);
        result = fetch_media_playlist(media);
        if (result == MINIIPTV_STAGE_OK) {
            miniiptv_live_tune_phase_complete(
                MINIIPTV_TUNE_PHASE_MEDIA_MANIFEST);
            return cancellation_result();
        }
        /* A selected rendition can disappear or its signed URL can expire.
         * Discard the hint and resolve the root master in this same tune;
         * cancellation and the original 30-second deadline remain in force. */
        rendition_cache_invalidate(channel);
        LightLock_Lock(&stream_lock);
        stream.rendition_cache_hit = false;
        LightLock_Unlock(&stream_lock);
        if (result == MINIIPTV_STAGE_CANCELLED ||
            result == MINIIPTV_STAGE_TUNE_TIMEOUT)
            return result;
        result = cancellation_result();
        if (result != MINIIPTV_STAGE_OK) return result;
    }
    return resolve_initial_playlist_uncached(channel, media);
}

static bool take_producer_seed(HlsMediaPlaylist *media,
                               HlsMediaPlaylist *audio_media) {
    bool available;
    if (!media || !audio_media) return false;
    LightLock_Lock(&stream_lock);
    available = stream.producer_seed_available;
    if (available) {
        *media = stream.producer_seed;
        if (stream.separate_audio)
            *audio_media = stream.audio_producer_seed;
        else
            memset(audio_media, 0, sizeof(*audio_media));
        stream.producer_seed_available = false;
    }
    LightLock_Unlock(&stream_lock);
    return available;
}

static void producer_main(void *unused) {
    unsigned int consecutive_manifest_failures = 0;
    unsigned int consecutive_segment_failures = 0;
    (void)unused;
    while (!stop_was_requested()) {
        HlsMediaPlaylist media;
        HlsMediaPlaylist audio_media;
        size_t buffered;
        bool added = false;
        bool found_new_segment = false;
        bool used_seed;
        int result;

        LightLock_Lock(&stream_lock);
        buffered = stream.ring_count;
        if (buffered >= STREAM_HIGH_WATER_BYTES) {
            stream.producer_state = MINIIPTV_PRODUCER_RING_HIGH;
            miniiptv_buffer_shadow_note_high_water(&stream.buffer_shadow);
        }
        LightLock_Unlock(&stream_lock);
        if (buffered >= STREAM_HIGH_WATER_BYTES) {
            Util_sleep(50000);
            continue;
        }

        used_seed = take_producer_seed(&media, &audio_media);
        if (!used_seed) {
            LightLock_Lock(&stream_lock);
            stream.producer_state = MINIIPTV_PRODUCER_PLAYLIST;
            LightLock_Unlock(&stream_lock);
            result = fetch_media_playlist(&media);
            if (result != MINIIPTV_STAGE_OK) {
                bool terminal;
                LightLock_Lock(&stream_lock);
                consecutive_manifest_failures++;
                stream.last_error = result;
                stream.producer_state = MINIIPTV_PRODUCER_ERROR;
                terminal = consecutive_manifest_failures >=
                    STREAM_MAX_CONSECUTIVE_PRODUCER_FAILURES;
                if (terminal) stream.stop_requested = true;
                LightLock_Unlock(&stream_lock);
                if (terminal) break;
                for (int retry = 0;
                     retry < 10 && !stop_was_requested(); retry++)
                    Util_sleep(100000);
                continue;
            }
            if (stream.separate_audio) {
                result = fetch_audio_playlist(&audio_media);
                if (result != MINIIPTV_STAGE_OK) {
                    bool terminal;
                    LightLock_Lock(&stream_lock);
                    consecutive_manifest_failures++;
                    stream.last_error = result;
                    stream.producer_state = MINIIPTV_PRODUCER_ERROR;
                    terminal = consecutive_manifest_failures >=
                        STREAM_MAX_CONSECUTIVE_PRODUCER_FAILURES;
                    if (terminal) stream.stop_requested = true;
                    LightLock_Unlock(&stream_lock);
                    if (terminal) break;
                    for (int retry = 0;
                         retry < 10 && !stop_was_requested(); retry++)
                        Util_sleep(100000);
                    continue;
                }
            }
            consecutive_manifest_failures = 0;
        }
        if (media.target_duration) {
            LightLock_Lock(&stream_lock);
            stream.target_duration = media.target_duration;
            miniiptv_buffer_shadow_set_target(&stream.buffer_shadow,
                media.target_duration > UINT32_MAX / 1000u
                    ? UINT32_MAX : media.target_duration * 1000u,
                osGetTime());
            LightLock_Unlock(&stream_lock);
        }
        if (media.count > 0 && stream.last_sequence > 0 &&
            media.segments[media.count - 1].sequence < stream.last_sequence) {
            LightLock_Lock(&stream_lock);
            stream.last_error = MINIIPTV_STAGE_DISCONTINUITY;
            stream.boundary_reason =
                MINIIPTV_BOUNDARY_PLAYLIST_REGRESSION;
            stream.stop_requested = true;
            LightLock_Unlock(&stream_lock);
            break;
        }
        for (size_t i = 0; i < media.count && !stop_was_requested(); i++) {
            size_t ignored_size = 0;
            const HlsSegment *audio_segment = NULL;
            bool sequence_gap = false;
            if (media.segments[i].sequence <= stream.last_sequence) continue;
            found_new_segment = true;
            if (stream.separate_audio) {
                audio_segment = matching_audio_segment(&media, i,
                                                       &audio_media);
                if (!audio_segment) {
                    found_new_segment = false;
                    continue;
                }
                if (audio_segment->sequence <= stream.last_audio_sequence)
                    continue;
                if (audio_segment->discontinuity) {
                    LightLock_Lock(&stream_lock);
                    stream.last_error = MINIIPTV_STAGE_DISCONTINUITY;
                    stream.boundary_reason =
                        MINIIPTV_BOUNDARY_DISCONTINUITY;
                    stream.stop_requested = true;
                    stream.producer_state = MINIIPTV_PRODUCER_ERROR;
                    LightLock_Unlock(&stream_lock);
                    break;
                }
                sequence_gap = stream.last_audio_sequence > 0 &&
                    audio_segment->sequence >
                        stream.last_audio_sequence + 1u;
            }
            if (media.segments[i].discontinuity) {
                LightLock_Lock(&stream_lock);
                stream.last_error = MINIIPTV_STAGE_DISCONTINUITY;
                stream.boundary_reason =
                    MINIIPTV_BOUNDARY_DISCONTINUITY;
                stream.stop_requested = true;
                stream.producer_state = MINIIPTV_PRODUCER_ERROR;
                LightLock_Unlock(&stream_lock);
                break;
            }
            if (stream.last_sequence > 0 &&
                media.segments[i].sequence > stream.last_sequence + 1u)
                sequence_gap = true;
            if (sequence_gap) {
                /* Never splice non-contiguous transport segments into an
                 * initialized MVD session. A skipped sequence can cross an
                 * unadvertised encoder/timestamp boundary, and an MVD fault
                 * can halt the console rather than only this application.
                 * Stop before committing new bytes; the app performs a
                 * bounded clean relock with a fresh decoder instance. */
                LightLock_Lock(&stream_lock);
                stream.sequence_resyncs++;
                stream.last_error = MINIIPTV_STAGE_DISCONTINUITY;
                stream.boundary_reason = MINIIPTV_BOUNDARY_DISCONTINUITY;
                stream.stop_requested = true;
                stream.producer_state = MINIIPTV_PRODUCER_ERROR;
                LightLock_Unlock(&stream_lock);
                break;
            }
            LightLock_Lock(&stream_lock);
            stream.producer_state = MINIIPTV_PRODUCER_SEGMENT;
            LightLock_Unlock(&stream_lock);
            result = stream_segment(&media.segments[i], audio_segment,
                                    &ignored_size);
            if (result == MINIIPTV_STAGE_TOO_LARGE) {
                LightLock_Lock(&stream_lock);
                stream.oversized_segment_skips++;
                stream.last_error = MINIIPTV_STAGE_TOO_LARGE;
                stream.boundary_reason =
                    MINIIPTV_BOUNDARY_OVERSIZED_SEGMENT;
                stream.stop_requested = true;
                stream.producer_state = MINIIPTV_PRODUCER_ERROR;
                LightLock_Unlock(&stream_lock);
                /* Do not splice the segment after this one into the current
                 * MVD session: HLS does not guarantee it is independently
                 * decodable. The app can retry with a fresh decoder at a new
                 * live edge. */
                break;
            }
            if (result != MINIIPTV_STAGE_OK) {
                bool terminal;
                LightLock_Lock(&stream_lock);
                stream.producer_state = MINIIPTV_PRODUCER_ERROR;
                if (retryable_network_stage_error(result)) {
                    consecutive_segment_failures++;
                    if (consecutive_segment_failures >=
                        STREAM_MAX_CONSECUTIVE_PRODUCER_FAILURES)
                        stream.stop_requested = true;
                }
                terminal = stream.stop_requested;
                LightLock_Unlock(&stream_lock);
                if (terminal) break;
                for (int retry = 0; retry < 5 && !stop_was_requested(); retry++)
                    Util_sleep(100000);
                break;
            }
            consecutive_segment_failures = 0;
            added = true;
        }
        if (stop_was_requested()) break;
        if (!used_seed) {
            LightLock_Lock(&stream_lock);
            miniiptv_buffer_shadow_note_playlist_poll(&stream.buffer_shadow,
                found_new_segment);
            LightLock_Unlock(&stream_lock);
        }
        /* A consumed seed with no newer sequence is stale but not an error;
         * refresh immediately.  Preserve the normal retry delay when a
         * segment was present but failed staging. */
        if (!added && (!used_seed || found_new_segment)) {
            if (!found_new_segment) {
                LightLock_Lock(&stream_lock);
                stream.producer_state = MINIIPTV_PRODUCER_LIVE_EDGE;
                LightLock_Unlock(&stream_lock);
            }
            for (int i = 0; i < 10 && !stop_was_requested(); i++)
                Util_sleep(100000);
        }
    }
    LightLock_Lock(&stream_lock);
    stream.producer_running = false;
    stream.producer_state = stream.stop_requested && stream.last_error != 0
        ? MINIIPTV_PRODUCER_ERROR : MINIIPTV_PRODUCER_STOPPED;
    LightLock_Unlock(&stream_lock);
    threadExit(0);
}

int miniiptv_live_stream_start(const MiniIptvChannel *channel,
                               MiniIptvStageInfo *initial_info,
                               MiniIptvBoundaryReason relock_reason,
                               MiniIptvCancelFunction should_cancel,
                               void *cancel_userdata) {
    HlsMediaPlaylist media;
    HlsMediaPlaylist audio_media;
    size_t selected;
    size_t first;
    size_t initial_segment_target;
    const AdaptiveProfileEntry *adaptive_profile;
    uint8_t requested_lag;
    uint64_t initial_tune_deadline_ms;
    Thread producer;
    int result;
    if (!channel || !channel->url[0] || !initial_info)
        return MINIIPTV_STAGE_INVALID_ARGUMENT;
    ensure_stream_lock();
    LightLock_Lock(&stream_lifecycle_lock);
    miniiptv_live_tune_phase_begin(
        MINIIPTV_TUNE_PHASE_OLD_STREAM_CLEANUP);
    live_stream_stop_serialized();
    miniiptv_live_tune_phase_complete(
        MINIIPTV_TUNE_PHASE_OLD_STREAM_CLEANUP);
    /* A cancel pressed while the previous producer was being joined belongs
     * to this tune, not the resettable old stream. Check the app-owned token
     * before clearing and initializing stream state. */
    if (should_cancel && should_cancel(cancel_userdata)) {
        miniiptv_live_tune_fail(MINIIPTV_STAGE_CANCELLED);
        LightLock_Unlock(&stream_lifecycle_lock);
        return MINIIPTV_STAGE_CANCELLED;
    }
    /* Joining the previous producer is serialized safety work, not part of
     * this signal's network allowance. Start the 30 second tune deadline only
     * after the old stream is fully gone. */
    initial_tune_deadline_ms = osGetTime() + STREAM_INITIAL_TUNE_TIMEOUT_MS;
    adaptive_profile = adaptive_profile_lookup(channel);
    requested_lag = adaptive_profile
        ? adaptive_profile->recommended_lag_segments : 2u;
    if (requested_lag < 1u) requested_lag = 1u;
    if (requested_lag > 3u) requested_lag = 3u;

    LightLock_Lock(&stream_lock);
    memset(&stream, 0, sizeof(stream));
    stream_generation++;
    if (stream_generation == 0u) stream_generation++;
    stream.generation = stream_generation;
    stream.starting = true;
    miniiptv_buffer_shadow_reset(&stream.buffer_shadow, 6000u);
    stream.channel = *channel;
    stream.relock_reason = relock_reason;
    if (relock_reason != MINIIPTV_BOUNDARY_NONE)
        automatic_relock_count++;
    stream.automatic_relocks = automatic_relock_count;
    stream.adaptive_profile_hit = adaptive_profile != NULL;
    stream.startup_lag_segments = requested_lag;
    stream.producer_state = MINIIPTV_PRODUCER_STARTING;
    stream.initial_tune_active = true;
    stream.initial_tune_deadline_ms = initial_tune_deadline_ms;
    stream.initial_tune_should_cancel = should_cancel;
    stream.initial_tune_cancel_userdata = cancel_userdata;
    LightLock_Unlock(&stream_lock);
    memset(initial_info, 0, sizeof(*initial_info));
    initial_info->segment_limit_bytes = MINIIPTV_SEGMENT_LIMIT;

    for (unsigned int retry = 0;; retry++) {
        result = resolve_initial_playlist(channel, &media);
        if (!retryable_network_stage_error(result) ||
            retry >= STREAM_NETWORK_RETRY_COUNT)
            break;
        result = wait_before_network_retry(retry);
        if (result != MINIIPTV_STAGE_OK) break;
    }
    if (result != MINIIPTV_STAGE_OK) goto failure;
    memset(&audio_media, 0, sizeof(audio_media));
    if (stream.separate_audio) {
        for (unsigned int retry = 0;; retry++) {
            result = fetch_audio_playlist(&audio_media);
            if (!retryable_network_stage_error(result) ||
                retry >= STREAM_NETWORK_RETRY_COUNT)
                break;
            result = wait_before_network_retry(retry);
            if (result != MINIIPTV_STAGE_OK) break;
        }
        if (result != MINIIPTV_STAGE_OK && stream.rendition_cache_hit &&
            result != MINIIPTV_STAGE_CANCELLED &&
            result != MINIIPTV_STAGE_TUNE_TIMEOUT) {
            rendition_cache_invalidate(channel);
            stream.rendition_cache_hit = false;
            result = resolve_initial_playlist_uncached(channel, &media);
            if (result == MINIIPTV_STAGE_OK && stream.separate_audio)
                result = fetch_audio_playlist(&audio_media);
        }
        if (result != MINIIPTV_STAGE_OK) goto failure;
    }
    stream.target_duration = media.target_duration ? media.target_duration : 6;
    miniiptv_buffer_shadow_set_target(&stream.buffer_shadow,
        stream.target_duration > UINT32_MAX / 1000u
            ? UINT32_MAX : stream.target_duration * 1000u,
        osGetTime());
    /* A complete transport-stream segment gives FFmpeg a reliable PAT/PMT,
     * SPS/PPS, and keyframe before the player opens. Hardware testing showed
     * that handing off a partially downloaded segment caused a long white
     * screen and intermittent failure to produce a first frame. */
    /* Cold tunes stage two complete segments so FFmpeg/MVD startup does not
     * consume the entire reserve before the producer can refill it. A proven
     * warm profile with healthy headroom and no underruns keeps the faster
     * one-segment path. Both paths remain bounded by the existing ring and
     * atomic segment ceilings. */
    initial_segment_target =
        relock_reason == MINIIPTV_BOUNDARY_NONE && adaptive_profile &&
        adaptive_profile->headroom_permille >= 1500u &&
        adaptive_profile->total_underruns == 0u
            ? STREAM_WARM_INITIAL_SEGMENTS
            : STREAM_COLD_INITIAL_SEGMENTS;
    {
        size_t newest = media.count > 0 ? media.count - 1u : 0u;
        size_t lag = requested_lag;
        size_t previous_safe;
        if (media.count <= 1u) lag = 0u;
        else if (lag > newest) lag = newest;
        first = newest - lag;
        previous_safe = newest > 0 ? newest - 1u : 0u;
        /* A deeper start must not span a discontinuity. The first selected
         * segment may itself carry the marker: this is a fresh decoder and
         * that marker describes the boundary before the segment. */
        for (size_t i = first + 1u; i < previous_safe; i++) {
            if (media.segments[i].discontinuity) {
                first = previous_safe;
                lag = newest - first;
                break;
            }
        }
        selected = media.count > first ? media.count - first : 0u;
        if (selected > initial_segment_target)
            selected = initial_segment_target;
        /* Never combine segments across a format/timestamp boundary during
         * initial staging. Leave the boundary for a future clean relock. */
        for (size_t i = first + 1u; i < first + selected; i++) {
            if (media.segments[i].discontinuity) {
                selected = i - first;
                break;
            }
        }
        stream.startup_lag_segments = (uint8_t)lag;
    }
    if (selected == 0) {
        result = MINIIPTV_STAGE_MEDIA_INVALID;
        goto failure;
    }
    miniiptv_live_tune_phase_begin(MINIIPTV_TUNE_PHASE_INITIAL_SEGMENT);
    tune_segment_plan(0, (unsigned int)selected);
    for (size_t i = first; i < first + selected; i++) {
        size_t bytes = 0;
        const HlsSegment *audio_segment = NULL;
        if (stream.separate_audio) {
            audio_segment = matching_audio_segment(&media, i, &audio_media);
            if (!audio_segment) {
                result = MINIIPTV_STAGE_TS_MUX_FAILED;
                goto failure;
            }
            if (i > first && audio_segment->discontinuity) break;
        }
        for (unsigned int retry = 0;; retry++) {
            result = stream_segment(&media.segments[i], audio_segment,
                                    &bytes);
            if (!retryable_network_stage_error(result) ||
                retry >= STREAM_NETWORK_RETRY_COUNT)
                break;
            result = wait_before_network_retry(retry);
            if (result != MINIIPTV_STAGE_OK) break;
        }
        LightLock_Lock(&stream_lock);
        initial_info->attempted_segment_bytes =
            stream.attempted_segment_bytes;
        initial_info->reported_segment_bytes =
            stream.reported_segment_bytes;
        initial_info->last_network_result = stream.last_network_result;
        LightLock_Unlock(&stream_lock);
        if (result != MINIIPTV_STAGE_OK) goto failure;
        if (initial_info->segments_staged == 0)
            initial_info->first_sequence = media.segments[i].sequence;
        initial_info->last_sequence = media.segments[i].sequence;
        initial_info->segments_staged++;
        initial_info->bytes_staged += bytes;
        initial_info->duration_staged += media.segments[i].duration;
        tune_segment_plan((unsigned int)initial_info->segments_staged,
                          (unsigned int)selected);
    }
    miniiptv_live_tune_phase_complete(MINIIPTV_TUNE_PHASE_INITIAL_SEGMENT);
    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) goto failure;
    snprintf(initial_info->media_url, sizeof(initial_info->media_url), "%s",
             stream.media_url);
    LightLock_Lock(&stream_lock);
    /* HlsMediaPlaylist owns fixed-size URL storage, so this is a bounded deep
     * copy rather than a pointer into resolve_initial_playlist()'s responses
     * or this function's stack.  The producer is the only consumer. */
    stream.producer_seed = media;
    stream.audio_producer_seed = audio_media;
    stream.producer_seed_available = true;
    stream.initial_tune_active = false;
    stream.initial_tune_should_cancel = NULL;
    stream.initial_tune_cancel_userdata = NULL;
    stream.producer_running = true;
    LightLock_Unlock(&stream_lock);
    /* The player's real-time packet reader occupies core 1.  Keeping TLS/HLS
     * downloads at normal priority on that same core slowly drained the live
     * ring even when Wi-Fi had enough throughput.  New 3DS has core 2
     * available, so run the producer there and let it compete fairly with the
     * mostly-blocked hardware decode worker. */
    producer = threadCreate(producer_main, NULL, 96 * 1024,
                            DEF_THREAD_PRIORITY_HIGH,
                            Util_is_core_available(2) ? 2 : 1, false);
    if (!producer) {
        LightLock_Lock(&stream_lock);
        stream.producer_running = false;
        LightLock_Unlock(&stream_lock);
        result = MINIIPTV_STAGE_FILE_FAILED;
        goto failure;
    }
    LightLock_Lock(&stream_lock);
    stream.producer = producer;
    stream.starting = false;
    stream.initialized = true;
    __atomic_store_n(&stream_active, true, __ATOMIC_RELEASE);
    LightLock_Unlock(&stream_lock);
    LightLock_Unlock(&stream_lifecycle_lock);
    return MINIIPTV_STAGE_OK;

failure:
    miniiptv_live_tune_fail(result);
    live_stream_stop_serialized();
    LightLock_Unlock(&stream_lifecycle_lock);
    return result;
}

void miniiptv_live_stream_request_stop(void) {
    ensure_stream_lock();
    LightLock_Lock(&stream_lock);
    if (stream_session_exists_locked()) stream.stop_requested = true;
    LightLock_Unlock(&stream_lock);
}

static void live_stream_stop_serialized(void) {
    MiniIptvBufferShadow shadow_copy;
    MiniIptvBufferShadowSnapshot snapshot;
    MiniIptvChannel channel_copy;
    Thread producer = NULL;
    bool save_profile;
    while (true) {
        LightLock_Lock(&stream_lock);
        if (stream.stopping) {
            LightLock_Unlock(&stream_lock);
            Util_sleep(STREAM_SLEEP_US);
            continue;
        }
        if (!stream.starting && !stream.initialized) {
            LightLock_Unlock(&stream_lock);
            return;
        }
        stream.stopping = true;
        stream.initialized = false;
        __atomic_store_n(&stream_active, false, __ATOMIC_RELEASE);
        stream.stop_requested = true;
        producer = stream.producer;
        stream.producer = NULL;
        LightLock_Unlock(&stream_lock);
        break;
    }

    if (producer) {
        threadJoin(producer, UINT64_MAX);
        threadFree(producer);
    }

    LightLock_Lock(&stream_lock);
    shadow_copy = stream.buffer_shadow;
    channel_copy = stream.channel;
    save_profile = shadow_copy.valid_samples >= 3u;
    if (save_profile) {
        uint64_t now = osGetTime();
        miniiptv_buffer_shadow_snapshot(&shadow_copy, now, false, &snapshot);
        adaptive_profile_store(&channel_copy, &snapshot, now);
    }
    memset(&stream, 0, sizeof(stream));
    LightLock_Unlock(&stream_lock);
}

void miniiptv_live_stream_stop(void) {
    ensure_stream_lock();
    /* Wake an in-progress network transfer before waiting for its tune worker
     * to leave the serialized lifecycle section. */
    miniiptv_live_stream_request_stop();
    LightLock_Lock(&stream_lifecycle_lock);
    live_stream_stop_serialized();
    LightLock_Unlock(&stream_lifecycle_lock);
}

int miniiptv_live_stream_is_active(void) {
    return __atomic_load_n(&stream_active, __ATOMIC_ACQUIRE);
}

int miniiptv_live_stream_read(unsigned char *buffer, int buffer_size) {
    uint32_t generation;
    if (!buffer || buffer_size <= 0) return -1;
    ensure_stream_lock();
    LightLock_Lock(&stream_lock);
    if (!stream.initialized || stream.stopping) {
        LightLock_Unlock(&stream_lock);
        return -1;
    }
    generation = stream.generation;
    LightLock_Unlock(&stream_lock);
    while (true) {
        size_t available;
        size_t contiguous;
        size_t chunk;
        size_t rebuffer_target;
        bool finished;
        LightLock_Lock(&stream_lock);
        if (stream.generation != generation) {
            LightLock_Unlock(&stream_lock);
            return 0;
        }
        stream.reader_started = true;
        available = stream.ring_count;
        rebuffer_target = rebuffer_target_bytes_locked();
        finished = stream.stop_requested || !stream.producer_running;
        /* Only block FFmpeg after a real underrun. Holding back readable data
         * at a synthetic low-water mark caused visible stalls on healthy
         * low-bitrate channels. */
        if (stream.bytes_read > 0 && available == 0 &&
            !stream.rebuffering && !finished) {
            uint64_t now = osGetTime();
            uint32_t target_duration_ms;
            MiniIptvBufferShadowSnapshot snapshot;
            MiniIptvRecoveryPlan recovery;
            stream.rebuffering = true;
            stream.underruns++;
            miniiptv_buffer_shadow_note_ring(&stream.buffer_shadow, 0, true);
            miniiptv_buffer_shadow_begin_refill(&stream.buffer_shadow, now);
            miniiptv_buffer_shadow_snapshot(&stream.buffer_shadow, now, true,
                                            &snapshot);
            target_duration_ms = stream.target_duration > UINT32_MAX / 1000u
                ? UINT32_MAX : stream.target_duration * 1000u;
            miniiptv_buffer_shadow_recovery_plan(
                &snapshot, (uint32_t)stream.last_segment_bytes,
                target_duration_ms, &recovery);
            stream.rebuffer_target_bytes = recovery.target_bytes;
            stream.rebuffer_wait_limit_ms = recovery.maximum_wait_ms;
            stream.rebuffer_deadline_ms = now + recovery.maximum_wait_ms;
            rebuffer_target = stream.rebuffer_target_bytes;
        }
        if (stream.rebuffering &&
            (available >= rebuffer_target ||
             (available > 0 && stream.rebuffer_deadline_ms > 0 &&
              osGetTime() >= stream.rebuffer_deadline_ms))) {
            uint64_t now = osGetTime();
            stream.rebuffering = false;
            stream.rebuffer_deadline_ms = 0;
            miniiptv_buffer_shadow_end_refill(&stream.buffer_shadow, now);
            miniiptv_buffer_shadow_note_ring(&stream.buffer_shadow,
                (uint32_t)available, true);
        }
        if ((!stream.rebuffering || finished) && available > 0) {
            contiguous = STREAM_RING_SIZE - stream.ring_read;
            chunk = (size_t)buffer_size;
            if (chunk > available) chunk = available;
            if (chunk > contiguous) chunk = contiguous;
            memcpy(buffer, stream_ring + stream.ring_read, chunk);
            stream.ring_read = (stream.ring_read + chunk) % STREAM_RING_SIZE;
            stream.ring_count -= chunk;
            stream.bytes_read += chunk;
            /* This is deliberately clock-free and division-free: it records
             * exact reserve minima/risk on the hot FFmpeg read path without
             * extending the shared-lock hold with shadow calculations. */
            miniiptv_buffer_shadow_note_ring(&stream.buffer_shadow,
                (uint32_t)stream.ring_count, true);
            LightLock_Unlock(&stream_lock);
            return (int)chunk;
        }
        LightLock_Unlock(&stream_lock);
        if (finished && available == 0) return 0;
        Util_sleep(STREAM_SLEEP_US);
    }
}

void miniiptv_live_stream_get_stats(size_t *buffered_bytes,
                                    unsigned long *downloaded_segments,
                                    unsigned long *read_kib,
                                    unsigned long *underruns,
                                    int *last_error) {
    if (buffered_bytes) *buffered_bytes = 0;
    if (downloaded_segments) *downloaded_segments = 0;
    if (read_kib) *read_kib = 0;
    if (underruns) *underruns = 0;
    if (last_error) *last_error = 0;
    ensure_stream_lock();
    LightLock_Lock(&stream_lock);
    if (!stream.initialized || stream.stopping) {
        LightLock_Unlock(&stream_lock);
        return;
    }
    if (buffered_bytes) *buffered_bytes = stream.ring_count;
    if (downloaded_segments)
        *downloaded_segments = stream.downloaded_segments;
    if (read_kib) *read_kib = (unsigned long)(stream.bytes_read / 1024u);
    if (underruns) *underruns = stream.underruns;
    if (last_error) *last_error = stream.last_error;
    LightLock_Unlock(&stream_lock);
}

void miniiptv_live_stream_get_info(MiniIptvLiveInfo *info) {
    MiniIptvBufferShadow shadow_copy;
    uint64_t now;
    if (!info) return;
    memset(info, 0, sizeof(*info));
    ensure_stream_lock();
    LightLock_Lock(&stream_lock);
    if (!stream.initialized || stream.stopping) {
        LightLock_Unlock(&stream_lock);
        return;
    }
    miniiptv_buffer_shadow_note_ring(&stream.buffer_shadow,
        (uint32_t)stream.ring_count, stream.reader_started);
    snprintf(info->channel_name, sizeof(info->channel_name), "%s",
             stream.channel.name);
    snprintf(info->codecs, sizeof(info->codecs), "%s", stream.variant_codecs);
    info->bandwidth = stream.variant_bandwidth;
    info->measured_bandwidth = measured_bandwidth_locked();
    info->network_bandwidth = stream.network_bandwidth;
    info->sequence_resyncs = stream.sequence_resyncs;
    info->oversized_segment_skips = stream.oversized_segment_skips;
    info->automatic_relocks = stream.automatic_relocks;
    info->last_segment_bytes = stream.last_segment_bytes;
    info->attempted_segment_bytes = stream.attempted_segment_bytes;
    info->reported_segment_bytes = stream.reported_segment_bytes;
    info->segment_limit_bytes = MINIIPTV_SEGMENT_LIMIT;
    info->last_download_milliseconds = stream.last_download_milliseconds;
    info->last_segment_milliseconds = stream.last_segment_milliseconds;
    info->rebuffer_target_bytes = rebuffer_target_bytes_locked();
    info->rebuffer_wait_limit_milliseconds = stream.rebuffer_wait_limit_ms;
    info->buffered_milliseconds = buffered_milliseconds_locked();
    info->width = stream.variant_width;
    info->height = stream.variant_height;
    info->last_network_result = stream.last_network_result;
    info->rebuffering = stream.rebuffering;
    info->rendition_cache_hit = stream.rendition_cache_hit;
    info->adaptive_profile_hit = stream.adaptive_profile_hit;
    info->startup_lag_segments = stream.startup_lag_segments;
    info->boundary_reason = stream.boundary_reason;
    info->relock_reason = stream.relock_reason;
    info->producer_state = stream.producer_state;
    shadow_copy = stream.buffer_shadow;
    LightLock_Unlock(&stream_lock);
    now = osGetTime();
    miniiptv_buffer_shadow_snapshot(&shadow_copy, now, info->rebuffering,
        &info->shadow);
}

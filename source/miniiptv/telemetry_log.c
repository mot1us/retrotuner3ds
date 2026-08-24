#include "miniiptv/telemetry_log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "miniiptv/hls_prefetch.h"

#define LOG_BUFFER_BYTES 8192u
#define LOG_FAST_SAMPLE_INTERVAL_MS 2000u
#define LOG_STEADY_SAMPLE_INTERVAL_MS 10000u
#define LOG_FAST_WINDOW_MS 60000u
#define LOG_FLUSH_INTERVAL_MS 10000u
#define LOG_VERSION_BYTES 32u
#define LOG_CHANNEL_BYTES 128u
#define LOG_STATE_BYTES 24u
#define LOG_CSV_FIELD_BYTES 260u
#define LOG_LINE_BYTES 1024u

typedef struct {
    FILE *file;
    char buffer[LOG_BUFFER_BYTES];
    char version[LOG_VERSION_BYTES];
    char last_channel[LOG_CHANNEL_BYTES];
    char last_app_state[LOG_STATE_BYTES];
    char last_shadow_state[LOG_STATE_BYTES];
    size_t buffer_used;
    size_t bytes_written;
    uint64_t session_started_ms;
    uint64_t last_sample_ms;
    uint64_t fast_sampling_until_ms;
    uint64_t last_flush_ms;
    uint64_t last_global_underruns;
    int64_t last_error;
    bool has_last;
    bool failed;
    bool capped;
} MiniIptvTelemetryLog;

static MiniIptvTelemetryLog telemetry_log;

static const char *safe_text(const char *text) {
    return text ? text : "";
}

static void copy_text(char *destination, size_t size, const char *source) {
    if (!destination || size == 0) return;
    snprintf(destination, size, "%s", safe_text(source));
}

static void csv_quote(char *destination, size_t size, const char *source) {
    size_t output = 0;
    const unsigned char *input = (const unsigned char *)safe_text(source);
    if (!destination || size == 0) return;
    if (size < 3) {
        destination[0] = '\0';
        return;
    }
    destination[output++] = '"';
    while (*input && output + 2 < size) {
        unsigned char character = *input++;
        if (character == '\r' || character == '\n') character = ' ';
        if (character == '"') {
            if (output + 3 >= size) break;
            destination[output++] = '"';
            destination[output++] = '"';
        } else {
            destination[output++] = (char)character;
        }
    }
    destination[output++] = '"';
    destination[output] = '\0';
}

static bool flush_buffer(void) {
    size_t written;
    if (!telemetry_log.file || telemetry_log.failed) return false;
    if (telemetry_log.buffer_used == 0) return true;
    written = fwrite(telemetry_log.buffer, 1, telemetry_log.buffer_used,
                     telemetry_log.file);
    if (written != telemetry_log.buffer_used ||
        fflush(telemetry_log.file) != 0) {
        telemetry_log.failed = true;
        telemetry_log.buffer_used = 0;
        return false;
    }
    telemetry_log.bytes_written += written;
    telemetry_log.buffer_used = 0;
    return true;
}

static bool append_line(const char *line, size_t length) {
    size_t projected;
    if (!telemetry_log.file || telemetry_log.failed || telemetry_log.capped ||
        !line || length == 0 || length > LOG_BUFFER_BYTES)
        return false;
    projected = telemetry_log.bytes_written + telemetry_log.buffer_used;
    if (projected >= MINIIPTV_TELEMETRY_LOG_MAX_BYTES ||
        length > MINIIPTV_TELEMETRY_LOG_MAX_BYTES - projected) {
        flush_buffer();
        telemetry_log.capped = true;
        return false;
    }
    if (length > LOG_BUFFER_BYTES - telemetry_log.buffer_used &&
        !flush_buffer())
        return false;
    memcpy(telemetry_log.buffer + telemetry_log.buffer_used, line, length);
    telemetry_log.buffer_used += length;
    return true;
}

int miniiptv_telemetry_log_rotate(const char *current_path,
                                  const char *previous_path) {
    if (!current_path || !previous_path || !current_path[0] ||
        !previous_path[0] || strcmp(current_path, previous_path) == 0)
        return -1;
    miniiptv_telemetry_log_close();
    if (remove(previous_path) != 0 && errno != ENOENT) return -2;
    if (rename(current_path, previous_path) != 0 && errno != ENOENT)
        return -3;
    return 0;
}

int miniiptv_telemetry_log_open(const char *path, const char *version,
                                uint64_t now_ms) {
    static const char header[] =
        "version,elapsed_ms,event,channel,app_state,tune_phase,shadow_state,"
        "producer_state,width,height,ring_bytes,buffered_ms,network_bps,"
        "content_bps,samples,headroom_permille,want_ms,rebuffer_target_bytes,"
        "rebuffer_wait_limit_ms,lag_segments,"
        "applied_lag_segments,profile_hit,segment_ms,download_ms,gap_ms,"
        "jitter_ms,rebuffering,recent_underruns,"
        "total_underruns,global_underruns,downloaded_segments,sequence_resyncs,"
        "oversized_segment_skips,no_new_streak,"
        "ring_min_bytes,ring_max_bytes,last_refill_ms,last_refill_commits,"
        "app_region_total_bytes,heap_total_bytes,heap_used_bytes,"
        "linear_total_bytes,linear_free_bytes,"
        "video_packets,video_decoded_frames,video_textures,"
        "video_presented_frames,audio_tracks,audio_state,"
        "audio_demux_packets,audio_frames,audio_buffers,audio_last_error,"
        "last_error\n";
    if (!path || !version) return -1;
    miniiptv_telemetry_log_close();
    memset(&telemetry_log, 0, sizeof(telemetry_log));
    telemetry_log.file = fopen(path, "wb");
    if (!telemetry_log.file) return -2;
    (void)setvbuf(telemetry_log.file, NULL, _IONBF, 0);
    copy_text(telemetry_log.version, sizeof(telemetry_log.version), version);
    telemetry_log.session_started_ms = now_ms;
    telemetry_log.fast_sampling_until_ms = now_ms + LOG_FAST_WINDOW_MS;
    telemetry_log.last_flush_ms = now_ms;
    if (!append_line(header, sizeof(header) - 1u) || !flush_buffer()) {
        miniiptv_telemetry_log_close();
        return -3;
    }
    return 0;
}

void miniiptv_telemetry_log_record(
    uint64_t now_ms, const MiniIptvTelemetrySample *sample) {
    char version[LOG_CSV_FIELD_BYTES];
    char channel[LOG_CSV_FIELD_BYTES];
    char app_state[LOG_CSV_FIELD_BYTES];
    char tune_phase[LOG_CSV_FIELD_BYTES];
    char shadow_state[LOG_CSV_FIELD_BYTES];
    char producer_state[LOG_CSV_FIELD_BYTES];
    char line[LOG_LINE_BYTES];
    const char *event = "SAMPLE";
    uint64_t elapsed_ms;
    bool channel_changed;
    bool app_state_changed;
    bool shadow_state_changed;
    bool underrun_changed;
    bool error_changed;
    bool sample_due;
    bool urgent;
    uint64_t sample_interval_ms;
    int64_t recorded_error;
    int length;

    if (!sample || !miniiptv_telemetry_log_is_enabled()) return;
    /* Cancellation is the expected result of B, L/R, START, and serialized
     * channel teardown. Keep it out of both the ERROR event stream and the
     * last_error column so normal user actions are not diagnosed as faults. */
    recorded_error = sample->last_error == MINIIPTV_STAGE_CANCELLED
        ? 0 : sample->last_error;
    elapsed_ms = now_ms >= telemetry_log.session_started_ms
        ? now_ms - telemetry_log.session_started_ms : 0;
    channel_changed = telemetry_log.has_last &&
        strcmp(telemetry_log.last_channel,
               safe_text(sample->channel_name)) != 0;
    app_state_changed = telemetry_log.has_last &&
        strcmp(telemetry_log.last_app_state,
               safe_text(sample->app_state)) != 0;
    shadow_state_changed = telemetry_log.has_last &&
        strcmp(telemetry_log.last_shadow_state,
               safe_text(sample->shadow_state)) != 0;
    underrun_changed = telemetry_log.has_last && !channel_changed &&
        sample->global_underruns > telemetry_log.last_global_underruns;
    error_changed = recorded_error != 0 &&
        (!telemetry_log.has_last ||
         recorded_error != telemetry_log.last_error);
    if (channel_changed)
        telemetry_log.fast_sampling_until_ms = now_ms + LOG_FAST_WINDOW_MS;
    sample_interval_ms = now_ms <= telemetry_log.fast_sampling_until_ms
        ? LOG_FAST_SAMPLE_INTERVAL_MS : LOG_STEADY_SAMPLE_INTERVAL_MS;
    sample_due = !telemetry_log.has_last ||
        (sample->periodic &&
         (now_ms < telemetry_log.last_sample_ms ||
          now_ms - telemetry_log.last_sample_ms >=
              sample_interval_ms));

    if (!telemetry_log.has_last)
        event = "START";
    else if (error_changed)
        event = "ERROR";
    else if (underrun_changed)
        event = "UNDERRUN";
    else if (channel_changed)
        event = "CHANNEL";
    else if (app_state_changed || shadow_state_changed)
        event = "STATE";
    else if (!sample_due)
        return;

    csv_quote(version, sizeof(version), telemetry_log.version);
    csv_quote(channel, sizeof(channel), sample->channel_name);
    csv_quote(app_state, sizeof(app_state), sample->app_state);
    csv_quote(tune_phase, sizeof(tune_phase), sample->tune_phase);
    csv_quote(shadow_state, sizeof(shadow_state), sample->shadow_state);
    csv_quote(producer_state, sizeof(producer_state),
              sample->producer_state);
    length = snprintf(
        line, sizeof(line),
        "%s,%" PRIu64 ",%s,%s,%s,%s,%s,%s,%" PRIu32 ",%" PRIu32
        ",%" PRIu64 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%u,%" PRIu32 ",%" PRIu32 ",%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRId64 "\n",
        version, elapsed_ms, event, channel, app_state, tune_phase,
        shadow_state, producer_state, sample->width, sample->height,
        sample->ring_bytes, sample->buffered_ms, sample->network_bps,
        sample->content_bps, sample->valid_samples,
        sample->headroom_permille, sample->desired_reserve_ms,
        sample->rebuffer_target_bytes, sample->rebuffer_wait_limit_ms,
        sample->recommended_lag_segments, sample->applied_lag_segments,
        sample->adaptive_profile_hit, sample->segment_ms,
        sample->download_ms, sample->commit_gap_ms,
        sample->commit_gap_deviation_ms, sample->rebuffering ? 1u : 0u,
        sample->recent_underruns, sample->total_underruns,
        sample->global_underruns, sample->downloaded_segments,
        sample->sequence_resyncs, sample->oversized_segment_skips,
        sample->no_new_poll_streak, sample->ring_min_bytes,
        sample->ring_max_bytes, sample->last_refill_ms,
        sample->last_refill_commits, sample->app_region_total_bytes,
        sample->heap_total_bytes, sample->heap_used_bytes,
        sample->linear_total_bytes, sample->linear_free_bytes,
        sample->video_packets, sample->video_decoded_frames,
        sample->video_textures, sample->video_presented_frames,
        sample->audio_tracks, sample->audio_state,
        sample->audio_demux_packets, sample->audio_frames,
        sample->audio_buffers, sample->audio_last_error,
        recorded_error);
    if (length <= 0 || (size_t)length >= sizeof(line) ||
        !append_line(line, (size_t)length))
        return;

    copy_text(telemetry_log.last_channel,
              sizeof(telemetry_log.last_channel), sample->channel_name);
    copy_text(telemetry_log.last_app_state,
              sizeof(telemetry_log.last_app_state), sample->app_state);
    copy_text(telemetry_log.last_shadow_state,
              sizeof(telemetry_log.last_shadow_state), sample->shadow_state);
    telemetry_log.last_global_underruns = sample->global_underruns;
    telemetry_log.last_error = recorded_error;
    telemetry_log.last_sample_ms = now_ms;
    telemetry_log.has_last = true;

    urgent = error_changed || underrun_changed || channel_changed;
    if (urgent || now_ms < telemetry_log.last_flush_ms ||
        now_ms - telemetry_log.last_flush_ms >= LOG_FLUSH_INTERVAL_MS) {
        if (flush_buffer()) telemetry_log.last_flush_ms = now_ms;
    }
}

void miniiptv_telemetry_log_close(void) {
    FILE *file = telemetry_log.file;
    if (file) {
        flush_buffer();
        (void)fclose(file);
    }
    memset(&telemetry_log, 0, sizeof(telemetry_log));
}

bool miniiptv_telemetry_log_is_enabled(void) {
    return telemetry_log.file && !telemetry_log.failed &&
        !telemetry_log.capped;
}

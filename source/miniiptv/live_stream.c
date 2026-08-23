#include "miniiptv/live_stream.h"

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniiptv/hls.h"
#include "miniiptv/network.h"
#include "system/util/thread_types.h"
#include "system/util/util.h"

#define STREAM_RING_SIZE (6u * 1024u * 1024u)
#define STREAM_INITIAL_SEGMENTS 1
#define STREAM_REBUFFER_TARGET_MS 3000u
#define STREAM_REBUFFER_MIN_BYTES (128u * 1024u)
#define STREAM_REBUFFER_MAX_BYTES (768u * 1024u)
#define STREAM_HIGH_WATER_BYTES (5u * 1024u * 1024u)
#define STREAM_SLEEP_US 10000ULL
#define STREAM_INITIAL_TUNE_TIMEOUT_MS 30000ULL
#define TS_PACKET_SIZE 188u

#if MINIIPTV_SEGMENT_LIMIT >= STREAM_RING_SIZE
#error "The atomic HLS segment limit must be smaller than the live ring"
#endif

#if STREAM_HIGH_WATER_BYTES >= STREAM_RING_SIZE
#error "The live high-water mark must be smaller than the live ring"
#endif

typedef struct {
    LightLock lock;
    MiniIptvChannel channel;
    char media_url[MINIIPTV_HLS_URL_MAX];
    Thread producer;
    bool initialized;
    bool stop_requested;
    bool producer_running;
    bool rebuffering;
    bool reader_started;
    bool initial_tune_active;
    bool initial_tune_timed_out;
    uint64_t initial_tune_deadline_ms;
    MiniIptvCancelFunction initial_tune_should_cancel;
    void *initial_tune_cancel_userdata;
    unsigned int target_duration;
    unsigned long variant_bandwidth;
    unsigned int variant_width;
    unsigned int variant_height;
    char variant_codecs[96];
    unsigned long last_sequence;
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
    unsigned long underruns;
    size_t attempted_segment_bytes;
    size_t reported_segment_bytes;
    int last_network_result;
    int last_error;
} LiveStream;

typedef struct {
    size_t total_size;
} SegmentWriter;

/* Static BSS storage uses ordinary application RAM, not scarce linear RAM. */
static unsigned char stream_ring[STREAM_RING_SIZE];
/* Commit complete segments atomically. A failed/truncated HTTP transfer must
 * never leave a partial access unit in the ring where FFmpeg/MVD can see it. */
static unsigned char segment_staging[MINIIPTV_SEGMENT_LIMIT];
static LiveStream stream;

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
    if (!stream.initialized) return true;
    LightLock_Lock(&stream.lock);
    stopped = stream.stop_requested;
    LightLock_Unlock(&stream.lock);
    return stopped;
}

static int curl_should_cancel(void *unused) {
    bool cancel;
    bool initial_tune_active;
    MiniIptvCancelFunction should_cancel;
    void *cancel_userdata;
    (void)unused;
    if (!stream.initialized) return 1;
    LightLock_Lock(&stream.lock);
    cancel = stream.stop_requested;
    initial_tune_active = stream.initial_tune_active;
    should_cancel = stream.initial_tune_should_cancel;
    cancel_userdata = stream.initial_tune_cancel_userdata;
    if (!cancel && initial_tune_active &&
        osGetTime() >= stream.initial_tune_deadline_ms) {
        stream.initial_tune_timed_out = true;
        cancel = true;
    }
    LightLock_Unlock(&stream.lock);
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
    if (!stream.initialized) return MINIIPTV_STAGE_CANCELLED;
    LightLock_Lock(&stream.lock);
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
    LightLock_Unlock(&stream.lock);
    if (result == MINIIPTV_STAGE_OK && initial_tune_active && should_cancel &&
        should_cancel(cancel_userdata))
        result = MINIIPTV_STAGE_CANCELLED;
    return result;
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
        LightLock_Lock(&stream.lock);
        if (stream.stop_requested) {
            LightLock_Unlock(&stream.lock);
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
            LightLock_Unlock(&stream.lock);
            return true;
        }
        can_wait = stream.reader_started;
        LightLock_Unlock(&stream.lock);
        /* Before playback starts there is no consumer that can free space.
         * Fail cleanly instead of deadlocking during initial buffering. */
        if (!can_wait) return false;
        Util_sleep(STREAM_SLEEP_US);
    }
}

static size_t segment_write_callback(const unsigned char *data, size_t size,
                                     void *userdata) {
    SegmentWriter *writer = userdata;
    if (!writer || (!data && size)) return 0;
    if (size > MINIIPTV_SEGMENT_LIMIT - writer->total_size) return 0;
    memcpy(segment_staging + writer->total_size, data, size);
    writer->total_size += size;
    return size;
}

static int stream_segment(const HlsSegment *segment, size_t *downloaded_size) {
    SegmentWriter writer;
    NetworkStreamMetrics metrics = {0};
    uint64_t download_started;
    uint64_t download_elapsed;
    bool too_large = false;
    bool valid_ts = false;
    bool committed = false;
    int result;
    if (!segment) return MINIIPTV_STAGE_INVALID_ARGUMENT;
    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) return result;
    memset(&writer, 0, sizeof(writer));
    download_started = osGetTime();
    result = network_stream_data(segment->url, stream.channel.user_agent,
                                 stream.channel.referrer,
                                 MINIIPTV_SEGMENT_LIMIT,
                                 segment_write_callback, &writer,
                                 curl_should_cancel, NULL, &metrics);
    download_elapsed = osGetTime() - download_started;
    if (result == -5) {
        int cancelled = cancellation_result();
        if (cancelled != MINIIPTV_STAGE_OK) result = cancelled;
    }
    too_large = result == MINIIPTV_NETWORK_TOO_LARGE;
    if (result == 0 && metrics.received_size == writer.total_size)
        valid_ts = is_complete_mpeg_ts(segment_staging, writer.total_size);
    if (result == 0 && valid_ts)
        committed = ring_commit_segment(segment_staging, writer.total_size);
    if (downloaded_size) *downloaded_size = committed ? writer.total_size : 0;

    LightLock_Lock(&stream.lock);
    stream.attempted_segment_bytes = metrics.received_size;
    stream.reported_segment_bytes = metrics.reported_size;
    stream.last_network_result = result;
    if (result == 0 && valid_ts && committed) {
        if (download_elapsed == 0) download_elapsed = 1;
        stream.downloaded_segments++;
        stream.last_segment_bytes = writer.total_size;
        stream.last_download_milliseconds = download_elapsed > 0xffffffffu
            ? 0xffffffffu : (unsigned int)download_elapsed;
        stream.last_segment_milliseconds = segment->duration > 0.0
            ? (unsigned int)(segment->duration * 1000.0 + 0.5) : 0;
        {
            uint64_t network_bps =
                ((uint64_t)writer.total_size * 8000u) / download_elapsed;
            stream.network_bandwidth = network_bps > 0xffffffffu
                ? 0xffffffffu : (unsigned long)network_bps;
        }
        if (segment->duration > 0.0) {
            stream.measured_segment_bytes += writer.total_size;
            stream.measured_segment_milliseconds +=
                (uint64_t)(segment->duration * 1000.0 + 0.5);
        }
        stream.last_sequence = segment->sequence;
        stream.last_error = 0;
    } else {
        if (too_large) {
            stream.last_error = MINIIPTV_STAGE_TOO_LARGE;
            /* Retrying the same oversized live segment can never succeed and
             * would repeatedly consume bandwidth and staging time. */
            stream.stop_requested = true;
        } else if (result == 0 && !valid_ts) {
            stream.last_error = MINIIPTV_STAGE_NOT_MPEG_TS;
            /* A successful HTTP body that is not a complete TS segment is a
             * permanent compatibility/safety failure, not a network retry. */
            stream.stop_requested = true;
        } else if (result == MINIIPTV_STAGE_CANCELLED ||
                   result == MINIIPTV_STAGE_TUNE_TIMEOUT) {
            stream.last_error = result;
        } else if (result != 0 || !committed) {
            stream.last_error = MINIIPTV_STAGE_SEGMENT_FETCH_FAILED;
        } else {
            stream.last_error = MINIIPTV_STAGE_NOT_MPEG_TS;
        }
    }
    result = (result == 0 && valid_ts && committed) ? MINIIPTV_STAGE_OK
                                                    : stream.last_error;
    LightLock_Unlock(&stream.lock);
    return result;
}

static int fetch_media_playlist(HlsMediaPlaylist *media) {
    NetworkTextResponse response = {0};
    const char *effective_url;
    int result;
    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) return result;
    result = network_get_data_cancelable(
        stream.media_url, stream.channel.user_agent, stream.channel.referrer,
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
    effective_url = response.final_url[0] ? response.final_url : stream.media_url;
    result = hls_parse_media_playlist(response.data, effective_url, media);
    if (result == 0 && response.final_url[0])
        snprintf(stream.media_url, sizeof(stream.media_url), "%s",
                 response.final_url);
    network_response_free(&response);
    if (result != 0) return MINIIPTV_STAGE_MEDIA_INVALID;
    if (!media->is_live || media->encrypted || media->uses_byte_ranges ||
        media->uses_init_map)
        return MINIIPTV_STAGE_UNSUPPORTED_HLS;
    return MINIIPTV_STAGE_OK;
}

static int resolve_initial_playlist(const MiniIptvChannel *channel,
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
    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) goto cleanup;
    snprintf(stream.media_url, sizeof(stream.media_url), "%s", media_url);
    result = MINIIPTV_STAGE_OK;

cleanup:
    network_response_free(&media_response);
    network_response_free(&root);
    return result;
}

static void producer_main(void *unused) {
    (void)unused;
    while (!stop_was_requested()) {
        HlsMediaPlaylist media;
        size_t buffered;
        bool added = false;
        int result;

        LightLock_Lock(&stream.lock);
        buffered = stream.ring_count;
        LightLock_Unlock(&stream.lock);
        if (buffered >= STREAM_HIGH_WATER_BYTES) {
            Util_sleep(50000);
            continue;
        }

        result = fetch_media_playlist(&media);
        if (result != MINIIPTV_STAGE_OK) {
            LightLock_Lock(&stream.lock);
            stream.last_error = result;
            LightLock_Unlock(&stream.lock);
            for (int retry = 0; retry < 10 && !stop_was_requested(); retry++)
                Util_sleep(100000);
            continue;
        }
        if (media.target_duration) stream.target_duration = media.target_duration;
        if (media.count > 0 && stream.last_sequence > 0 &&
            media.segments[media.count - 1].sequence < stream.last_sequence) {
            LightLock_Lock(&stream.lock);
            stream.last_error = MINIIPTV_STAGE_DISCONTINUITY;
            stream.stop_requested = true;
            LightLock_Unlock(&stream.lock);
            break;
        }
        for (size_t i = 0; i < media.count && !stop_was_requested(); i++) {
            size_t ignored_size = 0;
            if (media.segments[i].sequence <= stream.last_sequence) continue;
            if (media.segments[i].discontinuity ||
                (stream.last_sequence > 0 &&
                 media.segments[i].sequence != stream.last_sequence + 1)) {
                LightLock_Lock(&stream.lock);
                stream.last_error = MINIIPTV_STAGE_DISCONTINUITY;
                stream.stop_requested = true;
                LightLock_Unlock(&stream.lock);
                break;
            }
            result = stream_segment(&media.segments[i], &ignored_size);
            if (result != MINIIPTV_STAGE_OK) {
                for (int retry = 0; retry < 5 && !stop_was_requested(); retry++)
                    Util_sleep(100000);
                break;
            }
            added = true;
        }
        if (!added) {
            for (int i = 0; i < 10 && !stop_was_requested(); i++)
                Util_sleep(100000);
        }
    }
    LightLock_Lock(&stream.lock);
    stream.producer_running = false;
    LightLock_Unlock(&stream.lock);
    threadExit(0);
}

int miniiptv_live_stream_start(const MiniIptvChannel *channel,
                               MiniIptvStageInfo *initial_info,
                               MiniIptvCancelFunction should_cancel,
                               void *cancel_userdata) {
    HlsMediaPlaylist media;
    size_t selected;
    size_t first;
    size_t initial_segment_target;
    uint64_t initial_tune_deadline_ms;
    int result;
    if (!channel || !channel->url[0] || !initial_info)
        return MINIIPTV_STAGE_INVALID_ARGUMENT;
    initial_tune_deadline_ms = osGetTime() + STREAM_INITIAL_TUNE_TIMEOUT_MS;
    miniiptv_live_stream_stop();
    /* A cancel pressed while the previous producer was being joined belongs
     * to this tune, not the resettable old stream. Check the app-owned token
     * before clearing and initializing stream state. */
    if (should_cancel && should_cancel(cancel_userdata))
        return MINIIPTV_STAGE_CANCELLED;
    memset(&stream, 0, sizeof(stream));
    LightLock_Init(&stream.lock);
    stream.initialized = true;
    stream.channel = *channel;
    stream.initial_tune_active = true;
    stream.initial_tune_deadline_ms = initial_tune_deadline_ms;
    stream.initial_tune_should_cancel = should_cancel;
    stream.initial_tune_cancel_userdata = cancel_userdata;
    memset(initial_info, 0, sizeof(*initial_info));
    initial_info->segment_limit_bytes = MINIIPTV_SEGMENT_LIMIT;

    result = resolve_initial_playlist(channel, &media);
    if (result != MINIIPTV_STAGE_OK) goto failure;
    stream.target_duration = media.target_duration ? media.target_duration : 6;
    /* A complete transport-stream segment gives FFmpeg a reliable PAT/PMT,
     * SPS/PPS, and keyframe before the player opens. Hardware testing showed
     * that handing off a partially downloaded segment caused a long white
     * screen and intermittent failure to produce a first frame. */
    initial_segment_target = STREAM_INITIAL_SEGMENTS;
    /* Avoid the newest live-edge entry: some CDNs advertise it before every
     * edge node can serve it. The producer will fetch it immediately after
     * the player opens. */
    {
        size_t end = media.count;
        if (media.is_live && end > 1) end--;
        selected = end < initial_segment_target ? end : initial_segment_target;
        first = end - selected;
    }
    if (selected == 0) {
        result = MINIIPTV_STAGE_MEDIA_INVALID;
        goto failure;
    }
    for (size_t i = first; i < first + selected; i++) {
        size_t bytes = 0;
        if (media.segments[i].discontinuity) {
            result = MINIIPTV_STAGE_DISCONTINUITY;
            goto failure;
        }
        result = stream_segment(&media.segments[i], &bytes);
        LightLock_Lock(&stream.lock);
        initial_info->attempted_segment_bytes =
            stream.attempted_segment_bytes;
        initial_info->reported_segment_bytes =
            stream.reported_segment_bytes;
        initial_info->last_network_result = stream.last_network_result;
        LightLock_Unlock(&stream.lock);
        if (result != MINIIPTV_STAGE_OK) goto failure;
        if (initial_info->segments_staged == 0)
            initial_info->first_sequence = media.segments[i].sequence;
        initial_info->last_sequence = media.segments[i].sequence;
        initial_info->segments_staged++;
        initial_info->bytes_staged += bytes;
        initial_info->duration_staged += media.segments[i].duration;
    }
    result = cancellation_result();
    if (result != MINIIPTV_STAGE_OK) goto failure;
    snprintf(initial_info->media_url, sizeof(initial_info->media_url), "%s",
             stream.media_url);
    LightLock_Lock(&stream.lock);
    stream.initial_tune_active = false;
    stream.initial_tune_should_cancel = NULL;
    stream.initial_tune_cancel_userdata = NULL;
    stream.producer_running = true;
    LightLock_Unlock(&stream.lock);
    /* The player's real-time packet reader occupies core 1.  Keeping TLS/HLS
     * downloads at normal priority on that same core slowly drained the live
     * ring even when Wi-Fi had enough throughput.  New 3DS has core 2
     * available, so run the producer there and let it compete fairly with the
     * mostly-blocked hardware decode worker. */
    stream.producer = threadCreate(producer_main, NULL, 96 * 1024,
                                   DEF_THREAD_PRIORITY_HIGH,
                                   Util_is_core_available(2) ? 2 : 1, false);
    if (!stream.producer) {
        stream.producer_running = false;
        result = MINIIPTV_STAGE_FILE_FAILED;
        goto failure;
    }
    return MINIIPTV_STAGE_OK;

failure:
    miniiptv_live_stream_stop();
    return result;
}

void miniiptv_live_stream_request_stop(void) {
    if (!stream.initialized) return;
    LightLock_Lock(&stream.lock);
    stream.stop_requested = true;
    LightLock_Unlock(&stream.lock);
}

void miniiptv_live_stream_stop(void) {
    if (!stream.initialized) return;
    miniiptv_live_stream_request_stop();
    if (stream.producer) {
        threadJoin(stream.producer, UINT64_MAX);
        threadFree(stream.producer);
        stream.producer = NULL;
    }
    memset(&stream, 0, sizeof(stream));
}

int miniiptv_live_stream_is_active(void) {
    return stream.initialized;
}

int miniiptv_live_stream_read(unsigned char *buffer, int buffer_size) {
    if (!buffer || buffer_size <= 0 || !stream.initialized) return -1;
    while (true) {
        size_t available;
        size_t contiguous;
        size_t chunk;
        size_t rebuffer_target;
        bool finished;
        LightLock_Lock(&stream.lock);
        stream.reader_started = true;
        available = stream.ring_count;
        rebuffer_target = rebuffer_target_bytes_locked();
        finished = stream.stop_requested || !stream.producer_running;
        /* Only block FFmpeg after a real underrun. Holding back readable data
         * at a synthetic low-water mark caused visible stalls on healthy
         * low-bitrate channels. */
        if (stream.bytes_read > 0 && available == 0 &&
            !stream.rebuffering && !finished) {
            stream.rebuffering = true;
            stream.underruns++;
        }
        if (stream.rebuffering && available >= rebuffer_target)
            stream.rebuffering = false;
        if ((!stream.rebuffering || finished) && available > 0) {
            contiguous = STREAM_RING_SIZE - stream.ring_read;
            chunk = (size_t)buffer_size;
            if (chunk > available) chunk = available;
            if (chunk > contiguous) chunk = contiguous;
            memcpy(buffer, stream_ring + stream.ring_read, chunk);
            stream.ring_read = (stream.ring_read + chunk) % STREAM_RING_SIZE;
            stream.ring_count -= chunk;
            stream.bytes_read += chunk;
            LightLock_Unlock(&stream.lock);
            return (int)chunk;
        }
        LightLock_Unlock(&stream.lock);
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
    if (!stream.initialized) return;
    LightLock_Lock(&stream.lock);
    if (buffered_bytes) *buffered_bytes = stream.ring_count;
    if (downloaded_segments)
        *downloaded_segments = stream.downloaded_segments;
    if (read_kib) *read_kib = (unsigned long)(stream.bytes_read / 1024u);
    if (underruns) *underruns = stream.underruns;
    if (last_error) *last_error = stream.last_error;
    LightLock_Unlock(&stream.lock);
}

void miniiptv_live_stream_get_info(MiniIptvLiveInfo *info) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    if (!stream.initialized) return;
    LightLock_Lock(&stream.lock);
    snprintf(info->channel_name, sizeof(info->channel_name), "%s",
             stream.channel.name);
    snprintf(info->codecs, sizeof(info->codecs), "%s", stream.variant_codecs);
    info->bandwidth = stream.variant_bandwidth;
    info->measured_bandwidth = measured_bandwidth_locked();
    info->network_bandwidth = stream.network_bandwidth;
    info->last_segment_bytes = stream.last_segment_bytes;
    info->attempted_segment_bytes = stream.attempted_segment_bytes;
    info->reported_segment_bytes = stream.reported_segment_bytes;
    info->segment_limit_bytes = MINIIPTV_SEGMENT_LIMIT;
    info->last_download_milliseconds = stream.last_download_milliseconds;
    info->last_segment_milliseconds = stream.last_segment_milliseconds;
    info->rebuffer_target_bytes = rebuffer_target_bytes_locked();
    info->buffered_milliseconds = buffered_milliseconds_locked();
    info->width = stream.variant_width;
    info->height = stream.variant_height;
    info->last_network_result = stream.last_network_result;
    info->rebuffering = stream.rebuffering;
    LightLock_Unlock(&stream.lock);
}

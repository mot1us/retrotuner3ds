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
#define STREAM_REBUFFER_BYTES (768u * 1024u)
#define STREAM_HIGH_WATER_BYTES (5u * 1024u * 1024u)
#define STREAM_SLEEP_US 10000ULL
#define TS_PROBE_SIZE (188u * 2u)

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
    unsigned long downloaded_segments;
    unsigned long underruns;
    int last_error;
} LiveStream;

typedef struct {
    unsigned char probe[TS_PROBE_SIZE];
    size_t probe_size;
    size_t total_size;
    bool validated;
} SegmentWriter;

/* Static BSS storage uses ordinary application RAM, not scarce linear RAM. */
static unsigned char stream_ring[STREAM_RING_SIZE];
static LiveStream stream;

static bool stop_was_requested(void) {
    bool stopped;
    if (!stream.initialized) return true;
    LightLock_Lock(&stream.lock);
    stopped = stream.stop_requested;
    LightLock_Unlock(&stream.lock);
    return stopped;
}

static int curl_should_cancel(void *unused) {
    (void)unused;
    return stop_was_requested();
}

static bool looks_like_mpeg_ts(const unsigned char *data, size_t size) {
    if (!data || size < TS_PROBE_SIZE) return false;
    for (size_t offset = 0; offset < 188; offset++) {
        if (data[offset] == 0x47 && data[offset + 188] == 0x47)
            return true;
    }
    return false;
}

static size_t ring_write_bytes(const unsigned char *data, size_t size) {
    size_t written = 0;
    while (written < size) {
        size_t free_space;
        size_t contiguous;
        size_t chunk;
        if (stop_was_requested()) break;
        LightLock_Lock(&stream.lock);
        free_space = STREAM_RING_SIZE - stream.ring_count;
        contiguous = STREAM_RING_SIZE - stream.ring_write;
        chunk = size - written;
        if (chunk > free_space) chunk = free_space;
        if (chunk > contiguous) chunk = contiguous;
        if (chunk > 0) {
            memcpy(stream_ring + stream.ring_write, data + written, chunk);
            stream.ring_write = (stream.ring_write + chunk) % STREAM_RING_SIZE;
            stream.ring_count += chunk;
            stream.bytes_written += chunk;
        }
        LightLock_Unlock(&stream.lock);
        written += chunk;
        if (chunk == 0) {
            bool can_wait;
            LightLock_Lock(&stream.lock);
            can_wait = stream.reader_started;
            LightLock_Unlock(&stream.lock);
            /* Before playback starts there is no consumer that can free a
             * completely full ring. Fail cleanly instead of deadlocking a
             * high-bitrate channel during initial buffering. */
            if (!can_wait) break;
            Util_sleep(STREAM_SLEEP_US);
        }
    }
    return written;
}

static size_t segment_write_callback(const unsigned char *data, size_t size,
                                     void *userdata) {
    SegmentWriter *writer = userdata;
    size_t offset = 0;
    if (!writer || (!data && size)) return 0;
    if (size > MINIIPTV_SEGMENT_LIMIT - writer->total_size) return 0;
    writer->total_size += size;

    if (!writer->validated) {
        size_t needed = TS_PROBE_SIZE - writer->probe_size;
        size_t take = size < needed ? size : needed;
        memcpy(writer->probe + writer->probe_size, data, take);
        writer->probe_size += take;
        offset += take;
        if (writer->probe_size < TS_PROBE_SIZE) return size;
        if (!looks_like_mpeg_ts(writer->probe, writer->probe_size)) return 0;
        writer->validated = true;
        if (ring_write_bytes(writer->probe, writer->probe_size) !=
            writer->probe_size)
            return 0;
    }
    if (offset < size &&
        ring_write_bytes(data + offset, size - offset) != size - offset)
        return 0;
    return size;
}

static int stream_segment(const HlsSegment *segment, size_t *downloaded_size) {
    SegmentWriter writer;
    size_t bytes = 0;
    int result;
    if (!segment) return MINIIPTV_STAGE_INVALID_ARGUMENT;
    memset(&writer, 0, sizeof(writer));
    result = network_stream_data(segment->url, stream.channel.user_agent,
                                 stream.channel.referrer,
                                 MINIIPTV_SEGMENT_LIMIT,
                                 segment_write_callback, &writer,
                                 curl_should_cancel, NULL, &bytes);
    if (downloaded_size) *downloaded_size = bytes;

    LightLock_Lock(&stream.lock);
    if (result == 0 && writer.validated) {
        stream.downloaded_segments++;
        stream.last_sequence = segment->sequence;
        stream.last_error = 0;
    } else {
        stream.last_error = result != 0 ? MINIIPTV_STAGE_SEGMENT_FETCH_FAILED
                                        : MINIIPTV_STAGE_NOT_MPEG_TS;
        if (writer.validated && writer.total_size > 0)
            stream.last_sequence = segment->sequence;
    }
    result = (result == 0 && writer.validated) ? MINIIPTV_STAGE_OK
                                               : stream.last_error;
    LightLock_Unlock(&stream.lock);
    return result;
}

static int fetch_media_playlist(HlsMediaPlaylist *media) {
    NetworkTextResponse response = {0};
    const char *effective_url;
    int result;
    result = network_get_data(stream.media_url, stream.channel.user_agent,
                              stream.channel.referrer,
                              MINIIPTV_MANIFEST_LIMIT, &response);
    if (result != 0) return MINIIPTV_STAGE_MEDIA_FETCH_FAILED;
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

    if (network_get_data(channel->url, channel->user_agent, channel->referrer,
                         MINIIPTV_MANIFEST_LIMIT, &root) != 0)
        goto cleanup;
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
        if (network_get_data(selection.url, channel->user_agent,
                             channel->referrer, MINIIPTV_MANIFEST_LIMIT,
                             &media_response) != 0) {
            result = MINIIPTV_STAGE_MEDIA_FETCH_FAILED;
            goto cleanup;
        }
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
        for (size_t i = 0; i < media.count && !stop_was_requested(); i++) {
            size_t ignored_size = 0;
            if (media.segments[i].sequence <= stream.last_sequence) continue;
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
                               MiniIptvStageInfo *initial_info) {
    HlsMediaPlaylist media;
    size_t selected;
    size_t first;
    size_t initial_segment_target;
    int result;
    if (!channel || !channel->url[0] || !initial_info)
        return MINIIPTV_STAGE_INVALID_ARGUMENT;
    miniiptv_live_stream_stop();
    memset(&stream, 0, sizeof(stream));
    LightLock_Init(&stream.lock);
    stream.initialized = true;
    stream.channel = *channel;
    memset(initial_info, 0, sizeof(*initial_info));

    result = resolve_initial_playlist(channel, &media);
    if (result != MINIIPTV_STAGE_OK) goto failure;
    stream.target_duration = media.target_duration ? media.target_duration : 6;
    /* One validated transport-stream segment is enough for FFmpeg to open the
     * live input. Start the producer immediately after that first segment so
     * tuning and ring-buffer growth happen in parallel instead of making the
     * viewer wait for two complete segment downloads. */
    initial_segment_target = STREAM_INITIAL_SEGMENTS;
    selected = media.count < initial_segment_target ? media.count
                                                     : initial_segment_target;
    if (selected == 0) {
        result = MINIIPTV_STAGE_MEDIA_INVALID;
        goto failure;
    }
    first = media.count - selected;
    for (size_t i = first; i < media.count; i++) {
        size_t bytes = 0;
        result = stream_segment(&media.segments[i], &bytes);
        if (result != MINIIPTV_STAGE_OK) goto failure;
        if (initial_info->segments_staged == 0)
            initial_info->first_sequence = media.segments[i].sequence;
        initial_info->last_sequence = media.segments[i].sequence;
        initial_info->segments_staged++;
        initial_info->bytes_staged += bytes;
        initial_info->duration_staged += media.segments[i].duration;
    }
    snprintf(initial_info->media_url, sizeof(initial_info->media_url), "%s",
             stream.media_url);
    stream.producer_running = true;
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
        bool finished;
        LightLock_Lock(&stream.lock);
        stream.reader_started = true;
        available = stream.ring_count;
        finished = stream.stop_requested || !stream.producer_running;
        if (available == 0 && !stream.rebuffering && !finished) {
            stream.rebuffering = true;
            stream.underruns++;
        }
        if (stream.rebuffering && available >= STREAM_REBUFFER_BYTES)
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
    info->width = stream.variant_width;
    info->height = stream.variant_height;
    LightLock_Unlock(&stream.lock);
}

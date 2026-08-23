#include "miniiptv/hls_prefetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void response_free(NetworkTextResponse *response) {
    if (!response) return;
    free(response->data);
    memset(response, 0, sizeof(*response));
}

static const char *response_url(const NetworkTextResponse *response,
                                const char *fallback) {
    return response && response->final_url[0] ? response->final_url : fallback;
}

static int looks_like_mpeg_ts(const unsigned char *data, size_t size) {
    size_t offset;
    size_t max_offset;

    if (!data || size < 188) return 0;
    max_offset = size < 188 * 3 ? size - 188 : 188 * 2;
    for (offset = 0; offset <= max_offset; offset++) {
        size_t packets = 0;
        size_t cursor = offset;
        while (cursor < size && packets < 3) {
            if (data[cursor] != 0x47) break;
            packets++;
            cursor += 188;
        }
        if (packets >= 2 || (packets == 1 && size - offset < 376)) return 1;
    }
    return 0;
}

static int select_window(const HlsMediaPlaylist *playlist, size_t *first,
                         size_t *count) {
    size_t end;
    size_t selected;

    if (!playlist || !first || !count || playlist->count == 0) return -1;

    // For live media, avoid the newest entry because edge nodes may not all
    // have it yet. Keep at most three contiguous segments for a bounded test.
    end = playlist->count;
    if (playlist->is_live && end > 1) end--;
    selected = end < MINIIPTV_PREFETCH_SEGMENTS ? end : MINIIPTV_PREFETCH_SEGMENTS;
    if (selected == 0) return -1;

    *first = end - selected;
    *count = selected;
    return 0;
}

int miniiptv_stage_hls(const MiniIptvChannel *channel, const char *output_path,
                       MiniIptvFetchFunction fetch, MiniIptvStageInfo *info) {
    NetworkTextResponse root = {0};
    NetworkTextResponse media_response = {0};
    NetworkTextResponse segment = {0};
    HlsSelection selection;
    HlsMediaPlaylist media;
    const char *media_text;
    const char *media_url;
    char part_path[1024];
    size_t first = 0;
    size_t count = 0;
    size_t index;
    FILE *file = NULL;
    int result = MINIIPTV_STAGE_INVALID_ARGUMENT;

    if (!channel || !channel->url[0] || !output_path || !fetch || !info)
        return MINIIPTV_STAGE_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));

    if (snprintf(part_path, sizeof(part_path), "%s.part", output_path) < 0 ||
        strlen(output_path) + 5 >= sizeof(part_path))
        return MINIIPTV_STAGE_INVALID_ARGUMENT;

    if (fetch(channel->url, channel->user_agent, channel->referrer,
              MINIIPTV_MANIFEST_LIMIT, &root) != 0) {
        result = MINIIPTV_STAGE_MANIFEST_FETCH_FAILED;
        goto cleanup;
    }
    if (hls_select_stream(root.data, response_url(&root, channel->url),
                          &selection) != 0) {
        result = MINIIPTV_STAGE_MASTER_INVALID;
        goto cleanup;
    }

    if (selection.type == HLS_MASTER_PLAYLIST) {
        if (fetch(selection.url, channel->user_agent, channel->referrer,
                  MINIIPTV_MANIFEST_LIMIT, &media_response) != 0) {
            result = MINIIPTV_STAGE_MEDIA_FETCH_FAILED;
            goto cleanup;
        }
        media_text = media_response.data;
        media_url = response_url(&media_response, selection.url);
    } else {
        media_text = root.data;
        media_url = response_url(&root, channel->url);
    }

    if (hls_parse_media_playlist(media_text, media_url, &media) != 0) {
        result = MINIIPTV_STAGE_MEDIA_INVALID;
        goto cleanup;
    }
    if (media.encrypted || media.uses_byte_ranges || media.uses_init_map) {
        result = MINIIPTV_STAGE_UNSUPPORTED_HLS;
        goto cleanup;
    }
    if (select_window(&media, &first, &count) != 0) {
        result = MINIIPTV_STAGE_MEDIA_INVALID;
        goto cleanup;
    }
    for (index = first + 1; index < first + count; index++) {
        if (media.segments[index].discontinuity) {
            result = MINIIPTV_STAGE_DISCONTINUITY;
            goto cleanup;
        }
    }

    remove(part_path);
    file = fopen(part_path, "wb");
    if (!file) {
        result = MINIIPTV_STAGE_FILE_FAILED;
        goto cleanup;
    }

    for (index = first; index < first + count; index++) {
        const HlsSegment *entry = &media.segments[index];
        if (fetch(entry->url, channel->user_agent, channel->referrer,
                  MINIIPTV_SEGMENT_LIMIT, &segment) != 0) {
            result = MINIIPTV_STAGE_SEGMENT_FETCH_FAILED;
            goto cleanup;
        }
        if (!looks_like_mpeg_ts((const unsigned char *)segment.data, segment.size)) {
            result = MINIIPTV_STAGE_NOT_MPEG_TS;
            goto cleanup;
        }
        if (segment.size > MINIIPTV_PREFETCH_LIMIT - info->bytes_staged) {
            result = MINIIPTV_STAGE_TOO_LARGE;
            goto cleanup;
        }
        if (fwrite(segment.data, 1, segment.size, file) != segment.size) {
            result = MINIIPTV_STAGE_FILE_FAILED;
            goto cleanup;
        }

        if (info->segments_staged == 0) info->first_sequence = entry->sequence;
        info->last_sequence = entry->sequence;
        info->segments_staged++;
        info->bytes_staged += segment.size;
        info->duration_staged += entry->duration;
        response_free(&segment);
    }

    if (fflush(file) != 0 || fclose(file) != 0) {
        file = NULL;
        result = MINIIPTV_STAGE_FILE_FAILED;
        goto cleanup;
    }
    file = NULL;

    remove(output_path);
    if (rename(part_path, output_path) != 0) {
        result = MINIIPTV_STAGE_FILE_FAILED;
        goto cleanup;
    }
    snprintf(info->media_url, sizeof(info->media_url), "%s", media_url);
    result = MINIIPTV_STAGE_OK;

cleanup:
    if (file) fclose(file);
    if (result != MINIIPTV_STAGE_OK) remove(part_path);
    response_free(&segment);
    response_free(&media_response);
    response_free(&root);
    return result;
}

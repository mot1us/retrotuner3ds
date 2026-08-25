#include "miniiptv/hls.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_bounded(char *dst, size_t dst_size, const char *src, size_t count) {
    if (!dst || dst_size == 0) return;
    if (count >= dst_size) count = dst_size - 1;
    memcpy(dst, src, count);
    dst[count] = '\0';
}

static char *trim(char *line) {
    char *end;
    while (*line && isspace((unsigned char)*line)) line++;
    end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return line;
}

int hls_resolve_url(const char *base_url, const char *reference, char *output, size_t output_size) {
    const char *scheme;
    const char *authority_end;
    const char *path_end;
    const char *suffix;
    size_t prefix_length;

    if (!base_url || !reference || !output || output_size == 0) return -1;
    if (strncmp(reference, "http://", 7) == 0 || strncmp(reference, "https://", 8) == 0) {
        if (strlen(reference) >= output_size) return -2;
        strcpy(output, reference);
        return 0;
    }

    scheme = strstr(base_url, "://");
    if (!scheme) return -3;

    if (reference[0] == '?') {
        suffix = strpbrk(scheme + 3, "?#");
        if (!suffix) suffix = base_url + strlen(base_url);
        prefix_length = (size_t)(suffix - base_url);
        if (prefix_length + strlen(reference) >= output_size) return -2;
        copy_bounded(output, output_size, base_url, prefix_length);
        strcat(output, reference);
        return 0;
    }
    if (reference[0] == '#') {
        suffix = strchr(scheme + 3, '#');
        if (!suffix) suffix = base_url + strlen(base_url);
        prefix_length = (size_t)(suffix - base_url);
        if (prefix_length + strlen(reference) >= output_size) return -2;
        copy_bounded(output, output_size, base_url, prefix_length);
        strcat(output, reference);
        return 0;
    }

    authority_end = strpbrk(scheme + 3, "/?#");
    if (!authority_end) authority_end = base_url + strlen(base_url);

    if (strncmp(reference, "//", 2) == 0) {
        prefix_length = (size_t)(scheme - base_url + 1);
        if (prefix_length + strlen(reference) >= output_size) return -2;
        copy_bounded(output, output_size, base_url, prefix_length);
        strcat(output, reference);
        return 0;
    }

    if (reference[0] == '/') {
        prefix_length = (size_t)(authority_end - base_url);
        if (prefix_length + strlen(reference) >= output_size) return -2;
        copy_bounded(output, output_size, base_url, prefix_length);
        strcat(output, reference);
        return 0;
    }

    if (*authority_end != '/') {
        prefix_length = (size_t)(authority_end - base_url);
        if (prefix_length + 1 + strlen(reference) >= output_size) return -2;
        copy_bounded(output, output_size, base_url, prefix_length);
        strcat(output, "/");
        strcat(output, reference);
        return 0;
    }

    path_end = strpbrk(authority_end, "?#");
    if (!path_end) path_end = base_url + strlen(base_url);
    while (path_end > authority_end && path_end[-1] != '/') path_end--;
    prefix_length = (size_t)(path_end - base_url);
    if (prefix_length + strlen(reference) >= output_size) return -2;
    copy_bounded(output, output_size, base_url, prefix_length);
    strcat(output, reference);
    return 0;
}

static const char *find_attribute(const char *line, const char *key) {
    const char *value = strchr(line, ':');
    size_t key_length = strlen(key);
    int quoted;

    if (value) value++;
    else value = line;
    while (*value) {
        while (*value == ',' || isspace((unsigned char)*value)) value++;
        if (strncmp(value, key, key_length) == 0)
            return value + key_length;

        quoted = 0;
        while (*value) {
            if (*value == '"') quoted = !quoted;
            else if (*value == ',' && !quoted) {
                value++;
                break;
            }
            value++;
        }
    }
    return NULL;
}

static unsigned long parse_bandwidth(const char *line) {
    const char *value = find_attribute(line, "BANDWIDTH=");
    char *end;
    unsigned long bandwidth;
    if (!value) return ULONG_MAX;
    bandwidth = strtoul(value, &end, 10);
    return end == value ? ULONG_MAX : bandwidth;
}

static void parse_resolution(const char *line, unsigned int *width, unsigned int *height) {
    const char *value = find_attribute(line, "RESOLUTION=");
    if (!value) return;
    (void)sscanf(value, "%ux%u", width, height);
}

static unsigned int parse_frame_rate_millihz(const char *line) {
    const char *value = find_attribute(line, "FRAME-RATE=");
    char *end;
    double frame_rate;
    if (!value) return 0;
    frame_rate = strtod(value, &end);
    if (end == value || !isfinite(frame_rate) || frame_rate <= 0.0 ||
        frame_rate > 1000.0)
        return 0;
    return (unsigned int)(frame_rate * 1000.0 + 0.5);
}

static void parse_codecs(const char *line, char *output, size_t output_size) {
    const char *value = find_attribute(line, "CODECS=\"");
    const char *end;
    if (!value) return;
    end = strchr(value, '"');
    if (!end) return;
    copy_bounded(output, output_size, value, (size_t)(end - value));
}

static void parse_quoted_attribute(const char *line, const char *key,
                                   char *output, size_t output_size) {
    const char *value = find_attribute(line, key);
    const char *end;
    if (!output || output_size == 0) return;
    output[0] = '\0';
    if (!value) return;
    end = strchr(value, '"');
    if (!end) return;
    copy_bounded(output, output_size, value, (size_t)(end - value));
}

static int64_t days_from_civil(int year, unsigned int month,
                               unsigned int day) {
    int era;
    unsigned int year_of_era;
    unsigned int day_of_year;
    unsigned int day_of_era;
    unsigned int shifted_month;
    year -= month <= 2u;
    era = (year >= 0 ? year : year - 399) / 400;
    year_of_era = (unsigned int)(year - era * 400);
    shifted_month = month > 2u ? month - 3u : month + 9u;
    day_of_year = (153u * shifted_month + 2u) / 5u + day - 1u;
    day_of_era = year_of_era * 365u + year_of_era / 4u -
                 year_of_era / 100u + day_of_year;
    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static int parse_program_date_time(const char *line, int64_t *milliseconds) {
    int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    unsigned int fraction = 0;
    unsigned int fraction_digits = 0;
    int timezone_sign = 0;
    unsigned int timezone_hour = 0;
    unsigned int timezone_minute = 0;
    const char *cursor;
    int64_t result;
    if (!line || !milliseconds ||
        sscanf(line, "%d-%u-%uT%u:%u:%u", &year, &month, &day, &hour,
               &minute, &second) != 6)
        return -1;
    if (month < 1u || month > 12u || day < 1u || day > 31u || hour > 23u ||
        minute > 59u || second > 60u)
        return -1;
    cursor = strchr(line, 'T');
    if (!cursor) return -1;
    cursor = strchr(cursor, ':');
    if (!cursor) return -1;
    cursor = strchr(cursor + 1, ':');
    if (!cursor) return -1;
    cursor++;
    while (isdigit((unsigned char)*cursor)) cursor++;
    if (*cursor == '.') {
        cursor++;
        while (isdigit((unsigned char)*cursor)) {
            if (fraction_digits < 3u) {
                fraction = fraction * 10u + (unsigned int)(*cursor - '0');
                fraction_digits++;
            }
            cursor++;
        }
        while (fraction_digits < 3u) {
            fraction *= 10u;
            fraction_digits++;
        }
    }
    if (*cursor == 'Z' || *cursor == 'z') {
        cursor++;
    } else if (*cursor == '+' || *cursor == '-') {
        timezone_sign = *cursor == '+' ? 1 : -1;
        cursor++;
        if (sscanf(cursor, "%u:%u", &timezone_hour, &timezone_minute) != 2 ||
            timezone_hour > 23u || timezone_minute > 59u)
            return -1;
        cursor += 5;
    } else {
        return -1;
    }
    if (*cursor != '\0') return -1;
    result = days_from_civil(year, month, day) * INT64_C(86400000);
    result += (int64_t)hour * INT64_C(3600000);
    result += (int64_t)minute * INT64_C(60000);
    result += (int64_t)second * INT64_C(1000) + fraction;
    result -= (int64_t)timezone_sign *
              ((int64_t)timezone_hour * INT64_C(3600000) +
               (int64_t)timezone_minute * INT64_C(60000));
    *milliseconds = result;
    return 0;
}

static int codecs_are_compatible(const char *codecs) {
    int has_video;
    if (!codecs || !*codecs) return 1;
    has_video = strstr(codecs, "avc1") != NULL ||
                strstr(codecs, "avc3") != NULL ||
                strstr(codecs, "h264") != NULL;
    if (strstr(codecs, "hvc1") || strstr(codecs, "hev1") ||
        strstr(codecs, "av01") || strstr(codecs, "vp09") ||
        strstr(codecs, "ac-3") || strstr(codecs, "ec-3") ||
        strstr(codecs, "opus"))
        return 0;
    return has_video;
}

static int key_method_is_none(const char *line) {
    const char *method = find_attribute(line, "METHOD=");
    if (!method || strncmp(method, "NONE", 4) != 0) return 0;
    return method[4] == '\0' || method[4] == ',' ||
           isspace((unsigned char)method[4]);
}

static double parse_duration(const char *line) {
    char *end;
    double duration = strtod(line + 8, &end);
    if (end == line + 8 || !isfinite(duration) || duration < 0.0) return -1.0;
    return duration;
}

int hls_select_stream(const char *manifest, const char *manifest_url, HlsSelection *selection) {
    char *copy;
    char *cursor;
    unsigned long pending_bandwidth = ULONG_MAX;
    unsigned long lowest_bandwidth = ULONG_MAX;
    unsigned int pending_width = 0;
    unsigned int pending_height = 0;
    unsigned int pending_frame_rate_millihz = 0;
    char pending_codecs[96] = {0};
    char pending_audio_group[64] = {0};
    struct {
        char group[64];
        char uri[MINIIPTV_HLS_URL_MAX];
    } audio_renditions[8];
    size_t audio_rendition_count = 0;
    int selected_variant = 0;
    int expecting_uri = 0;
    int saw_hls_header = 0;
    int saw_media_segment = 0;

    if (!manifest || !manifest_url || !selection) return -1;
    memset(selection, 0, sizeof(*selection));
    size_t length = strlen(manifest);
    copy = malloc(length + 1);
    if (!copy) return -2;
    memcpy(copy, manifest, length + 1);

    cursor = copy;
    while (*cursor) {
        char *next = strpbrk(cursor, "\r\n");
        char *line;
        if (next) {
            *next = '\0';
            line = trim(cursor);
            cursor = next + 1;
            while (*cursor == '\r' || *cursor == '\n') cursor++;
        } else {
            line = trim(cursor);
            cursor += strlen(cursor);
        }

        if (strcmp(line, "#EXTM3U") == 0) saw_hls_header = 1;
        else if (strncmp(line, "#EXT-X-MEDIA:", 13) == 0 &&
                 strstr(line, "TYPE=AUDIO") != NULL &&
                 audio_rendition_count < 8u) {
            char group[64];
            char uri[MINIIPTV_HLS_URL_MAX];
            parse_quoted_attribute(line, "GROUP-ID=\"", group,
                                   sizeof(group));
            parse_quoted_attribute(line, "URI=\"", uri, sizeof(uri));
            if (group[0] && uri[0] &&
                hls_resolve_url(manifest_url, uri,
                    audio_renditions[audio_rendition_count].uri,
                    sizeof(audio_renditions[audio_rendition_count].uri)) == 0) {
                copy_bounded(audio_renditions[audio_rendition_count].group,
                    sizeof(audio_renditions[audio_rendition_count].group),
                    group, strlen(group));
                audio_rendition_count++;
            }
        }
        else if (strncmp(line, "#EXT-X-STREAM-INF:", 18) == 0) {
            pending_bandwidth = parse_bandwidth(line);
            pending_width = pending_height = 0;
            pending_frame_rate_millihz = 0;
            pending_codecs[0] = '\0';
            parse_resolution(line, &pending_width, &pending_height);
            pending_frame_rate_millihz = parse_frame_rate_millihz(line);
            parse_codecs(line, pending_codecs, sizeof(pending_codecs));
            parse_quoted_attribute(line, "AUDIO=\"", pending_audio_group,
                                   sizeof(pending_audio_group));
            expecting_uri = 1;
        } else if (strncmp(line, "#EXTINF:", 8) == 0) {
            saw_media_segment = 1;
        } else if (*line && *line != '#' && expecting_uri) {
            if (codecs_are_compatible(pending_codecs) &&
                (!selected_variant || pending_bandwidth < lowest_bandwidth)) {
                if (hls_resolve_url(manifest_url, line, selection->url, sizeof(selection->url)) == 0) {
                    lowest_bandwidth = pending_bandwidth;
                    selection->width = pending_width;
                    selection->height = pending_height;
                    selection->frame_rate_millihz =
                        pending_frame_rate_millihz;
                    copy_bounded(selection->codecs, sizeof(selection->codecs), pending_codecs, strlen(pending_codecs));
                    copy_bounded(selection->audio_group,
                        sizeof(selection->audio_group), pending_audio_group,
                        strlen(pending_audio_group));
                    selected_variant = 1;
                }
            }
            expecting_uri = 0;
            pending_bandwidth = ULONG_MAX;
            pending_audio_group[0] = '\0';
        }
    }
    free(copy);

    if (!saw_hls_header) return -3;
    if (selected_variant) {
        for (size_t i = 0; i < audio_rendition_count; i++) {
            if (selection->audio_group[0] &&
                strcmp(selection->audio_group, audio_renditions[i].group) == 0) {
                copy_bounded(selection->audio_url,
                    sizeof(selection->audio_url), audio_renditions[i].uri,
                    strlen(audio_renditions[i].uri));
                selection->has_separate_audio = 1;
                break;
            }
        }
        selection->type = HLS_MASTER_PLAYLIST;
        selection->bandwidth = lowest_bandwidth == ULONG_MAX ? 0 : lowest_bandwidth;
        return 0;
    }
    if (saw_media_segment) {
        selection->type = HLS_MEDIA_PLAYLIST;
        selection->bandwidth = 0;
        if (strlen(manifest_url) >= sizeof(selection->url)) return -4;
        strcpy(selection->url, manifest_url);
        return 0;
    }
    return -5;
}

int hls_parse_media_playlist(const char *manifest, const char *manifest_url, HlsMediaPlaylist *playlist) {
    char *copy;
    char *cursor;
    double pending_duration = -1.0;
    int pending_discontinuity = 0;
    int pending_has_program_date_time = 0;
    int64_t pending_program_date_time_ms = 0;
    int saw_header = 0;
    int saw_endlist = 0;
    size_t segment_index = 0;

    if (!manifest || !manifest_url || !playlist) return -1;
    memset(playlist, 0, sizeof(*playlist));
    size_t length = strlen(manifest);
    copy = malloc(length + 1);
    if (!copy) return -2;
    memcpy(copy, manifest, length + 1);

    cursor = copy;
    while (*cursor) {
        char *next = strpbrk(cursor, "\r\n");
        char *line;
        if (next) {
            *next = '\0';
            line = trim(cursor);
            cursor = next + 1;
            while (*cursor == '\r' || *cursor == '\n') cursor++;
        } else {
            line = trim(cursor);
            cursor += strlen(cursor);
        }

        if (strcmp(line, "#EXTM3U") == 0) saw_header = 1;
        else if (strncmp(line, "#EXT-X-TARGETDURATION:", 22) == 0)
            playlist->target_duration = (unsigned int)strtoul(line + 22, NULL, 10);
        else if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0)
            playlist->media_sequence = strtoul(line + 22, NULL, 10);
        else if (strncmp(line, "#EXTINF:", 8) == 0)
            pending_duration = parse_duration(line);
        else if (strncmp(line, "#EXT-X-PROGRAM-DATE-TIME:", 25) == 0) {
            pending_has_program_date_time =
                parse_program_date_time(line + 25,
                                        &pending_program_date_time_ms) == 0;
        }
        else if (strncmp(line, "#EXT-X-KEY:", 11) == 0 && !key_method_is_none(line))
            playlist->encrypted = 1;
        else if (strncmp(line, "#EXT-X-BYTERANGE:", 17) == 0)
            playlist->uses_byte_ranges = 1;
        else if (strncmp(line, "#EXT-X-MAP:", 11) == 0)
            playlist->uses_init_map = 1;
        else if (strcmp(line, "#EXT-X-DISCONTINUITY") == 0)
            pending_discontinuity = 1;
        else if (strcmp(line, "#EXT-X-ENDLIST") == 0)
            saw_endlist = 1;
        else if (*line && *line != '#' && pending_duration >= 0.0) {
            HlsSegment segment;
            memset(&segment, 0, sizeof(segment));
            if (hls_resolve_url(manifest_url, line, segment.url, sizeof(segment.url)) != 0) {
                free(copy);
                return -3;
            }
            segment.duration = pending_duration;
            segment.program_date_time_ms = pending_program_date_time_ms;
            segment.has_program_date_time = pending_has_program_date_time;
            segment.discontinuity = pending_discontinuity;
            segment.sequence = playlist->media_sequence + segment_index++;
            if (playlist->count == MINIIPTV_HLS_MAX_SEGMENTS) {
                memmove(&playlist->segments[0], &playlist->segments[1],
                        sizeof(playlist->segments[0]) * (MINIIPTV_HLS_MAX_SEGMENTS - 1));
                playlist->count--;
            }
            playlist->segments[playlist->count++] = segment;
            pending_duration = -1.0;
            pending_has_program_date_time = 0;
            pending_discontinuity = 0;
        }
    }
    free(copy);
    if (!saw_header || playlist->count == 0) return -4;
    playlist->is_live = !saw_endlist;
    return 0;
}

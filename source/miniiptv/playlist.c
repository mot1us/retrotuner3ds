#include "miniiptv/playlist.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYLIST_FILE_LIMIT (128 * 1024)

static void copy_bounded(char *dst, size_t dst_size, const char *src, size_t count) {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
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

static const char *metadata_end(const char *line) {
    const char *cursor = line + 8;
    int quoted = 0;

    while (*cursor) {
        if (*cursor == '"') quoted = !quoted;
        else if (*cursor == ',' && !quoted) return cursor;
        cursor++;
    }
    return cursor;
}

static const char *find_attribute(const char *line, const char *key) {
    const char *cursor = line + 8;
    const char *end = metadata_end(line);
    size_t key_length = strlen(key);
    int quoted;

    while (cursor < end) {
        while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
        if ((size_t)(end - cursor) >= key_length &&
            strncmp(cursor, key, key_length) == 0)
            return cursor + key_length;

        quoted = 0;
        while (cursor < end) {
            if (*cursor == '"') quoted = !quoted;
            else if (!quoted && isspace((unsigned char)*cursor)) break;
            cursor++;
        }
    }
    return NULL;
}

static void attribute(const char *line, const char *key, char *out, size_t out_size) {
    const char *start = find_attribute(line, key);
    const char *end;
    if (!start) return;
    if (*start != '"') return;
    start++;
    end = strchr(start, '"');
    if (!end) return;
    copy_bounded(out, out_size, start, (size_t)(end - start));
}

static const char *display_name(const char *line) {
    const char *end = metadata_end(line);
    return *end == ',' ? end + 1 : NULL;
}

static void parse_extinf(const char *line, MiniIptvChannel *pending) {
    const char *name = display_name(line);
    const char *end;
    memset(pending, 0, sizeof(*pending));
    if (name) {
        while (*name && isspace((unsigned char)*name)) name++;
        end = name + strlen(name);
        while (end > name && isspace((unsigned char)end[-1])) end--;
    } else {
        end = NULL;
    }
    if (name && end > name)
        copy_bounded(pending->name, sizeof(pending->name), name, (size_t)(end - name));
    else
        copy_bounded(pending->name, sizeof(pending->name), "Unnamed channel", 15);
    attribute(line, "group-title=", pending->group, sizeof(pending->group));
    attribute(line, "http-user-agent=", pending->user_agent, sizeof(pending->user_agent));
    attribute(line, "http-referrer=", pending->referrer, sizeof(pending->referrer));
}

static void parse_vlc_option(const char *line, MiniIptvChannel *pending) {
    const char *value;
    if (strncmp(line, "#EXTVLCOPT:http-user-agent=", 27) == 0) {
        value = line + 27;
        copy_bounded(pending->user_agent, sizeof(pending->user_agent), value, strlen(value));
    } else if (strncmp(line, "#EXTVLCOPT:http-referrer=", 25) == 0) {
        value = line + 25;
        copy_bounded(pending->referrer, sizeof(pending->referrer), value, strlen(value));
    }
}

int playlist_parse_text(const char *text, MiniIptvPlaylist *playlist) {
    char *copy;
    char *cursor;
    MiniIptvChannel pending;
    int have_pending = 0;

    if (!text || !playlist) return -1;
    memset(playlist, 0, sizeof(*playlist));
    memset(&pending, 0, sizeof(pending));

    size_t length = strlen(text);
    copy = malloc(length + 1);
    if (!copy) return -2;
    memcpy(copy, text, length + 1);

    cursor = copy;
    while (*cursor && playlist->count < MINIIPTV_MAX_CHANNELS) {
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

        if (strncmp(line, "#EXTINF:", 8) == 0) {
            parse_extinf(line, &pending);
            have_pending = 1;
        } else if (strncmp(line, "#EXTVLCOPT:", 11) == 0 && have_pending) {
            parse_vlc_option(line, &pending);
        } else if (*line && *line != '#' && have_pending) {
            if ((strncmp(line, "http://", 7) == 0 || strncmp(line, "https://", 8) == 0) &&
                strlen(line) < sizeof(pending.url)) {
                copy_bounded(pending.url, sizeof(pending.url), line, strlen(line));
                playlist->channels[playlist->count++] = pending;
            }
            memset(&pending, 0, sizeof(pending));
            have_pending = 0;
        }
    }

    free(copy);
    return playlist->count ? 0 : -3;
}

int playlist_load_file(const char *path, MiniIptvPlaylist *playlist) {
    FILE *file;
    char *data;
    long length;
    size_t read_length;
    int result;

    if (!path || !playlist) return -1;
    file = fopen(path, "rb");
    if (!file) return -2;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return -3; }
    length = ftell(file);
    if (length <= 0 || length > PLAYLIST_FILE_LIMIT) { fclose(file); return -4; }
    rewind(file);
    data = malloc((size_t)length + 1);
    if (!data) { fclose(file); return -5; }
    read_length = fread(data, 1, (size_t)length, file);
    if (read_length != (size_t)length) {
        free(data);
        fclose(file);
        return -6;
    }
    fclose(file);
    data[read_length] = '\0';
    result = playlist_parse_text(data, playlist);
    free(data);
    return result;
}

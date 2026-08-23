#ifndef MINIIPTV_HLS_H
#define MINIIPTV_HLS_H

#include <stddef.h>

#define MINIIPTV_HLS_URL_MAX 1024
#define MINIIPTV_HLS_MAX_SEGMENTS 8

typedef enum {
    HLS_MEDIA_PLAYLIST = 0,
    HLS_MASTER_PLAYLIST = 1
} HlsPlaylistType;

typedef struct {
    HlsPlaylistType type;
    unsigned long bandwidth;
    unsigned int width;
    unsigned int height;
    char codecs[96];
    char url[MINIIPTV_HLS_URL_MAX];
} HlsSelection;

typedef struct {
    char url[MINIIPTV_HLS_URL_MAX];
    double duration;
    unsigned long sequence;
    int discontinuity;
} HlsSegment;

typedef struct {
    HlsSegment segments[MINIIPTV_HLS_MAX_SEGMENTS];
    size_t count;
    unsigned long media_sequence;
    unsigned int target_duration;
    int is_live;
    int encrypted;
    int uses_byte_ranges;
    int uses_init_map;
} HlsMediaPlaylist;

int hls_select_stream(const char *manifest, const char *manifest_url, HlsSelection *selection);
int hls_resolve_url(const char *base_url, const char *reference, char *output, size_t output_size);
int hls_parse_media_playlist(const char *manifest, const char *manifest_url, HlsMediaPlaylist *playlist);

#endif

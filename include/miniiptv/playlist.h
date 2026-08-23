#ifndef MINIIPTV_PLAYLIST_H
#define MINIIPTV_PLAYLIST_H

#include <stddef.h>

#define MINIIPTV_MAX_CHANNELS 32
#define MINIIPTV_NAME_MAX 64
#define MINIIPTV_GROUP_MAX 32
#define MINIIPTV_URL_MAX 1024
#define MINIIPTV_HEADER_MAX 256

typedef struct {
    char name[MINIIPTV_NAME_MAX];
    char group[MINIIPTV_GROUP_MAX];
    char url[MINIIPTV_URL_MAX];
    char user_agent[MINIIPTV_HEADER_MAX];
    char referrer[MINIIPTV_HEADER_MAX];
} MiniIptvChannel;

typedef struct {
    MiniIptvChannel channels[MINIIPTV_MAX_CHANNELS];
    size_t count;
} MiniIptvPlaylist;

int playlist_load_file(const char *path, MiniIptvPlaylist *playlist);
int playlist_parse_text(const char *text, MiniIptvPlaylist *playlist);

#endif

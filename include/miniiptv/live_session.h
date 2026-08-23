#ifndef MINIIPTV_LIVE_SESSION_H
#define MINIIPTV_LIVE_SESSION_H

#include "miniiptv/hls_prefetch.h"

#define MINIIPTV_LIVE_CACHE_DIR "sdmc:/3ds/retrotuner3ds/"
#define MINIIPTV_PLAYER_CACHE_DIR "/3ds/retrotuner3ds/"
#define MINIIPTV_LIVE_CACHE_NAME "prefetched-live.ts"
#define MINIIPTV_LIVE_CACHE_PATH MINIIPTV_LIVE_CACHE_DIR MINIIPTV_LIVE_CACHE_NAME

int miniiptv_live_session_init(void);
void miniiptv_live_session_exit(void);
int miniiptv_live_stage_channel(const MiniIptvChannel *channel,
                                MiniIptvStageInfo *info);

#endif

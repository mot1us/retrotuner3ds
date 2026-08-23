#include "miniiptv/live_session.h"

#include "miniiptv/network.h"

int miniiptv_live_session_init(void) {
    return network_init();
}

void miniiptv_live_session_exit(void) {
    network_exit();
}

int miniiptv_live_stage_channel(const MiniIptvChannel *channel,
                                MiniIptvStageInfo *info) {
    return miniiptv_stage_hls(channel, MINIIPTV_LIVE_CACHE_PATH,
                              network_get_data, info);
}

#include "miniiptv/live_app.h"

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "miniiptv/live_session.h"
#include "miniiptv/live_stream.h"
#include "miniiptv/playlist.h"
#include "system/draw/draw.h"
#include "system/util/hid_types.h"
#include "system/util/thread_types.h"
#include "system/util/util.h"
#include "video_player.h"

#define USER_PLAYLIST "sdmc:/3ds/retrotuner3ds/channels.m3u"

/* Retro broadcast palette (ABGR8888). */
#define UI_INK 0xFF21160Fu
#define UI_PANEL 0xFF3C2A1Du
#define UI_CREAM 0xFFB8EEFFu
#define UI_ORANGE 0xFF00A8FFu
#define UI_MINT 0xFF8FEA69u
#define UI_SHADOW 0xFF120C08u

typedef enum {
    LIVE_APP_NO_PLAYLIST = 0,
    LIVE_APP_IDLE,
    LIVE_APP_LOADING,
    LIVE_APP_READY,
    LIVE_APP_ERROR
} LiveAppState;

typedef struct {
    LightLock lock;
    MiniIptvPlaylist playlist;
    MiniIptvChannel pending_channel;
    MiniIptvStageInfo stage_info;
    size_t selected;
    Thread worker;
    LiveAppState state;
    char status[160];
    bool network_ready;
    bool awaiting_player_return;
    uint32_t player_return_generation;
} LiveApp;

static LiveApp app;

static void set_status_locked(LiveAppState state, const char *message) {
    app.state = state;
    snprintf(app.status, sizeof(app.status), "%s", message ? message : "");
}

static bool begin_player_handoff(void) {
    bool started;

    LightLock_Lock(&app.lock);
    if (app.state != LIVE_APP_READY || app.awaiting_player_return) {
        LightLock_Unlock(&app.lock);
        return false;
    }
    snprintf(app.status, sizeof(app.status), "%s",
             "Tuned. Starting live playback...");
    app.player_return_generation = Vid_query_playback_return_generation();
    app.awaiting_player_return = true;
    LightLock_Unlock(&app.lock);

    started = Vid_prepare_and_start_file("", MINIIPTV_LIVE_STREAM_URL);
    if (!started) {
        miniiptv_live_stream_stop();
        LightLock_Lock(&app.lock);
        app.awaiting_player_return = false;
        set_status_locked(LIVE_APP_ERROR,
                          "Player handoff failed. Press A to retry.");
        LightLock_Unlock(&app.lock);
    }
    Draw_set_refresh_needed(true);
    return started;
}

static void worker_main(void *unused) {
    MiniIptvStageInfo info;
    int result;
    bool auto_start = false;
    (void)unused;

    if (!app.network_ready) {
        result = miniiptv_live_session_init();
        if (result == 0) app.network_ready = true;
    } else {
        result = 0;
    }
    if (result == 0)
        result = miniiptv_live_stream_start(&app.pending_channel, &info);
    LightLock_Lock(&app.lock);
    if (result == MINIIPTV_STAGE_OK) {
        app.stage_info = info;
        app.state = LIVE_APP_READY;
        snprintf(app.status, sizeof(app.status),
                 "Signal locked: %lu seg, %.1fs, %lu KiB.",
                 (unsigned long)info.segments_staged, info.duration_staged,
                 (unsigned long)(info.bytes_staged / 1024));
        auto_start = true;
    } else {
        app.state = LIVE_APP_ERROR;
        snprintf(app.status, sizeof(app.status),
                 "Live staging failed: %d. Press A to retry.", result);
    }
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
    if (auto_start) begin_player_handoff();
    threadExit(0);
}

static void reap_worker_if_finished(void) {
    LiveAppState state;

    LightLock_Lock(&app.lock);
    state = app.state;
    LightLock_Unlock(&app.lock);
    if (app.worker && state != LIVE_APP_LOADING) {
        threadJoin(app.worker, UINT64_MAX);
        threadFree(app.worker);
        app.worker = NULL;
    }
}

static void update_player_return_locked(void) {
    uint32_t generation;

    if (!app.awaiting_player_return) return;
    generation = Vid_query_playback_return_generation();
    if (generation == app.player_return_generation) return;

    miniiptv_live_stream_stop();
    app.awaiting_player_return = false;
    app.state = LIVE_APP_IDLE;
    snprintf(app.status, sizeof(app.status),
             "OFF AIR // %.1f MiB linear free // A to retune",
             Util_check_free_linear_space() / 1048576.0);
}

static bool live_hid(const Hid_info *key) {
    LiveAppState state;
    size_t count;

    if (!key) return false;
    reap_worker_if_finished();

    LightLock_Lock(&app.lock);
    update_player_return_locked();
    state = app.state;
    count = app.playlist.count;

    if (state != LIVE_APP_LOADING && count && DEF_HID_PHY_PR(key->d_up)) {
        miniiptv_live_stream_stop();
        app.selected = app.selected == 0 ? count - 1 : app.selected - 1;
        set_status_locked(LIVE_APP_IDLE, "READY // press A to tune this signal");
        LightLock_Unlock(&app.lock);
        Draw_set_refresh_needed(true);
        return true;
    }
    if (state != LIVE_APP_LOADING && count && DEF_HID_PHY_PR(key->d_down)) {
        miniiptv_live_stream_stop();
        app.selected = (app.selected + 1) % count;
        set_status_locked(LIVE_APP_IDLE, "READY // press A to tune this signal");
        LightLock_Unlock(&app.lock);
        Draw_set_refresh_needed(true);
        return true;
    }

    if (DEF_HID_PHY_PR(key->a)) {
        if (state == LIVE_APP_READY) {
            LightLock_Unlock(&app.lock);
            begin_player_handoff();
            return true;
        }
        if ((state == LIVE_APP_IDLE || state == LIVE_APP_ERROR) && count) {
            app.pending_channel = app.playlist.channels[app.selected];
            set_status_locked(LIVE_APP_LOADING,
                              "TUNING > HLS SCAN > LIVE BUFFER...");
            LightLock_Unlock(&app.lock);
            app.worker = threadCreate(worker_main, NULL, 128 * 1024,
                                      DEF_THREAD_PRIORITY_NORMAL, 1, false);
            if (!app.worker) {
                LightLock_Lock(&app.lock);
                set_status_locked(LIVE_APP_ERROR,
                                  "Could not start the network worker.");
                LightLock_Unlock(&app.lock);
            }
            Draw_set_refresh_needed(true);
            return true;
        }
        if (state == LIVE_APP_LOADING) {
            LightLock_Unlock(&app.lock);
            return true;
        }
    }

    LightLock_Unlock(&app.lock);
    return state != LIVE_APP_READY && DEF_HID_PR_EM(key->a, 1);
}

static void live_draw(bool top_screen, uint32_t color, uint32_t back_color) {
    Draw_image_data pixel = Draw_get_empty_image();
    char channel_names[MINIIPTV_MAX_CHANNELS][MINIIPTV_NAME_MAX];
    LiveAppState state;
    size_t count;
    size_t selected;
    char status[160];
    char line[112];
    size_t i;

    (void)color;
    (void)back_color;

    LightLock_Lock(&app.lock);
    update_player_return_locked();
    count = app.playlist.count;
    for (i = 0; i < count; i++)
        snprintf(channel_names[i], sizeof(channel_names[i]), "%s",
                 app.playlist.channels[i].name);
    state = app.state;
    selected = app.selected;
    snprintf(status, sizeof(status), "%s", app.status);
    LightLock_Unlock(&app.lock);

    if (top_screen) {
        Draw_texture(&pixel, UI_INK, 0, 15, 400, 225);
        for (i = 19; i < 238; i += 8)
            Draw_texture(&pixel, UI_SHADOW, 0, (float)i, 400, 1);

        Draw_texture(&pixel, UI_ORANGE, 12, 25, 376, 4);
		Draw_c("+-------------- RETRO TUNER 3DS --------------+",
			   27, 39, 13.0f, UI_CREAM);
		Draw_align_c("NEW 3DS POCKET TELEVISION", 0, 59, 12.0f,
                     UI_ORANGE, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                     400, 18);

        Draw_texture(&pixel, UI_PANEL, 24, 88, 352, 70);
        Draw_c("NOW SELECTING", 38, 98, 10.0f, UI_MINT);
        if (count) {
            snprintf(line, sizeof(line), "CH %02lu  %.38s",
                     (unsigned long)(selected + 1), channel_names[selected]);
            Draw_align_c(line, 34, 117, 16.0f, UI_CREAM,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 332, 28);
        }

        if (state == LIVE_APP_LOADING) {
            Draw_c("TUNING SIGNAL >>>", 119, 169, 13.0f, UI_ORANGE);
            Draw_texture(&pixel, UI_SHADOW, 74, 190, 252, 10);
            Draw_texture(&pixel, UI_ORANGE, 76, 192, 160, 6);
        } else if (state == LIVE_APP_ERROR || state == LIVE_APP_NO_PLAYLIST) {
            Draw_align_c("NO SIGNAL // PRESS A TO RETRY", 0, 174, 12.0f,
                         DEF_DRAW_RED, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 20);
        } else {
            Draw_align_c("A  TUNE IN     D-PAD  CHANNEL     START  EXIT",
                         0, 180, 11.5f, UI_MINT, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 20);
        }
		Draw_align_c("PIXEL DECK 0.5.1-rc1 // H264", 0, 211, 9.5f,
                     UI_CREAM, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                     400, 14);
        return;
    }

    Draw_texture(&pixel, UI_INK, 0, 0, 320, 225);
    for (i = 4; i < 220; i += 8)
        Draw_texture(&pixel, UI_SHADOW, 0, (float)i, 320, 1);

    Draw_texture(&pixel, UI_ORANGE, 8, 8, 304, 3);
    snprintf(line, sizeof(line), "CHANNEL DECK // %lu STATIONS",
             (unsigned long)count);
    Draw_c(line, 14, 16, 13.0f, UI_CREAM);

    for (i = 0; i < count; i++) {
        float y = 39 + (float)i * 14;
        Draw_texture(&pixel, i == selected ? UI_ORANGE : UI_PANEL,
                     10, y, 300, 12);
        snprintf(line, sizeof(line), "%02lu  %.39s",
                 (unsigned long)(i + 1), channel_names[i]);
        Draw_c(line, 17, y + 1, 10.5f,
               i == selected ? UI_INK : UI_CREAM);
    }

    Draw_texture(&pixel, UI_PANEL, 8, 183, 304, 28);
    Draw_align_c(status, 14, 184, 9.5f,
                 state == LIVE_APP_ERROR || state == LIVE_APP_NO_PLAYLIST
                     ? DEF_DRAW_RED : UI_MINT,
                 DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 292, 24);
    Draw_align_c("A TUNE   UP/DOWN PICK   START QUIT", 0, 212, 9.0f,
                 UI_ORANGE, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                 320, 12);
}

void MiniIptv_live_app_init(void) {
    memset(&app, 0, sizeof(app));
    LightLock_Init(&app.lock);

	if (playlist_load_file(USER_PLAYLIST, &app.playlist) != 0) {
		set_status_locked(LIVE_APP_NO_PLAYLIST,
						  "ADD /3ds/retrotuner3ds/channels.m3u");
    } else {
        set_status_locked(LIVE_APP_IDLE, "READY // press A to tune this signal");
    }

    Vid_set_idle_hooks(live_hid, live_draw);
}

void MiniIptv_live_app_exit(void) {
    Vid_set_idle_hooks(NULL, NULL);
    miniiptv_live_stream_request_stop();
    if (app.worker) {
        threadJoin(app.worker, UINT64_MAX);
        threadFree(app.worker);
        app.worker = NULL;
    }
    miniiptv_live_stream_stop();
    if (app.network_ready) miniiptv_live_session_exit();
    app.network_ready = false;
}

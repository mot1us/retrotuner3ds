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
#define CHANNELS_PER_PAGE 10u

/* Retro broadcast palette (ABGR8888). */
#define UI_INK 0xFF21160Fu
#define UI_PANEL 0xFF3C2A1Du
#define UI_CREAM 0xFFB8EEFFu
#define UI_ORANGE 0xFF00A8FFu
#define UI_MINT 0xFF8FEA69u
#define UI_PINK 0xFF9A4FFFu
#define UI_CYAN 0xFFFFEB5Du
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
    int pending_channel_step;
    uint64_t tuning_started_ms;
} LiveApp;

static LiveApp app;

static void draw_key_hint(Draw_image_data *pixel, const char *key,
                          const char *action, float x, float y,
                          float key_width, uint32_t key_color) {
    Draw_texture(pixel, key_color, x, y, key_width, 13);
    Draw_align_c(key, x, y, 9.5f, UI_INK, DRAW_X_ALIGN_CENTER,
                 DRAW_Y_ALIGN_CENTER, key_width, 13);
    Draw_c(action, x + key_width + 5, y + 1, 9.5f, UI_CREAM);
}

static const char *stage_error_text(int result) {
    switch (result) {
        case MINIIPTV_STAGE_MANIFEST_FETCH_FAILED:
        case MINIIPTV_STAGE_MEDIA_FETCH_FAILED:
        case MINIIPTV_STAGE_SEGMENT_FETCH_FAILED:
            return "NETWORK ERROR // CHECK SIGNAL AND RETRY";
        case MINIIPTV_STAGE_MASTER_INVALID:
        case MINIIPTV_STAGE_MEDIA_INVALID:
            return "INVALID HLS PLAYLIST // TRY ANOTHER SIGNAL";
        case MINIIPTV_STAGE_UNSUPPORTED_HLS:
            return "UNSUPPORTED HLS FORMAT // TRY ANOTHER SIGNAL";
        case MINIIPTV_STAGE_NOT_MPEG_TS:
            return "UNSUPPORTED VIDEO CONTAINER // TRY ANOTHER SIGNAL";
        case MINIIPTV_STAGE_TOO_LARGE:
            return "SEGMENT TOO LARGE // TRY ANOTHER SIGNAL";
        case MINIIPTV_STAGE_DISCONTINUITY:
            return "SIGNAL CHANGED // PRESS A TO RETRY";
        default:
            return "TUNING FAILED // PRESS A TO RETRY";
    }
}

static void set_status_locked(LiveAppState state, const char *message) {
    app.state = state;
    snprintf(app.status, sizeof(app.status), "%s", message ? message : "");
}

static void player_error(uint32_t error_code) {
    LightLock_Lock(&app.lock);
    app.state = LIVE_APP_ERROR;
    snprintf(app.status, sizeof(app.status),
             "PLAYER ERROR 0x%08lX // PRESS A TO RETRY",
             (unsigned long)error_code);
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
}

static void player_channel_request(int direction) {
    LightLock_Lock(&app.lock);
    app.pending_channel_step = direction < 0 ? -1 : 1;
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
}

static bool begin_player_handoff(void) {
    bool started;

    LightLock_Lock(&app.lock);
    if (app.state != LIVE_APP_READY || app.awaiting_player_return) {
        LightLock_Unlock(&app.lock);
        return false;
    }
    snprintf(app.status, sizeof(app.status), "%s",
             "SIGNAL LOCKED // STARTING PLAYER...");
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
                 "SIGNAL FOUND // OPENING LIVE FEED...");
        auto_start = true;
    } else {
        app.state = LIVE_APP_ERROR;
        snprintf(app.status, sizeof(app.status), "%s (%d)",
                 stage_error_text(result), result);
    }
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
    if (auto_start) begin_player_handoff();
    threadExit(0);
}

static bool prepare_selected_tune_locked(void) {
    if ((app.state != LIVE_APP_IDLE && app.state != LIVE_APP_ERROR) ||
        app.playlist.count == 0 || app.worker)
        return false;
    app.pending_channel = app.playlist.channels[app.selected];
    app.tuning_started_ms = osGetTime();
    set_status_locked(LIVE_APP_LOADING,
                      "TUNING > LOCK > 1 SEGMENT > PLAY...");
    return true;
}

static void launch_tune_worker(void) {
    if (app.worker) return;
    app.worker = threadCreate(worker_main, NULL, 128 * 1024,
                              DEF_THREAD_PRIORITY_NORMAL, 1, false);
    if (!app.worker) {
        LightLock_Lock(&app.lock);
        set_status_locked(LIVE_APP_ERROR,
                          "Could not start the network worker.");
        LightLock_Unlock(&app.lock);
    }
    Draw_set_refresh_needed(true);
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

static bool update_player_return_locked(void) {
    uint32_t generation;

    if (!app.awaiting_player_return) return false;
    generation = Vid_query_playback_return_generation();
    if (generation == app.player_return_generation) return false;

    miniiptv_live_stream_stop();
    app.awaiting_player_return = false;
    if (app.pending_channel_step != 0 && app.playlist.count > 0 &&
        app.state != LIVE_APP_ERROR) {
        if (app.pending_channel_step < 0)
            app.selected = app.selected == 0 ? app.playlist.count - 1
                                             : app.selected - 1;
        else
            app.selected = (app.selected + 1) % app.playlist.count;
        app.pending_channel_step = 0;
        app.pending_channel = app.playlist.channels[app.selected];
        app.tuning_started_ms = osGetTime();
        set_status_locked(LIVE_APP_LOADING,
                          "SWITCHING > LOCK > 1 SEGMENT > PLAY...");
        return true;
    }
    app.pending_channel_step = 0;
    if (app.state != LIVE_APP_ERROR) {
        app.state = LIVE_APP_IDLE;
        snprintf(app.status, sizeof(app.status),
                 "OFF AIR // %.1f MiB linear free // A to retune",
                 Util_check_free_linear_space() / 1048576.0);
    }
    return false;
}

static bool live_hid(const Hid_info *key) {
    LiveAppState state;
    size_t count;
    bool launch_switch;

    if (!key) return false;
    reap_worker_if_finished();

    LightLock_Lock(&app.lock);
    launch_switch = update_player_return_locked();
    state = app.state;
    count = app.playlist.count;

    if (launch_switch) {
        LightLock_Unlock(&app.lock);
        launch_tune_worker();
        return true;
    }

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
            bool start = prepare_selected_tune_locked();
            LightLock_Unlock(&app.lock);
            if (start) launch_tune_worker();
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
    uint64_t tuning_started_ms;
    char status[160];
    char line[112];
    size_t i;
    size_t page_start;
    size_t page_end;
    bool launch_switch;

    (void)color;
    (void)back_color;

    reap_worker_if_finished();
    LightLock_Lock(&app.lock);
    launch_switch = update_player_return_locked();
    count = app.playlist.count;
    for (i = 0; i < count; i++)
        snprintf(channel_names[i], sizeof(channel_names[i]), "%s",
                 app.playlist.channels[i].name);
    state = app.state;
    selected = app.selected;
    tuning_started_ms = app.tuning_started_ms;
    snprintf(status, sizeof(status), "%s", app.status);
    LightLock_Unlock(&app.lock);
    if (launch_switch) launch_tune_worker();

    page_start = count ? (selected / CHANNELS_PER_PAGE) * CHANNELS_PER_PAGE : 0;
    page_end = page_start + CHANNELS_PER_PAGE;
    if (page_end > count) page_end = count;

    if (top_screen) {
        Draw_texture(&pixel, UI_INK, 0, 15, 400, 225);
        for (i = 19; i < 238; i += 8)
            Draw_texture(&pixel, UI_SHADOW, 0, (float)i, 400, 1);

        Draw_texture(&pixel, UI_ORANGE, 12, 25, 376, 3);
        Draw_texture(&pixel, UI_CYAN, 24, 43, 82, 2);
        Draw_texture(&pixel, UI_PINK, 294, 43, 82, 2);
		Draw_align_c("RETRO TUNER 3DS", 0, 31, 18.0f, UI_CREAM,
                     DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 24);
		Draw_align_c("POCKET BROADCAST SYSTEM // 199X", 0, 61, 11.0f,
                     UI_CYAN, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
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
            uint64_t elapsed = osGetTime() - tuning_started_ms;
            unsigned int phase = (unsigned int)((elapsed / 180u) % 12u);
            snprintf(line, sizeof(line), "TUNING SIGNAL // %lu.%lus",
                     (unsigned long)(elapsed / 1000u),
                     (unsigned long)((elapsed % 1000u) / 100u));
            Draw_align_c(line, 0, 169, 13.0f, UI_ORANGE,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 16);
            Draw_texture(&pixel, UI_SHADOW, 74, 190, 252, 10);
            for (i = 0; i < 12; i++)
                Draw_texture(&pixel, i == phase ? UI_CREAM : UI_ORANGE,
                             78 + (float)i * 20, 192, 14, 6);
            Draw_set_refresh_needed(true);
        } else if (state == LIVE_APP_ERROR || state == LIVE_APP_NO_PLAYLIST) {
            Draw_align_c("NO SIGNAL // PRESS A TO RETRY", 0, 174, 12.0f,
                         DEF_DRAW_RED, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 20);
        } else {
            draw_key_hint(&pixel, "A", "TUNE", 22, 181, 18, UI_PINK);
            draw_key_hint(&pixel, "D-PAD", "CHANNEL", 113, 181, 46,
                          UI_CYAN);
            draw_key_hint(&pixel, "START", "EXIT", 280, 181, 47,
                          UI_ORANGE);
        }
		Draw_align_c("PIXEL DECK 0.5.1-rc7 // H264", 0, 211, 9.5f,
                     UI_CREAM, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                     400, 14);
        return;
    }

    Draw_texture(&pixel, UI_INK, 0, 0, 320, 225);
    for (i = 4; i < 220; i += 8)
        Draw_texture(&pixel, UI_SHADOW, 0, (float)i, 320, 1);

    Draw_texture(&pixel, UI_ORANGE, 8, 8, 304, 3);
    snprintf(line, sizeof(line), "CHANNEL DECK // %lu STATIONS // P%lu/%lu",
             (unsigned long)count,
             (unsigned long)(count ? page_start / CHANNELS_PER_PAGE + 1 : 0),
             (unsigned long)(count ? (count + CHANNELS_PER_PAGE - 1) /
                                      CHANNELS_PER_PAGE : 0));
    Draw_c(line, 14, 16, 13.0f, UI_CREAM);

    for (i = page_start; i < page_end; i++) {
        float y = 39 + (float)(i - page_start) * 14;
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
    draw_key_hint(&pixel, "A", "TUNE", 9, 211, 17, UI_PINK);
    draw_key_hint(&pixel, "UP/DN", "PICK", 77, 211, 42, UI_CYAN);
    draw_key_hint(&pixel, "START", "QUIT", 209, 211, 45, UI_ORANGE);
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
    Vid_set_live_error_hook(player_error);
    Vid_set_live_channel_hook(player_channel_request);
}

void MiniIptv_live_app_exit(void) {
    Vid_set_live_error_hook(NULL);
    Vid_set_live_channel_hook(NULL);
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

#include "miniiptv/live_app.h"

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "miniiptv/live_session.h"
#include "miniiptv/live_stream.h"
#include "miniiptv/playlist.h"
#include "miniiptv/telemetry_log.h"
#include "miniiptv/version.h"
#include "system/draw/draw.h"
#include "system/util/err_types.h"
#include "system/util/hid_types.h"
#include "system/util/thread_types.h"
#include "system/util/util.h"
#include "video_player.h"

#define USER_PLAYLIST "sdmc:/3ds/retrotuner3ds/channels.m3u"
#define TELEMETRY_LOG "sdmc:/3ds/retrotuner3ds/telemetry.csv"
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

typedef enum {
    PLAYER_RETURN_NONE = 0,
    PLAYER_RETURN_STOP,
    PLAYER_RETURN_RETUNE
} PlayerReturnAction;

typedef struct {
    LightLock lock;
    MiniIptvPlaylist playlist;
    MiniIptvChannel pending_channel;
    MiniIptvStageInfo stage_info;
    size_t selected;
    Thread worker;
    bool worker_finished;
    bool worker_reaping;
    bool launch_after_reap;
    bool cancel_to_deck;
    bool queued_tune;
    bool stream_cleanup_pending;
    LiveAppState state;
    char status[160];
    bool network_ready;
    bool awaiting_player_return;
    uint32_t player_return_generation;
    int pending_channel_step;
    uint64_t tuning_started_ms;
    bool switching_from_player;
    size_t switch_from_index;
    size_t switch_to_index;
    uint32_t player_error_code;
    Vid_live_diagnostics player_error_diagnostics;
    bool has_player_error_diagnostics;
    bool exit_requested;
} LiveApp;

static LiveApp app;

static void launch_tune_worker(void);

static const char *live_app_state_label(LiveAppState state) {
    switch (state) {
        case LIVE_APP_NO_PLAYLIST: return "NO_PLAYLIST";
        case LIVE_APP_IDLE: return "DECK";
        case LIVE_APP_LOADING: return "TUNING";
        case LIVE_APP_READY: return "READY";
        case LIVE_APP_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static void draw_key_hint(Draw_image_data *pixel, const char *key,
                          const char *action, float x, float y,
                          float key_width, uint32_t key_color) {
    Draw_texture(pixel, key_color, x, y, key_width, 13);
    Draw_align_c(key, x, y, 9.5f, UI_INK, DRAW_X_ALIGN_CENTER,
                 DRAW_Y_ALIGN_CENTER, key_width, 13);
    Draw_c(action, x + key_width + 5, y + 1, 9.5f, UI_CREAM);
}

static void format_tune_status(char *line, size_t line_size,
                               const MiniIptvTuneTelemetry *tune) {
    if (!line || line_size == 0 || !tune) return;
    if (tune->phase == MINIIPTV_TUNE_PHASE_INITIAL_SEGMENT)
        snprintf(line, line_size, "%s // %lu KiB // T+%u.%us",
                 miniiptv_live_tune_phase_label(tune->phase),
                 (unsigned long)(tune->initial_segment_received_bytes / 1024u),
                 tune->total_elapsed_milliseconds / 1000u,
                 (tune->total_elapsed_milliseconds % 1000u) / 100u);
    else
        snprintf(line, line_size, "%s // %u.%us // T+%u.%us",
                 miniiptv_live_tune_phase_label(tune->phase),
                 tune->phase_elapsed_milliseconds / 1000u,
                 (tune->phase_elapsed_milliseconds % 1000u) / 100u,
                 tune->total_elapsed_milliseconds / 1000u,
                 (tune->total_elapsed_milliseconds % 1000u) / 100u);
}

static const char *stage_error_text(int result) {
    switch (result) {
        case MINIIPTV_STAGE_CANCELLED:
            return "TUNING CANCELED // PRESS A TO RETRY";
        case MINIIPTV_STAGE_TUNE_TIMEOUT:
            return "TUNING TIMEOUT // TRY AGAIN OR PICK ANOTHER SIGNAL";
        case MINIIPTV_STAGE_PLAYER_OPEN_TIMEOUT:
            return "PLAYER SETUP TIMEOUT // TRY ANOTHER SIGNAL";
        case MINIIPTV_STAGE_MVD_INIT_TIMEOUT:
            return "MVD INIT TIMEOUT // TRY ANOTHER SIGNAL";
        case MINIIPTV_STAGE_FIRST_FRAME_TIMEOUT:
            return "FIRST FRAME TIMEOUT // TRY ANOTHER SIGNAL";
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
    app.player_error_code = 0;
    memset(&app.player_error_diagnostics, 0,
           sizeof(app.player_error_diagnostics));
    app.has_player_error_diagnostics = false;
    snprintf(app.status, sizeof(app.status), "%s", message ? message : "");
}

static const char *audio_diagnostic_state(Vid_live_audio_state state) {
    switch (state) {
        case VID_LIVE_AUDIO_NONE: return "NONE";
        case VID_LIVE_AUDIO_DEMUXED: return "DEMUX";
        case VID_LIVE_AUDIO_READY: return "OK";
        case VID_LIVE_AUDIO_INIT_FAILED: return "INIT!";
        case VID_LIVE_AUDIO_DECODE_FAILED: return "DEC!";
        case VID_LIVE_AUDIO_CONVERT_FAILED: return "CVT!";
        case VID_LIVE_AUDIO_OUTPUT_FAILED: return "OUT!";
        case VID_LIVE_AUDIO_SCANNING:
        default: return "SCAN";
    }
}

static int tune_should_cancel(void *unused) {
    bool cancel;
    (void)unused;
    LightLock_Lock(&app.lock);
    cancel = app.exit_requested || app.cancel_to_deck || app.queued_tune;
    LightLock_Unlock(&app.lock);
    return cancel;
}

static void player_error(uint32_t error_code) {
    Vid_live_diagnostics diagnostics = {0};
    Vid_query_live_diagnostics(&diagnostics);
    miniiptv_live_tune_fail((int32_t)error_code);
    LightLock_Lock(&app.lock);
    /* A player failure can race an already-latched L/R request. Keep that
     * request so teardown returns into the requested channel, not the failed
     * one. With no request, this remains an ordinary NO SIGNAL state. */
    if (app.pending_channel_step == 0)
        app.switching_from_player = false;
    app.state = LIVE_APP_ERROR;
    app.player_error_code = error_code;
    app.player_error_diagnostics = diagnostics;
    app.has_player_error_diagnostics = true;
    if ((int32_t)error_code == MINIIPTV_STAGE_PLAYER_OPEN_TIMEOUT)
        snprintf(app.status, sizeof(app.status),
                 "PLAYER SETUP TIMEOUT // TRY ANOTHER SIGNAL");
    else if ((int32_t)error_code == MINIIPTV_STAGE_MVD_INIT_TIMEOUT)
        snprintf(app.status, sizeof(app.status),
                 "MVD INIT TIMEOUT // TRY ANOTHER SIGNAL");
    else if ((int32_t)error_code == MINIIPTV_STAGE_FIRST_FRAME_TIMEOUT)
        snprintf(app.status, sizeof(app.status),
                 "1ST FRAME TIMEOUT // TRY ANOTHER SIGNAL");
    else if ((int32_t)error_code == MINIIPTV_STAGE_TOO_LARGE)
        snprintf(app.status, sizeof(app.status),
                 "SIGNAL REJECTED // MAX 640x480 AT 30FPS");
    else if ((int32_t)error_code == MINIIPTV_STAGE_UNSUPPORTED_HLS)
        snprintf(app.status, sizeof(app.status),
                 "SIGNAL REJECTED // H264 YUV420 REQUIRED");
    else if (error_code == DEF_ERR_UNSAFE_VIDEO_STREAM)
        snprintf(app.status, sizeof(app.status),
                 "SIGNAL FORMAT CHANGED // STOPPED FOR SAFETY");
    else
        snprintf(app.status, sizeof(app.status),
                 "PLAYER ERROR 0x%08lX // PRESS A TO RETRY",
                 (unsigned long)error_code);
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
}

static void player_channel_request(int direction) {
    LightLock_Lock(&app.lock);
    if (direction == 0) {
        /* B wins over an earlier bumper press and returns to the deck after the
         * decoder has completed its serialized teardown. */
        app.cancel_to_deck = true;
        app.queued_tune = false;
        app.pending_channel_step = 0;
        app.switching_from_player = false;
        LightLock_Unlock(&app.lock);
        Draw_set_refresh_needed(true);
        return;
    }
    if (app.switching_from_player || app.pending_channel_step != 0) {
        LightLock_Unlock(&app.lock);
        return;
    }
    app.cancel_to_deck = false;
    app.pending_channel_step = direction < 0 ? -1 : 1;
    app.switching_from_player = app.playlist.count > 0;
    app.switch_from_index = app.selected;
    if (app.playlist.count > 0) {
        if (direction < 0)
            app.switch_to_index = app.selected == 0 ? app.playlist.count - 1
                                                    : app.selected - 1;
        else
            app.switch_to_index = (app.selected + 1) % app.playlist.count;
    }
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
}

static bool begin_player_handoff(void) {
    bool started;

    LightLock_Lock(&app.lock);
    if (app.exit_requested || app.state != LIVE_APP_READY ||
        app.awaiting_player_return) {
        LightLock_Unlock(&app.lock);
        return false;
    }
    snprintf(app.status, sizeof(app.status), "%s",
             "SIGNAL LOCKED // STARTING PLAYER...");
    app.player_return_generation = Vid_query_playback_return_generation();
    app.awaiting_player_return = true;
    LightLock_Unlock(&app.lock);

    miniiptv_live_tune_phase_begin(MINIIPTV_TUNE_PHASE_PLAYER_OPEN);
    started = Vid_prepare_and_start_file("", MINIIPTV_LIVE_STREAM_URL);
    if (!started) {
        miniiptv_live_tune_fail(MINIIPTV_STAGE_FILE_FAILED);
        miniiptv_live_stream_stop();
        LightLock_Lock(&app.lock);
        app.awaiting_player_return = false;
        app.pending_channel_step = 0;
        app.switching_from_player = false;
        set_status_locked(LIVE_APP_ERROR,
                          "Player handoff failed. Press A to retry.");
        LightLock_Unlock(&app.lock);
    } else {
        LightLock_Lock(&app.lock);
        app.switching_from_player = false;
        LightLock_Unlock(&app.lock);
    }
    Draw_set_refresh_needed(true);
    return started;
}

static void worker_main(void *unused) {
    MiniIptvStageInfo info = {0};
    MiniIptvChannel pending_channel;
    int result;
    bool auto_start = false;
    bool exiting = false;
    bool cancel_to_deck = false;
    bool queued_tune = false;
    bool stop_stream = false;
    bool network_ready;
    (void)unused;

    LightLock_Lock(&app.lock);
    network_ready = app.network_ready;
    pending_channel = app.pending_channel;
    exiting = app.exit_requested;
    cancel_to_deck = app.cancel_to_deck;
    queued_tune = app.queued_tune;
    LightLock_Unlock(&app.lock);
    if (exiting || cancel_to_deck || queued_tune) {
        result = MINIIPTV_STAGE_CANCELLED;
    } else if (!network_ready) {
        result = miniiptv_live_session_init();
        if (result == 0) {
            LightLock_Lock(&app.lock);
            app.network_ready = true;
            LightLock_Unlock(&app.lock);
        }
    } else {
        result = 0;
    }
    if (result == 0) {
        /* Network/session setup can race a cancel press. Check the app-owned
         * intent again before resetting the stream for this tune. */
        LightLock_Lock(&app.lock);
        exiting = app.exit_requested;
        cancel_to_deck = app.cancel_to_deck;
        queued_tune = app.queued_tune;
        LightLock_Unlock(&app.lock);
        if (exiting || cancel_to_deck || queued_tune)
            result = MINIIPTV_STAGE_CANCELLED;
        else
            result = miniiptv_live_stream_start(&pending_channel, &info,
                                                tune_should_cancel, NULL);
    }
    /* Session setup can fail before live_stream_start() has a chance to own
     * the tune timeline. Preserve that failure in the same diagnostics path. */
    if (result != MINIIPTV_STAGE_OK)
        miniiptv_live_tune_fail(result);
    LightLock_Lock(&app.lock);
    exiting = app.exit_requested;
    cancel_to_deck = app.cancel_to_deck;
    queued_tune = app.queued_tune;
    if (exiting) {
        app.pending_channel_step = 0;
        app.switching_from_player = false;
        stop_stream = result == MINIIPTV_STAGE_OK;
    } else if (cancel_to_deck) {
        app.pending_channel_step = 0;
        app.switching_from_player = false;
        set_status_locked(LIVE_APP_LOADING,
                          "TUNING CANCELED // FINISHING CLEAN STOP...");
        stop_stream = result == MINIIPTV_STAGE_OK;
    } else if (queued_tune) {
        set_status_locked(LIVE_APP_LOADING,
                          "NEXT SIGNAL QUEUED // FINISHING CLEAN STOP...");
        stop_stream = result == MINIIPTV_STAGE_OK;
    } else if (result == MINIIPTV_STAGE_OK) {
        app.stage_info = info;
        app.state = LIVE_APP_READY;
        snprintf(app.status, sizeof(app.status),
                 "SIGNAL FOUND // OPENING LIVE FEED...");
        auto_start = true;
    } else {
        app.state = LIVE_APP_ERROR;
        app.pending_channel_step = 0;
        app.switching_from_player = false;
        if (result == MINIIPTV_STAGE_CANCELLED) {
            snprintf(app.status, sizeof(app.status), "%s",
                     "TUNING CANCELED // PRESS A TO RETRY");
        } else if (result == MINIIPTV_STAGE_TUNE_TIMEOUT) {
            snprintf(app.status, sizeof(app.status), "%s",
                     "TUNING TIMEOUT // PICK ANOTHER SIGNAL");
        } else if (result == MINIIPTV_STAGE_TOO_LARGE &&
                   info.segment_limit_bytes > 0) {
            size_t observed = info.reported_segment_bytes
                ? info.reported_segment_bytes : info.attempted_segment_bytes;
            snprintf(app.status, sizeof(app.status),
                     "SEGMENT TOO LARGE // %s %lu / %lu KiB",
                     info.reported_segment_bytes ? "LEN" : "RX",
                     (unsigned long)(observed / 1024u),
                     (unsigned long)(info.segment_limit_bytes / 1024u));
        } else {
            snprintf(app.status, sizeof(app.status), "%s (%d)",
                     stage_error_text(result), result);
        }
    }
    LightLock_Unlock(&app.lock);
    if (stop_stream) miniiptv_live_stream_stop();
    Draw_set_refresh_needed(true);
    if (auto_start) begin_player_handoff();
    else if (exiting) miniiptv_live_stream_request_stop();
    LightLock_Lock(&app.lock);
    /* A second key press can arrive while a successful-but-canceled stream is
     * being joined above. Resolve the latest intent, not the earlier snapshot,
     * before publishing the worker as finished. */
    if (!app.exit_requested && app.cancel_to_deck) {
        app.cancel_to_deck = false;
        app.queued_tune = false;
        app.pending_channel_step = 0;
        app.switching_from_player = false;
        set_status_locked(LIVE_APP_IDLE,
                          "TUNING CANCELED // READY FOR ANOTHER SIGNAL");
    } else if (!app.exit_requested && app.queued_tune) {
        set_status_locked(LIVE_APP_LOADING,
                          "NEXT SIGNAL QUEUED // FINISHING CLEAN STOP...");
    }
    app.worker_finished = true;
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
    threadExit(0);
}

static bool prepare_selected_tune_locked(void) {
    if ((app.state != LIVE_APP_IDLE && app.state != LIVE_APP_ERROR) ||
        app.playlist.count == 0 || app.worker || app.stream_cleanup_pending ||
        app.exit_requested)
        return false;
    app.pending_channel = app.playlist.channels[app.selected];
    app.cancel_to_deck = false;
    app.queued_tune = false;
    app.tuning_started_ms = osGetTime();
    set_status_locked(LIVE_APP_LOADING,
                      "TUNING > LOCK > 1 SEGMENT > PLAY...");
    return true;
}

static void launch_tune_worker(void) {
    Thread worker;

    LightLock_Lock(&app.lock);
    if (app.exit_requested) {
        LightLock_Unlock(&app.lock);
        return;
    }
    if (app.worker_reaping) {
        /* The old handle has already been detached and is known finished.
         * Preserve this launch request for its single owning reaper instead of
         * racing a new worker against threadFree and old queued intent. */
        app.launch_after_reap = true;
        LightLock_Unlock(&app.lock);
        Draw_set_refresh_needed(true);
        return;
    }
    if (app.worker) {
        LightLock_Unlock(&app.lock);
        return;
    }
    /* Publish the handle while holding the lock. START/exit cannot pass this
     * point, observe NULL, and tear down curl/stream state before the newly
     * scheduled worker becomes visible. */
    miniiptv_live_tune_reset();
    worker = threadCreate(worker_main, NULL, 128 * 1024,
                          DEF_THREAD_PRIORITY_NORMAL, 1, false);
    app.worker = worker;
    app.worker_finished = false;
    if (!worker) {
        miniiptv_live_tune_fail(MINIIPTV_STAGE_FILE_FAILED);
        app.pending_channel_step = 0;
        app.switching_from_player = false;
        set_status_locked(LIVE_APP_ERROR,
                          "Could not start the network worker.");
    }
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
}

static void reap_worker_if_finished(void) {
    Thread finished_worker = NULL;
    bool launch_queued = false;

    LightLock_Lock(&app.lock);
    if (app.worker && app.worker_finished && !app.worker_reaping) {
        finished_worker = app.worker;
        app.worker = NULL;
        app.worker_finished = false;
        app.worker_reaping = true;
    }
    LightLock_Unlock(&app.lock);
    if (finished_worker) {
        threadJoin(finished_worker, UINT64_MAX);
        threadFree(finished_worker);

        LightLock_Lock(&app.lock);
        /* STOP cleanup is the sole stream owner while this flag is set. Leave
         * queued_tune intact so process_player_return_action() launches it only
         * after miniiptv_live_stream_stop() has completed. */
        if (!app.exit_requested && !app.stream_cleanup_pending &&
            app.queued_tune && !app.cancel_to_deck &&
            app.playlist.count > 0) {
            app.queued_tune = false;
            app.pending_channel = app.playlist.channels[app.selected];
            app.tuning_started_ms = osGetTime();
            set_status_locked(LIVE_APP_LOADING,
                              "SWITCHING > LOCK > 1 SEGMENT > PLAY...");
            launch_queued = true;
        } else if (!app.exit_requested && app.cancel_to_deck) {
            app.cancel_to_deck = false;
            app.queued_tune = false;
            app.pending_channel_step = 0;
            app.switching_from_player = false;
            set_status_locked(LIVE_APP_IDLE,
                              "TUNING CANCELED // READY FOR ANOTHER SIGNAL");
        } else if (!app.exit_requested && !app.stream_cleanup_pending &&
                   app.launch_after_reap) {
            launch_queued = true;
        }
        app.launch_after_reap = false;
        app.worker_reaping = false;
        LightLock_Unlock(&app.lock);
    }
    if (launch_queued) launch_tune_worker();
}

static PlayerReturnAction update_player_return_locked(void) {
    uint32_t generation;
    bool deck_requested;

    if (!app.awaiting_player_return) return PLAYER_RETURN_NONE;
    generation = Vid_query_playback_return_generation();
    if (generation == app.player_return_generation) return PLAYER_RETURN_NONE;

    app.awaiting_player_return = false;
    deck_requested = app.cancel_to_deck;
    if (!deck_requested && app.pending_channel_step != 0 &&
        app.playlist.count > 0) {
        if (app.pending_channel_step < 0)
            app.selected = app.selected == 0 ? app.playlist.count - 1
                                             : app.selected - 1;
        else
            app.selected = (app.selected + 1) % app.playlist.count;
        app.pending_channel_step = 0;
        app.pending_channel = app.playlist.channels[app.selected];
        app.cancel_to_deck = false;
        app.queued_tune = false;
        app.tuning_started_ms = osGetTime();
        set_status_locked(LIVE_APP_LOADING,
                          "SWITCHING > LOCK > 1 SEGMENT > PLAY...");
        /* If the playback-return event wins the race with reaping the worker
         * that opened this player, remember the tune now. The reaper will own
         * exactly one launch after freeing that old handle. */
        if (app.worker || app.worker_reaping)
            app.launch_after_reap = true;
        return PLAYER_RETURN_RETUNE;
    }
    app.pending_channel_step = 0;
    app.switching_from_player = false;
    app.cancel_to_deck = false;
    app.stream_cleanup_pending = true;
    if (app.state != LIVE_APP_ERROR) {
        app.state = LIVE_APP_IDLE;
        snprintf(app.status, sizeof(app.status),
                 "OFF AIR // %.1f MiB linear free // A to retune",
                 Util_check_free_linear_space() / 1048576.0);
    } else if (deck_requested) {
        set_status_locked(LIVE_APP_IDLE,
                          "READY // press A to tune this signal");
    }
    return PLAYER_RETURN_STOP;
}

static void process_player_return_action(PlayerReturnAction action) {
    bool launch_queued = false;

    if (action == PLAYER_RETURN_NONE) return;
    miniiptv_live_stream_request_stop();
    if (action == PLAYER_RETURN_RETUNE) {
        /* The tuning worker owns the full producer join. Keeping that wait off
         * the HID/draw path makes a slow manifest unable to freeze controls. */
        launch_tune_worker();
    } else {
        /* No new worker will consume the old stream when returning to the
         * deck, so finish its cleanup here, after releasing app.lock. */
        miniiptv_live_stream_stop();
        LightLock_Lock(&app.lock);
        app.stream_cleanup_pending = false;
        /* HID and draw run independently. If L/R arrived while the old stream
         * was being joined, consume that queued choice as soon as cleanup has
         * released the single stream/decoder owner. */
        if (!app.exit_requested && app.queued_tune && !app.cancel_to_deck &&
            !app.worker && !app.worker_reaping && app.playlist.count > 0) {
            app.queued_tune = false;
            app.pending_channel = app.playlist.channels[app.selected];
            app.tuning_started_ms = osGetTime();
            set_status_locked(LIVE_APP_LOADING,
                              "SWITCHING > LOCK > 1 SEGMENT > PLAY...");
            launch_queued = true;
        }
        LightLock_Unlock(&app.lock);
        Draw_set_refresh_needed(true);
        if (launch_queued) launch_tune_worker();
    }
}

static bool live_hid(const Hid_info *key) {
    LiveAppState state;
    size_t count;
    PlayerReturnAction return_action;
    int loading_step = 0;
    bool cancel_loading = false;
    bool launch_error_tune = false;
    bool stop_error_stream = false;
    bool error_return_input_latched = false;

    if (!key) return false;

    /* A successful tune can leave its already-finished worker handle parked
     * while the player owns both screens. Reap it before a bumper return tries
     * to launch the next tune, or app.worker would falsely make that launch a
     * no-op. worker_finished guarantees this join cannot wait on network I/O. */
    reap_worker_if_finished();
    LightLock_Lock(&app.lock);
    /* The HID thread may already have loaded this hook when START unhooks it.
     * Once exit owns the app, never let that in-flight callback touch the
     * stream or schedule another tune. */
    if (app.exit_requested) {
        LightLock_Unlock(&app.lock);
        return false;
    }
    /* If the player reached IDLE in the same scan as the user's error-screen
     * input, latch that input before consuming the return generation. */
    if (app.awaiting_player_return && app.state == LIVE_APP_ERROR) {
        if (DEF_HID_PHY_PR(key->b)) {
            app.cancel_to_deck = true;
            app.queued_tune = false;
            app.pending_channel_step = 0;
            app.switching_from_player = false;
            error_return_input_latched = true;
        } else if (app.playlist.count > 0 && DEF_HID_PHY_PR(key->l)) {
            app.cancel_to_deck = false;
            app.pending_channel_step = -1;
            app.switching_from_player = true;
            app.switch_from_index = app.selected;
            app.switch_to_index = app.selected == 0
                ? app.playlist.count - 1 : app.selected - 1;
            error_return_input_latched = true;
        } else if (app.playlist.count > 0 && DEF_HID_PHY_PR(key->r)) {
            app.cancel_to_deck = false;
            app.pending_channel_step = 1;
            app.switching_from_player = true;
            app.switch_from_index = app.selected;
            app.switch_to_index = (app.selected + 1) % app.playlist.count;
            error_return_input_latched = true;
        }
    }
    return_action = update_player_return_locked();
    state = app.state;
    count = app.playlist.count;

    if (return_action != PLAYER_RETURN_NONE) {
        LightLock_Unlock(&app.lock);
        process_player_return_action(return_action);
        return true;
    }
    if (error_return_input_latched) {
        /* The decode thread publishes IDLE immediately before incrementing the
         * return generation. Keep this key's pending B/L/R intent intact until
         * the next scan observes that increment; falling through would treat
         * it as a fresh idle-error action and race a new tune against cleanup. */
        LightLock_Unlock(&app.lock);
        Draw_set_refresh_needed(true);
        return true;
    }

    if (state == LIVE_APP_LOADING) {
        if (DEF_HID_PHY_PR(key->b)) {
            app.cancel_to_deck = true;
            app.queued_tune = false;
            app.pending_channel_step = 0;
            app.switching_from_player = false;
            snprintf(app.status, sizeof(app.status), "%s",
                     "CANCELING TUNE // RETURNING TO CHANNEL DECK...");
            cancel_loading = true;
        } else if (count && (DEF_HID_PHY_PR(key->d_up) ||
                            DEF_HID_PHY_PR(key->l))) {
            loading_step = -1;
        } else if (count && (DEF_HID_PHY_PR(key->d_down) ||
                            DEF_HID_PHY_PR(key->r))) {
            loading_step = 1;
        }
        if (loading_step != 0) {
            size_t previous = app.selected;
            app.selected = loading_step < 0
                ? (app.selected == 0 ? count - 1 : app.selected - 1)
                : (app.selected + 1) % count;
            if (!app.switching_from_player) app.switch_from_index = previous;
            app.switching_from_player = true;
            app.switch_to_index = app.selected;
            app.cancel_to_deck = false;
            app.queued_tune = true;
            snprintf(app.status, sizeof(app.status),
                     "CANCELING CURRENT TUNE // QUEUED CH %02lu",
                     (unsigned long)(app.selected + 1));
            cancel_loading = true;
        }
        if (cancel_loading) {
            LightLock_Unlock(&app.lock);
            miniiptv_live_stream_request_stop();
            Draw_set_refresh_needed(true);
            return true;
        }
    }

    if (state == LIVE_APP_ERROR && DEF_HID_PHY_PR(key->b)) {
        app.cancel_to_deck = app.worker || app.worker_reaping;
        app.queued_tune = false;
        app.pending_channel_step = 0;
        app.switching_from_player = false;
        set_status_locked(LIVE_APP_IDLE,
                          "READY // press A to tune this signal");
        LightLock_Unlock(&app.lock);
        miniiptv_live_stream_request_stop();
        Draw_set_refresh_needed(true);
        return true;
    }

    if (state == LIVE_APP_ERROR && count &&
        (DEF_HID_PHY_PR(key->l) || DEF_HID_PHY_PR(key->r))) {
        int direction = DEF_HID_PHY_PR(key->l) ? -1 : 1;
        size_t previous = app.selected;

        app.selected = direction < 0
            ? (app.selected == 0 ? count - 1 : app.selected - 1)
            : (app.selected + 1) % count;
        app.pending_channel_step = 0;
        app.cancel_to_deck = false;
        app.switching_from_player = true;
        app.switch_from_index = previous;
        app.switch_to_index = app.selected;
        if (app.worker || app.worker_reaping || app.stream_cleanup_pending) {
            app.queued_tune = true;
            set_status_locked(LIVE_APP_LOADING,
                              "NEXT SIGNAL QUEUED // FINISHING CLEAN STOP...");
            stop_error_stream = true;
        } else {
            app.queued_tune = false;
            app.pending_channel = app.playlist.channels[app.selected];
            app.tuning_started_ms = osGetTime();
            set_status_locked(LIVE_APP_LOADING,
                              "SWITCHING > LOCK > 1 SEGMENT > PLAY...");
            launch_error_tune = true;
        }
        LightLock_Unlock(&app.lock);
        if (stop_error_stream) miniiptv_live_stream_request_stop();
        if (launch_error_tune) launch_tune_worker();
        Draw_set_refresh_needed(true);
        return true;
    }

    if (!app.stream_cleanup_pending && !app.worker && !app.worker_reaping &&
        state != LIVE_APP_LOADING && count &&
        DEF_HID_PHY_PR(key->d_up)) {
        app.selected = app.selected == 0 ? count - 1 : app.selected - 1;
        set_status_locked(LIVE_APP_IDLE, "READY // press A to tune this signal");
        LightLock_Unlock(&app.lock);
        miniiptv_live_stream_stop();
        Draw_set_refresh_needed(true);
        return true;
    }
    if (!app.stream_cleanup_pending && !app.worker && !app.worker_reaping &&
        state != LIVE_APP_LOADING && count &&
        DEF_HID_PHY_PR(key->d_down)) {
        app.selected = (app.selected + 1) % count;
        set_status_locked(LIVE_APP_IDLE, "READY // press A to tune this signal");
        LightLock_Unlock(&app.lock);
        miniiptv_live_stream_stop();
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
    MiniIptvTuneTelemetry tune = {0};
    char channel_names[MINIIPTV_MAX_CHANNELS][MINIIPTV_NAME_MAX];
    LiveAppState state;
    size_t count;
    size_t selected;
    uint64_t tuning_started_ms;
    bool switching_from_player;
    size_t switch_from_index;
    size_t switch_to_index;
    uint32_t player_error_code;
    Vid_live_diagnostics player_error_diagnostics;
    bool has_player_error_diagnostics;
    char status[160];
    char line[112];
    size_t i;
    size_t page_start;
    size_t page_end;
    PlayerReturnAction return_action;

    (void)color;
    (void)back_color;

    reap_worker_if_finished();
    LightLock_Lock(&app.lock);
    return_action = update_player_return_locked();
    count = app.playlist.count;
    for (i = 0; i < count; i++)
        snprintf(channel_names[i], sizeof(channel_names[i]), "%s",
                 app.playlist.channels[i].name);
    state = app.state;
    selected = app.selected;
    tuning_started_ms = app.tuning_started_ms;
    switching_from_player = app.switching_from_player;
    switch_from_index = app.switch_from_index;
    switch_to_index = app.switch_to_index;
    player_error_code = app.player_error_code;
    player_error_diagnostics = app.player_error_diagnostics;
    has_player_error_diagnostics = app.has_player_error_diagnostics;
    snprintf(status, sizeof(status), "%s", app.status);
    LightLock_Unlock(&app.lock);
    miniiptv_live_tune_get_telemetry(&tune);
    if (top_screen) {
        MiniIptvTelemetrySample log_sample = {0};
        log_sample.channel_name = count ? channel_names[selected] : "";
        log_sample.app_state = live_app_state_label(state);
        log_sample.tune_phase =
            miniiptv_live_tune_phase_label(tune.phase);
        log_sample.shadow_state = "";
        log_sample.producer_state = "";
        log_sample.last_error = player_error_code
            ? (int64_t)(uint64_t)player_error_code
            : (int64_t)tune.result;
        log_sample.periodic = state == LIVE_APP_LOADING;
        miniiptv_telemetry_log_record(osGetTime(), &log_sample);
    }
    process_player_return_action(return_action);

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
        Draw_c(switching_from_player ? "LIVE CHANNEL HANDOFF" : "NOW SELECTING",
               38, 98, 10.0f, UI_MINT);
        if (count) {
            snprintf(line, sizeof(line), "CH %02lu  %.38s",
                     (unsigned long)(selected + 1), channel_names[selected]);
            Draw_align_c(line, 34, 117, 16.0f, UI_CREAM,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 332, 28);
        }

        if (state == LIVE_APP_LOADING) {
            uint64_t elapsed = osGetTime() - tuning_started_ms;
            unsigned int phase = (unsigned int)((elapsed / 180u) % 12u);
            format_tune_status(line, sizeof(line), &tune);
            Draw_align_c(line, 0, 169, 11.0f, UI_ORANGE,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 16);
            Draw_texture(&pixel, UI_SHADOW, 74, 190, 252, 10);
            for (i = 0; i < 12; i++)
                Draw_texture(&pixel, i == phase ? UI_CREAM : UI_ORANGE,
                             78 + (float)i * 20, 192, 14, 6);
            Draw_align_c("B CANCEL // L/R CHANGE", 0, 201, 9.0f, UI_MINT,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 10);
            Draw_set_refresh_needed(true);
        } else if (state == LIVE_APP_ERROR) {
            Draw_align_c("NO SIGNAL // A RETRY // B DECK // L/R CHANGE",
                         0, 174, 12.0f,
                         DEF_DRAW_RED, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 20);
            if (tune.phase == MINIIPTV_TUNE_PHASE_FAILED) {
                if (has_player_error_diagnostics)
                    snprintf(line, sizeof(line),
                             "FAILED @ %s // T+%u.%us // ERR:%08lX",
                             miniiptv_live_tune_phase_label(tune.failure_phase),
                             tune.total_elapsed_milliseconds / 1000u,
                             (tune.total_elapsed_milliseconds % 1000u) / 100u,
                             (unsigned long)player_error_code);
                else
                    snprintf(line, sizeof(line), "FAILED @ %s // T+%u.%us",
                             miniiptv_live_tune_phase_label(tune.failure_phase),
                             tune.total_elapsed_milliseconds / 1000u,
                             (tune.total_elapsed_milliseconds % 1000u) / 100u);
                Draw_align_c(line, 0, 195, 8.5f, UI_ORANGE,
                             DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                             400, 12);
            }
        } else if (state == LIVE_APP_NO_PLAYLIST) {
            Draw_align_c("NO PLAYLIST // ADD CHANNELS.M3U", 0, 174, 12.0f,
                         DEF_DRAW_RED, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 20);
        } else {
            draw_key_hint(&pixel, "A", "TUNE", 22, 181, 18, UI_PINK);
            draw_key_hint(&pixel, "D-PAD", "CHANNEL", 113, 181, 46,
                          UI_CYAN);
            draw_key_hint(&pixel, "START", "EXIT", 280, 181, 47,
                          UI_ORANGE);
        }
		Draw_align_c("PIXEL DECK " RETROTUNER_VERSION " // H264", 0, 211, 9.5f,
                     UI_CREAM, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                     400, 14);
        return;
    }

    Draw_texture(&pixel, UI_INK, 0, 0, 320, 225);
    for (i = 4; i < 220; i += 8)
        Draw_texture(&pixel, UI_SHADOW, 0, (float)i, 320, 1);

    Draw_texture(&pixel, UI_ORANGE, 8, 8, 304, 3);

    if (state == LIVE_APP_LOADING && switching_from_player && count > 0) {
        uint64_t elapsed = osGetTime() - tuning_started_ms;
        unsigned int phase = (unsigned int)((elapsed / 160u) % 10u);
        Draw_c("[ RETRO TUNER // CHANNEL HANDOFF ]", 14, 16, 13.0f,
               UI_CREAM);
        Draw_texture(&pixel, UI_PANEL, 10, 42, 300, 42);
        Draw_c("CURRENT SIGNAL", 18, 48, 9.5f, UI_CYAN);
        snprintf(line, sizeof(line), "CH %02lu  %.35s",
                 (unsigned long)(switch_from_index + 1),
                 channel_names[switch_from_index]);
        Draw_c(line, 18, 64, 12.0f, UI_CREAM);

        Draw_texture(&pixel, UI_SHADOW, 38, 96, 244, 10);
        for (i = 0; i < 10; i++)
            Draw_texture(&pixel, i == phase ? UI_CREAM : UI_ORANGE,
                         42 + (float)i * 24, 98, 16, 6);

        Draw_texture(&pixel, UI_PANEL, 10, 118, 300, 42);
        Draw_c("NEXT SIGNAL", 18, 124, 9.5f, UI_PINK);
        snprintf(line, sizeof(line), "CH %02lu  %.35s",
                 (unsigned long)(switch_to_index + 1),
                 channel_names[switch_to_index]);
        Draw_c(line, 18, 140, 12.0f, UI_CREAM);

        format_tune_status(line, sizeof(line), &tune);
        Draw_align_c(line, 8, 176, 10.5f, UI_MINT,
                     DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 304, 20);
        Draw_align_c("B CANCEL // L/R CHANGE AGAIN", 8, 205, 9.5f,
                     UI_ORANGE, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                     304, 14);
        Draw_set_refresh_needed(true);
        return;
    }

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

    if (state == LIVE_APP_ERROR && has_player_error_diagnostics) {
        Draw_texture(&pixel, UI_PANEL, 8, 181, 304, 29);
        snprintf(line, sizeof(line), "V PKT:%lu DEC:%lu TEX:%lu SHOW:%u",
                 (unsigned long)player_error_diagnostics.video_packets,
                 (unsigned long)player_error_diagnostics.decoded_frames,
                 (unsigned long)player_error_diagnostics.textures,
                 player_error_diagnostics.presented ? 1u : 0u);
        Draw_align_c(line, 12, 182, 8.5f, UI_CYAN,
                     DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 296, 12);
        snprintf(line, sizeof(line), "A %u %s RX:%lu DEC:%lu Q:%lu AE:%lX",
                 player_error_diagnostics.audio_tracks,
                 audio_diagnostic_state(player_error_diagnostics.audio_state),
                 (unsigned long)player_error_diagnostics.audio_demux_packets,
                 (unsigned long)player_error_diagnostics.audio_frames,
                 (unsigned long)player_error_diagnostics.audio_buffers,
                 (unsigned long)player_error_diagnostics.audio_last_error);
        Draw_align_c(line, 12, 196, 8.0f, UI_MINT,
                     DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 296, 12);
    } else {
        Draw_texture(&pixel, UI_PANEL, 8, 183, 304, 28);
        if (state == LIVE_APP_LOADING)
            format_tune_status(line, sizeof(line), &tune);
        Draw_align_c(state == LIVE_APP_LOADING ? line : status,
                     14, 184, 9.5f,
                     state == LIVE_APP_ERROR || state == LIVE_APP_NO_PLAYLIST
                         ? DEF_DRAW_RED : UI_MINT,
                     DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 292, 24);
    }
    if (state == LIVE_APP_LOADING) {
        draw_key_hint(&pixel, "B", "CANCEL", 9, 211, 17, UI_PINK);
        draw_key_hint(&pixel, "L/R", "CHANGE", 77, 211, 32, UI_CYAN);
    } else if (state == LIVE_APP_ERROR) {
        draw_key_hint(&pixel, "A", "RETRY", 9, 211, 17, UI_PINK);
        draw_key_hint(&pixel, "B", "DECK", 75, 211, 17, UI_MINT);
        draw_key_hint(&pixel, "L/R", "CHANGE", 133, 211, 29, UI_CYAN);
    } else {
        draw_key_hint(&pixel, "A", "TUNE", 9, 211, 17, UI_PINK);
        draw_key_hint(&pixel, "UP/DN", "PICK", 77, 211, 42, UI_CYAN);
    }
    draw_key_hint(&pixel, "START", "QUIT", 209, 211, 45, UI_ORANGE);
}

void MiniIptv_live_app_init(void) {
    int telemetry_result;
    memset(&app, 0, sizeof(app));
    LightLock_Init(&app.lock);
    miniiptv_live_tune_telemetry_init();
    telemetry_result = miniiptv_telemetry_log_open(
        TELEMETRY_LOG, RETROTUNER_VERSION, osGetTime());

	if (playlist_load_file(USER_PLAYLIST, &app.playlist) != 0) {
		set_status_locked(LIVE_APP_NO_PLAYLIST,
						  "ADD /3ds/retrotuner3ds/channels.m3u");
    } else {
        set_status_locked(LIVE_APP_IDLE,
            telemetry_result == 0
                ? "READY // TELEMETRY LOG ON // press A to tune"
                : "READY // LOG OFF (SD WRITE FAILED) // press A to tune");
    }

    Vid_set_idle_hooks(live_hid, live_draw);
    Vid_set_live_error_hook(player_error);
    Vid_set_live_channel_hook(player_channel_request);
}

void MiniIptv_live_app_exit(void) {
    Thread worker = NULL;
    bool network_ready;

    LightLock_Lock(&app.lock);
    app.exit_requested = true;
    app.pending_channel_step = 0;
    app.switching_from_player = false;
    worker = app.worker;
    app.worker = NULL;
    LightLock_Unlock(&app.lock);

    Vid_set_live_error_hook(NULL);
    Vid_set_live_channel_hook(NULL);
    Vid_set_idle_hooks(NULL, NULL);
    miniiptv_live_stream_request_stop();
    if (worker) {
        threadJoin(worker, UINT64_MAX);
        threadFree(worker);
    }
    /* The worker can initialize/reset the stream after the first request.
     * Stop it again, then join every FFmpeg/MVD reader before the stream lock
     * and curl session are destroyed. Menu_exit() observes Video uninited and
     * therefore will not invoke Vid_exit() a second time. */
    miniiptv_live_stream_request_stop();
    if (Vid_query_init_flag()) Vid_exit(!aptShouldClose());
    miniiptv_live_stream_stop();
    LightLock_Lock(&app.lock);
    network_ready = app.network_ready;
    app.network_ready = false;
    LightLock_Unlock(&app.lock);
    if (network_ready) miniiptv_live_session_exit();
    miniiptv_telemetry_log_close();
}

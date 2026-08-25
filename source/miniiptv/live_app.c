#include "miniiptv/live_app.h"

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "miniiptv/live_session.h"
#include "miniiptv/live_stream.h"
#include "miniiptv/channel_scan.h"
#include "miniiptv/network.h"
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
#define PREVIOUS_TELEMETRY_LOG \
    "sdmc:/3ds/retrotuner3ds/telemetry-prev.csv"
#define CHANNELS_PER_PAGE 10u
#define DRAWER_CHANNELS_PER_PAGE 8u
#define AUTO_RELOCK_MAX_ATTEMPTS 2u
#define AUTO_RELOCK_WINDOW_MS 30000ULL

/* Minimal 1990s portable-TV palette (ABGR8888). */
#define UI_INK 0xFF121110u
#define UI_PANEL 0xFF2C2926u
#define UI_CREAM 0xFFE8EBEDu
#define UI_MINT 0xFFB6B9B9u
#define UI_PINK 0xFF4F4FD5u
#define UI_CYAN 0xFFD2B56Cu

typedef enum {
    LIVE_APP_NO_PLAYLIST = 0,
    LIVE_APP_IDLE,
    LIVE_APP_LOADING,
    LIVE_APP_READY,
    LIVE_APP_ERROR
} LiveAppState;

typedef enum {
    CHANNEL_SESSION_UNTRIED = 0,
    CHANNEL_SESSION_PLAYED,
    CHANNEL_SESSION_FAILED
} ChannelSessionState;

typedef enum {
    PLAYER_RETURN_NONE = 0,
    PLAYER_RETURN_STOP,
    PLAYER_RETURN_STOP_TO_DECK,
    PLAYER_RETURN_RETUNE
} PlayerReturnAction;

typedef struct {
    LightLock lock;
    MiniIptvPlaylist source_playlist;
    MiniIptvPlaylist playlist;
    MiniIptvScanStatus playlist_scan_status[MINIIPTV_MAX_CHANNELS];
    ChannelSessionState channel_session_state[MINIIPTV_MAX_CHANNELS];
    MiniIptvScanResult scan_results[MINIIPTV_MAX_CHANNELS];
    size_t scan_index;
    size_t scan_current_index;
    Thread scan_worker;
    bool scan_worker_finished;
    bool scan_worker_running;
    bool scan_stop_requested;
    bool scan_complete;
    MiniIptvChannel pending_channel;
    MiniIptvStageInfo stage_info;
    size_t selected;
    size_t drawer_selected;
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
    bool pending_channel_target_valid;
    size_t pending_channel_target;
    uint64_t tuning_started_ms;
    bool switching_from_player;
    size_t switch_from_index;
    size_t switch_to_index;
    uint32_t player_error_code;
    Vid_live_diagnostics player_error_diagnostics;
    bool has_player_error_diagnostics;
    unsigned int auto_relock_attempts;
    uint64_t auto_relock_window_started_ms;
    MiniIptvBoundaryReason pending_relock_reason;
    bool exit_requested;
} LiveApp;

static LiveApp app;

static void launch_tune_worker(void);
static void launch_scan_worker(void);
static void draw_static_aperture(Draw_image_data *pixel, uint64_t now,
                                 float top, float height);

void MiniIptv_live_app_draw_boot_screen(void) {
    Draw_image_data pixel = Draw_get_empty_image();
    static uint64_t boot_started_ms = 0;
    uint64_t now = osGetTime();
    uint64_t elapsed;
    float aperture_height;
    float aperture_top;
    unsigned int sweep;

    if (boot_started_ms == 0 || now < boot_started_ms)
        boot_started_ms = now;
    elapsed = now - boot_started_ms;
    aperture_height = elapsed >= 1500u
        ? 240.0f : 2.0f + (float)elapsed * 238.0f / 1500.0f;
    aperture_top = (240.0f - aperture_height) / 2.0f;
    sweep = (unsigned int)((elapsed / 90u) % 16u);

    Draw_frame_ready();
    Draw_screen_ready(DRAW_SCREEN_TOP_LEFT, UI_INK);
    draw_static_aperture(&pixel, now, aperture_top, aperture_height);
    if (elapsed > 450u) {
        Draw_texture(&pixel, 0xD0121110u, 84, 87, 232, 65);
        Draw_align_c("RETRO TUNER", 0, 96, 20.0f, UI_CREAM,
                     DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 28);
        Draw_align_c("PORTABLE TELEVISION", 0, 126, 9.5f, UI_MINT,
                     DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 16);
    }

    Draw_screen_ready(DRAW_SCREEN_BOTTOM, UI_INK);
    Draw_align_c("WARMING UP", 0, 76, 14.0f, UI_CREAM,
                 DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 320, 18);
    Draw_align_c("VIDEO  /  AUDIO  /  NETWORK", 0, 102, 9.0f, UI_MINT,
                 DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 320, 14);
    Draw_texture(&pixel, UI_PANEL, 31, 132, 258, 6);
    Draw_texture(&pixel, UI_CREAM, 33 + (float)sweep * 16, 133, 14, 4);
    Draw_apply_draw();
}

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

static uint32_t tuning_static_next(uint32_t *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed;
}

static void draw_static_aperture(Draw_image_data *pixel, uint64_t now,
                                 float top, float height) {
    static const uint32_t snow[] = {
        0xFF242424u, 0xFF484848u, 0xFF747474u, 0xFFA8A8A8u,
        0xFFD8D8D8u
    };
    uint32_t seed = (uint32_t)(now / 70u) ^ 0x52543344u;
    size_t i;

    if (height < 1.0f) height = 1.0f;
    if (top < 0.0f) top = 0.0f;
    if (top + height > 240.0f) height = 240.0f - top;
    Draw_texture(pixel, 0xFF181818u, 0, top, 400, height);
    for (i = 0; i < 40; i++) {
        uint32_t value = tuning_static_next(&seed);
        float x = (float)(value % 400u);
        float y = top + (float)((value >> 9) % (uint32_t)height);
        float width = 8.0f + (float)((value >> 18) % 84u);
        float noise_height = 1.0f + (float)((value >> 27) % 4u);
        if (x + width > 400.0f) width = 400.0f - x;
        if (y + noise_height > top + height)
            noise_height = top + height - y;
        Draw_texture(pixel,
                     snow[(value >> 24) %
                          (sizeof(snow) / sizeof(snow[0]))],
                     x, y, width, noise_height);
    }
    for (i = (size_t)top; i < (size_t)(top + height); i += 7u)
        Draw_texture(pixel, (i & 1u) ? 0xFF303030u : 0xFF0D0D0Du,
                     0, (float)i, 400, 1);
}

static void draw_tuning_static(Draw_image_data *pixel, uint64_t now) {
    draw_static_aperture(pixel, now, 15.0f, 225.0f);
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
        case MINIIPTV_STAGE_TS_MUX_FAILED:
            return "SEPARATE AUDIO UNSUPPORTED // TRY ANOTHER SIGNAL";
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

static int tune_should_cancel(void *unused) {
    bool cancel;
    (void)unused;
    LightLock_Lock(&app.lock);
    cancel = app.exit_requested || app.cancel_to_deck || app.queued_tune;
    LightLock_Unlock(&app.lock);
    return cancel;
}

static int scan_should_cancel(void *unused) {
    bool cancel;
    (void)unused;
    LightLock_Lock(&app.lock);
    cancel = app.exit_requested || app.scan_stop_requested;
    LightLock_Unlock(&app.lock);
    return cancel;
}

static void scan_worker_main(void *unused) {
    const NetworkRequestOptions scan_options = {4u, 6u};
    (void)unused;

    LightLock_Lock(&app.lock);
    if (!app.network_ready && !app.exit_requested &&
        !app.scan_stop_requested) {
        LightLock_Unlock(&app.lock);
        if (miniiptv_live_session_init() == 0) {
            LightLock_Lock(&app.lock);
            app.network_ready = true;
            LightLock_Unlock(&app.lock);
        } else {
            LightLock_Lock(&app.lock);
            app.scan_complete = true;
            if (app.state == LIVE_APP_IDLE)
                snprintf(app.status, sizeof(app.status),
                         "RADIO OFFLINE // NETWORK COULD NOT START");
            LightLock_Unlock(&app.lock);
        }
    } else {
        LightLock_Unlock(&app.lock);
    }

    for (;;) {
        MiniIptvChannel channel;
        MiniIptvScanResult scan_result;
        NetworkTextResponse root = {0};
        NetworkTextResponse media = {0};
        size_t source_index;
        int fetch_result;
        int needs_media = 0;

        LightLock_Lock(&app.lock);
        if (app.exit_requested || app.scan_stop_requested ||
            !app.network_ready ||
            app.scan_index >= app.source_playlist.count) {
            if (app.scan_index >= app.source_playlist.count)
                app.scan_complete = true;
            LightLock_Unlock(&app.lock);
            break;
        }
        source_index = app.scan_index;
        app.scan_current_index = source_index;
        channel = app.source_playlist.channels[source_index];
        app.scan_results[source_index].status = MINIIPTV_SCAN_CHECKING;
        if (app.state == LIVE_APP_IDLE)
            snprintf(app.status, sizeof(app.status),
                     "SCANNING CH %02lu/%02lu // %.56s",
                     (unsigned long)(source_index + 1),
                     (unsigned long)app.source_playlist.count,
                     channel.name);
        LightLock_Unlock(&app.lock);
        Draw_set_refresh_needed(true);

        memset(&scan_result, 0, sizeof(scan_result));
        fetch_result = network_get_data_cancelable_with_options(
            channel.url, channel.user_agent, channel.referrer,
            MINIIPTV_MANIFEST_LIMIT, scan_should_cancel, NULL,
            &scan_options, &root);
        if (fetch_result == 0) {
            const char *root_url = root.final_url[0]
                ? root.final_url : channel.url;
            needs_media = miniiptv_channel_scan_classify_root(
                root.data, root_url, &scan_result);
            if (needs_media) {
                fetch_result = network_get_data_cancelable_with_options(
                    scan_result.media_url, channel.user_agent,
                    channel.referrer, MINIIPTV_MANIFEST_LIMIT,
                    scan_should_cancel, NULL, &scan_options, &media);
                if (fetch_result == 0) {
                    const char *media_url = media.final_url[0]
                        ? media.final_url : scan_result.media_url;
                    miniiptv_channel_scan_classify_media(
                        media.data, media_url, &scan_result);
                }
            }
        }
        if (fetch_result != 0) {
            memset(&scan_result, 0, sizeof(scan_result));
            scan_result.status = MINIIPTV_SCAN_OFFLINE;
            scan_result.detail = fetch_result;
        }
        network_response_free(&media);
        network_response_free(&root);

        LightLock_Lock(&app.lock);
        if (app.exit_requested || app.scan_stop_requested) {
            LightLock_Unlock(&app.lock);
            break;
        }
        app.scan_results[source_index] = scan_result;
        if ((scan_result.status == MINIIPTV_SCAN_READY ||
             scan_result.status == MINIIPTV_SCAN_UNKNOWN) &&
            app.playlist.count < MINIIPTV_MAX_CHANNELS) {
            size_t found_index = app.playlist.count++;
            app.playlist.channels[found_index] = channel;
            app.playlist_scan_status[found_index] = scan_result.status;
        }
        app.scan_index = source_index + 1;
        if (app.scan_index >= app.source_playlist.count)
            app.scan_complete = true;
        if (app.state == LIVE_APP_IDLE) {
            if (app.scan_complete)
                snprintf(app.status, sizeof(app.status),
                         "AIRWAVES READY // * VERIFIED // ? TUNE CHECK");
            else
                snprintf(app.status, sizeof(app.status),
                         "%s // %lu FOUND // SCANNING %02lu/%02lu",
                         miniiptv_channel_scan_status_label(
                             scan_result.status),
                         (unsigned long)app.playlist.count,
                         (unsigned long)app.scan_index,
                         (unsigned long)app.source_playlist.count);
        }
        LightLock_Unlock(&app.lock);
        Draw_set_refresh_needed(true);
    }

    LightLock_Lock(&app.lock);
    app.scan_worker_running = false;
    app.scan_worker_finished = true;
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
    threadExit(0);
}

static void stop_scan_worker(void) {
    Thread worker;
    LightLock_Lock(&app.lock);
    app.scan_stop_requested = true;
    worker = app.scan_worker;
    app.scan_worker = NULL;
    LightLock_Unlock(&app.lock);
    if (worker) {
        threadJoin(worker, UINT64_MAX);
        threadFree(worker);
    }
    LightLock_Lock(&app.lock);
    app.scan_worker_running = false;
    app.scan_worker_finished = false;
    LightLock_Unlock(&app.lock);
}

static void reap_scan_worker_if_finished(void) {
    Thread worker = NULL;
    LightLock_Lock(&app.lock);
    if (app.scan_worker && app.scan_worker_finished) {
        worker = app.scan_worker;
        app.scan_worker = NULL;
        app.scan_worker_finished = false;
    }
    LightLock_Unlock(&app.lock);
    if (worker) {
        threadJoin(worker, UINT64_MAX);
        threadFree(worker);
    }
}

static void launch_scan_worker(void) {
    Thread worker;
    LightLock_Lock(&app.lock);
    if (app.exit_requested || app.scan_complete || app.scan_worker ||
        app.worker || app.worker_reaping || app.stream_cleanup_pending ||
        app.awaiting_player_return || app.state != LIVE_APP_IDLE ||
        app.scan_index >= app.source_playlist.count) {
        LightLock_Unlock(&app.lock);
        return;
    }
    app.scan_stop_requested = false;
    app.scan_worker_finished = false;
    worker = threadCreate(scan_worker_main, NULL, 64 * 1024,
                          DEF_THREAD_PRIORITY_NORMAL, 1, false);
    app.scan_worker = worker;
    app.scan_worker_running = worker != NULL;
    if (!worker)
        snprintf(app.status, sizeof(app.status),
                 "SCANNER COULD NOT START // PRESS A TO USE FOUND STATIONS");
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
}

static bool recoverable_stream_boundary_error(int result) {
    return result == MINIIPTV_STAGE_DISCONTINUITY ||
           result == MINIIPTV_STAGE_TOO_LARGE;
}

static void reset_auto_relock_locked(void) {
    app.auto_relock_attempts = 0;
    app.auto_relock_window_started_ms = 0;
    app.pending_relock_reason = MINIIPTV_BOUNDARY_NONE;
}

static void player_error(uint32_t error_code) {
    Vid_live_diagnostics diagnostics = {0};
    Vid_query_live_diagnostics(&diagnostics);
    miniiptv_live_tune_fail((int32_t)error_code);
    LightLock_Lock(&app.lock);
    /* A player failure can race an already-latched L/R request. Keep that
     * request so teardown returns into the requested channel, not the failed
     * one. With no request, this remains an ordinary NO SIGNAL state. */
    if (app.pending_channel_step == 0 &&
        !app.pending_channel_target_valid)
        app.switching_from_player = false;
    app.state = error_code == DEF_ERR_UNSAFE_VIDEO_STREAM
        ? LIVE_APP_LOADING : LIVE_APP_ERROR;
    app.player_error_code = error_code;
    app.player_error_diagnostics = diagnostics;
    app.has_player_error_diagnostics = true;
    if (app.selected < app.playlist.count)
        app.channel_session_state[app.selected] =
            diagnostics.presented_frames > 0
                ? CHANNEL_SESSION_PLAYED
                : (app.channel_session_state[app.selected] ==
                       CHANNEL_SESSION_PLAYED
                       ? CHANNEL_SESSION_PLAYED : CHANNEL_SESSION_FAILED);
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
    else if ((int32_t)error_code == MINIIPTV_STAGE_TS_MUX_FAILED)
        snprintf(app.status, sizeof(app.status),
                 "SEPARATE AUDIO COULD NOT SYNC");
    else if (error_code == DEF_ERR_UNSAFE_VIDEO_STREAM)
        snprintf(app.status, sizeof(app.status),
                 "SIGNAL SHIFT // PREPARING CLEAN RELOCK...");
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
        app.pending_channel_target_valid = false;
        app.switching_from_player = false;
        LightLock_Unlock(&app.lock);
        Draw_set_refresh_needed(true);
        return;
    }
    if (app.switching_from_player || app.pending_channel_step != 0 ||
        app.pending_channel_target_valid) {
        LightLock_Unlock(&app.lock);
        return;
    }
    app.cancel_to_deck = false;
    reset_auto_relock_locked();
    app.pending_channel_target_valid = false;
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

static Vid_live_drawer_result live_drawer_hid(const Hid_info *key) {
    Vid_live_drawer_result result = VID_LIVE_DRAWER_HANDLED;
    size_t count;

    if (!key) return result;
    LightLock_Lock(&app.lock);
    count = app.playlist.count;
    if (count == 0) {
        LightLock_Unlock(&app.lock);
        return VID_LIVE_DRAWER_CLOSE;
    }
    if (app.drawer_selected >= count) app.drawer_selected = app.selected;

    if (DEF_HID_PHY_PR(key->d_up))
        app.drawer_selected = app.drawer_selected == 0
            ? count - 1 : app.drawer_selected - 1;
    else if (DEF_HID_PHY_PR(key->d_down))
        app.drawer_selected = (app.drawer_selected + 1) % count;
    else if (DEF_HID_PHY_PR(key->d_left)) {
        size_t page = app.drawer_selected / DRAWER_CHANNELS_PER_PAGE;
        if (page > 0) app.drawer_selected =
            (page - 1u) * DRAWER_CHANNELS_PER_PAGE;
    } else if (DEF_HID_PHY_PR(key->d_right)) {
        size_t page = app.drawer_selected / DRAWER_CHANNELS_PER_PAGE;
        size_t pages = (count + DRAWER_CHANNELS_PER_PAGE - 1u) /
            DRAWER_CHANNELS_PER_PAGE;
        if (page + 1u < pages) app.drawer_selected =
            (page + 1u) * DRAWER_CHANNELS_PER_PAGE;
    } else if (DEF_HID_PHY_PR(key->a)) {
        if (app.drawer_selected == app.selected) {
            result = VID_LIVE_DRAWER_CLOSE;
        } else {
            reset_auto_relock_locked();
            app.cancel_to_deck = false;
            app.queued_tune = false;
            app.pending_channel_step = 0;
            app.pending_channel_target_valid = true;
            app.pending_channel_target = app.drawer_selected;
            app.switching_from_player = true;
            app.switch_from_index = app.selected;
            app.switch_to_index = app.drawer_selected;
            snprintf(app.status, sizeof(app.status),
                     "SWITCHING TO CH %02lu...",
                     (unsigned long)(app.drawer_selected + 1u));
            result = VID_LIVE_DRAWER_TUNE;
        }
    }
    LightLock_Unlock(&app.lock);
    Draw_set_refresh_needed(true);
    return result;
}

static void live_drawer_draw(uint32_t color, uint32_t back_color) {
    Draw_image_data pixel = Draw_get_empty_image();
    char names[MINIIPTV_MAX_CHANNELS][MINIIPTV_NAME_MAX];
    ChannelSessionState session[MINIIPTV_MAX_CHANNELS];
    MiniIptvScanStatus scan[MINIIPTV_MAX_CHANNELS];
    char line[96];
    size_t count;
    size_t active;
    size_t selected;
    size_t page_start;
    size_t page_end;
    size_t page_count;
    size_t i;

    (void)color;
    (void)back_color;
    LightLock_Lock(&app.lock);
    count = app.playlist.count;
    active = app.selected;
    selected = app.drawer_selected < count ? app.drawer_selected : active;
    for (i = 0; i < count; i++) {
        snprintf(names[i], sizeof(names[i]), "%s",
                 app.playlist.channels[i].name);
        session[i] = app.channel_session_state[i];
        scan[i] = app.playlist_scan_status[i];
    }
    LightLock_Unlock(&app.lock);

    page_start = count
        ? (selected / DRAWER_CHANNELS_PER_PAGE) * DRAWER_CHANNELS_PER_PAGE : 0;
    page_end = page_start + DRAWER_CHANNELS_PER_PAGE;
    if (page_end > count) page_end = count;
    page_count = count
        ? (count + DRAWER_CHANNELS_PER_PAGE - 1u) /
            DRAWER_CHANNELS_PER_PAGE : 0;

    Draw_texture(&pixel, UI_INK, 0, 0, 320, 225);
    Draw_texture(&pixel, UI_CREAM, 10, 10, 300, 1);
    Draw_c("CHANNELS", 12, 17, 13.0f, UI_CREAM);
    snprintf(line, sizeof(line), "PAGE %lu/%lu",
             (unsigned long)(page_count ? page_start /
                 DRAWER_CHANNELS_PER_PAGE + 1u : 0u),
             (unsigned long)page_count);
    Draw_align_c(line, 225, 17, 9.0f, UI_MINT,
                 DRAW_X_ALIGN_RIGHT, DRAW_Y_ALIGN_CENTER, 82, 14);
    snprintf(line, sizeof(line), "LIVE  CH %02lu",
             (unsigned long)(active + 1u));
    Draw_c(line, 12, 32, 9.0f, UI_CYAN);

    for (i = page_start; i < page_end; i++) {
        float y = 49.0f + (float)(i - page_start) * 17.0f;
        char marker = session[i] == CHANNEL_SESSION_PLAYED ? '+' :
            (session[i] == CHANNEL_SESSION_FAILED ? '!' :
             (scan[i] == MINIIPTV_SCAN_READY ? '*' : '?'));
        if (i == selected)
            Draw_texture(&pixel, UI_CREAM, 10, y, 300, 15);
        else if (i == active)
            Draw_texture(&pixel, UI_PANEL, 10, y, 300, 15);
        snprintf(line, sizeof(line), "%c %02lu  %.34s",
                 marker, (unsigned long)(i + 1u), names[i]);
        Draw_c(line, 17, y + 2, 10.0f,
               i == selected ? UI_INK :
                   (i == active ? UI_CYAN : UI_CREAM));
    }

    Draw_texture(&pixel, UI_CREAM, 10, 192, 300, 1);
    Draw_align_c("A TUNE   B CLOSE   LEFT/RIGHT PAGE", 8, 198, 9.0f,
                 UI_CREAM, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                 304, 15);
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
    app.drawer_selected = app.selected;
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
        app.pending_channel_target_valid = false;
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
    MiniIptvBoundaryReason relock_reason;
    (void)unused;

    LightLock_Lock(&app.lock);
    network_ready = app.network_ready;
    pending_channel = app.pending_channel;
    exiting = app.exit_requested;
    cancel_to_deck = app.cancel_to_deck;
    queued_tune = app.queued_tune;
    relock_reason = app.pending_relock_reason;
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
                                                relock_reason,
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
        app.pending_channel_target_valid = false;
        app.switching_from_player = false;
        stop_stream = result == MINIIPTV_STAGE_OK;
    } else if (cancel_to_deck) {
        app.pending_channel_step = 0;
        app.pending_channel_target_valid = false;
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
        if (app.selected < app.playlist.count &&
            result != MINIIPTV_STAGE_CANCELLED &&
            app.channel_session_state[app.selected] !=
                CHANNEL_SESSION_PLAYED)
            app.channel_session_state[app.selected] =
                CHANNEL_SESSION_FAILED;
        app.pending_channel_step = 0;
        app.pending_channel_target_valid = false;
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
    reset_auto_relock_locked();
    app.tuning_started_ms = osGetTime();
    set_status_locked(LIVE_APP_LOADING,
                      "TUNING > LOCK > RESERVE > PLAY...");
    return true;
}

static void launch_tune_worker(void) {
    Thread worker;

    /* The scanner and player deliberately share one curl handle and one small
     * Wi-Fi pipe. Fully stop manifest discovery before staging video. */
    stop_scan_worker();

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
                              "SWITCHING > LOCK > RESERVE > PLAY...");
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
    if (app.state != LIVE_APP_ERROR && app.selected < app.playlist.count)
        app.channel_session_state[app.selected] = CHANNEL_SESSION_PLAYED;
    if (!deck_requested &&
        (app.pending_channel_target_valid || app.pending_channel_step != 0) &&
        app.playlist.count > 0) {
        if (app.pending_channel_target_valid)
            app.selected = app.pending_channel_target < app.playlist.count
                ? app.pending_channel_target : app.selected;
        else if (app.pending_channel_step < 0)
            app.selected = app.selected == 0 ? app.playlist.count - 1
                                             : app.selected - 1;
        else
            app.selected = (app.selected + 1) % app.playlist.count;
        reset_auto_relock_locked();
        app.pending_channel_step = 0;
        app.pending_channel_target_valid = false;
        app.drawer_selected = app.selected;
        app.pending_channel = app.playlist.channels[app.selected];
        app.cancel_to_deck = false;
        app.queued_tune = false;
        app.tuning_started_ms = osGetTime();
        set_status_locked(LIVE_APP_LOADING,
                          "SWITCHING > LOCK > RESERVE > PLAY...");
        /* If the playback-return event wins the race with reaping the worker
         * that opened this player, remember the tune now. The reaper will own
         * exactly one launch after freeing that old handle. */
        if (app.worker || app.worker_reaping)
            app.launch_after_reap = true;
        return PLAYER_RETURN_RETUNE;
    }
    app.pending_channel_step = 0;
    app.pending_channel_target_valid = false;
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
    return deck_requested ? PLAYER_RETURN_STOP_TO_DECK
                          : PLAYER_RETURN_STOP;
}

static void process_player_return_action(PlayerReturnAction action) {
    bool launch_queued = false;
    bool auto_relock = false;
    bool preserve_stream_error = false;
    bool recoverable_player_boundary = false;
    int stream_error = 0;
    MiniIptvLiveInfo stream_info = {0};
    Vid_live_diagnostics stream_error_diagnostics = {0};

    if (action == PLAYER_RETURN_NONE) return;
    if (action == PLAYER_RETURN_STOP) {
        miniiptv_live_stream_get_stats(NULL, NULL, NULL, NULL,
                                       &stream_error);
        miniiptv_live_stream_get_info(&stream_info);
        LightLock_Lock(&app.lock);
        recoverable_player_boundary =
            app.player_error_code == DEF_ERR_UNSAFE_VIDEO_STREAM;
        LightLock_Unlock(&app.lock);
        if (stream_error != 0) {
            Vid_query_live_diagnostics(&stream_error_diagnostics);
            miniiptv_live_tune_fail(stream_error);
            preserve_stream_error = true;
        } else if (recoverable_player_boundary) {
            Vid_query_live_diagnostics(&stream_error_diagnostics);
            preserve_stream_error = true;
        }
    }
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
        if (preserve_stream_error && !app.exit_requested &&
            (recoverable_stream_boundary_error(stream_error) ||
             recoverable_player_boundary) &&
            !app.cancel_to_deck && !app.queued_tune &&
            app.playlist.count > 0) {
            uint64_t now = osGetTime();
            if (app.auto_relock_window_started_ms == 0 ||
                now < app.auto_relock_window_started_ms ||
                now - app.auto_relock_window_started_ms >
                    AUTO_RELOCK_WINDOW_MS) {
                app.auto_relock_attempts = 0;
                app.auto_relock_window_started_ms = now;
            }
            if (app.auto_relock_attempts < AUTO_RELOCK_MAX_ATTEMPTS) {
                app.auto_relock_attempts++;
                app.pending_channel = app.playlist.channels[app.selected];
                if (recoverable_player_boundary)
                    app.pending_relock_reason =
                        MINIIPTV_BOUNDARY_PLAYER_FORMAT;
                else if (stream_info.boundary_reason !=
                         MINIIPTV_BOUNDARY_NONE)
                    app.pending_relock_reason = stream_info.boundary_reason;
                else if (stream_error == MINIIPTV_STAGE_TOO_LARGE)
                    app.pending_relock_reason =
                        MINIIPTV_BOUNDARY_OVERSIZED_SEGMENT;
                else
                    app.pending_relock_reason =
                        MINIIPTV_BOUNDARY_DISCONTINUITY;
                app.tuning_started_ms = now;
                set_status_locked(
                    LIVE_APP_LOADING,
                    app.auto_relock_attempts == 1u
                        ? "SIGNAL SHIFT // CLEAN RELOCK 1/2..."
                        : "SIGNAL SHIFT // CLEAN RELOCK 2/2...");
                auto_relock = true;
                if (app.worker || app.worker_reaping)
                    app.launch_after_reap = true;
            }
        }
        if (!auto_relock &&
            preserve_stream_error && !app.exit_requested &&
            app.state != LIVE_APP_ERROR) {
            app.state = LIVE_APP_ERROR;
            app.player_error_code = (uint32_t)stream_error;
            app.player_error_diagnostics = stream_error_diagnostics;
            app.has_player_error_diagnostics = true;
            snprintf(app.status, sizeof(app.status), "%s (%d)",
                     stage_error_text(stream_error), stream_error);
        }
        /* HID and draw run independently. If L/R arrived while the old stream
         * was being joined, consume that queued choice as soon as cleanup has
         * released the single stream/decoder owner. */
        if (!auto_relock && !app.exit_requested && app.queued_tune &&
            !app.cancel_to_deck &&
            !app.worker && !app.worker_reaping && app.playlist.count > 0) {
            app.queued_tune = false;
            app.pending_channel = app.playlist.channels[app.selected];
            app.tuning_started_ms = osGetTime();
            set_status_locked(LIVE_APP_LOADING,
                              "SWITCHING > LOCK > RESERVE > PLAY...");
            launch_queued = true;
        }
        LightLock_Unlock(&app.lock);
        Draw_set_refresh_needed(true);
        if (auto_relock || launch_queued) launch_tune_worker();
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
            app.pending_channel_target_valid = false;
            app.switching_from_player = false;
            error_return_input_latched = true;
        } else if (app.playlist.count > 0 && DEF_HID_PHY_PR(key->l)) {
            app.cancel_to_deck = false;
            app.pending_channel_target_valid = false;
            app.pending_channel_step = -1;
            app.switching_from_player = true;
            app.switch_from_index = app.selected;
            app.switch_to_index = app.selected == 0
                ? app.playlist.count - 1 : app.selected - 1;
            error_return_input_latched = true;
        } else if (app.playlist.count > 0 && DEF_HID_PHY_PR(key->r)) {
            app.cancel_to_deck = false;
            app.pending_channel_target_valid = false;
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
            app.pending_channel_target_valid = false;
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
            reset_auto_relock_locked();
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
        app.pending_channel_target_valid = false;
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
        reset_auto_relock_locked();
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
                              "SWITCHING > LOCK > RESERVE > PLAY...");
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
    if (!app.stream_cleanup_pending && !app.worker && !app.worker_reaping &&
        state != LIVE_APP_LOADING && count &&
        (DEF_HID_PHY_PR(key->d_left) || DEF_HID_PHY_PR(key->d_right))) {
        size_t page = app.selected / CHANNELS_PER_PAGE;
        size_t page_count =
            (count + CHANNELS_PER_PAGE - 1u) / CHANNELS_PER_PAGE;

        if (DEF_HID_PHY_PR(key->d_left) && page > 0u)
            app.selected = (page - 1u) * CHANNELS_PER_PAGE;
        else if (DEF_HID_PHY_PR(key->d_right) && page + 1u < page_count)
            app.selected = (page + 1u) * CHANNELS_PER_PAGE;
        else {
            LightLock_Unlock(&app.lock);
            return true;
        }
        set_status_locked(LIVE_APP_IDLE,
                          "READY // press A to tune this signal");
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
    MiniIptvScanStatus channel_scan_status[MINIIPTV_MAX_CHANNELS];
    ChannelSessionState channel_session_state[MINIIPTV_MAX_CHANNELS];
    LiveAppState state;
    size_t count;
    size_t selected;
    uint64_t tuning_started_ms;
    bool switching_from_player;
    size_t switch_from_index;
    size_t switch_to_index;
    uint32_t player_error_code;
    char status[160];
    char line[112];
    size_t i;
    size_t page_start;
    size_t page_end;
    size_t page_count;
    size_t page_number;
    PlayerReturnAction return_action;
    bool scan_running;
    size_t scan_current_index;
    size_t source_count;
    char scan_channel_name[MINIIPTV_NAME_MAX];
    MiniIptvScanStatus current_scan_status;

    (void)color;
    (void)back_color;

    reap_worker_if_finished();
    reap_scan_worker_if_finished();
    LightLock_Lock(&app.lock);
    return_action = update_player_return_locked();
    LightLock_Unlock(&app.lock);
    process_player_return_action(return_action);
    launch_scan_worker();

    /* Snapshot after processing a playback return. Otherwise one stale deck
     * or error frame is drawn between the player and the relocking animation,
     * making a controlled decoder rebuild look like a failed channel. */
    LightLock_Lock(&app.lock);
    count = app.playlist.count;
    for (i = 0; i < count; i++) {
        snprintf(channel_names[i], sizeof(channel_names[i]), "%s",
                 app.playlist.channels[i].name);
        channel_scan_status[i] = app.playlist_scan_status[i];
        channel_session_state[i] = app.channel_session_state[i];
    }
    state = app.state;
    selected = app.selected;
    tuning_started_ms = app.tuning_started_ms;
    switching_from_player = app.switching_from_player;
    switch_from_index = app.switch_from_index;
    switch_to_index = app.switch_to_index;
    player_error_code = app.player_error_code;
    snprintf(status, sizeof(status), "%s", app.status);
    scan_running = app.scan_worker_running;
    scan_current_index = app.scan_current_index;
    source_count = app.source_playlist.count;
    current_scan_status = source_count
        ? app.scan_results[scan_current_index].status
        : MINIIPTV_SCAN_UNCHECKED;
    snprintf(scan_channel_name, sizeof(scan_channel_name), "%s",
             source_count
                 ? app.source_playlist.channels[scan_current_index].name : "");
    LightLock_Unlock(&app.lock);
    miniiptv_live_tune_get_telemetry(&tune);
    if (top_screen) {
        MiniIptvTelemetrySample log_sample = {0};
        log_sample.channel_name = scan_running ? scan_channel_name
            : (count ? channel_names[selected] : "");
        log_sample.app_state = scan_running && state == LIVE_APP_IDLE
            ? "SCANNING" : live_app_state_label(state);
        log_sample.tune_phase = scan_running
            ? miniiptv_channel_scan_status_label(current_scan_status)
            : miniiptv_live_tune_phase_label(tune.phase);
        log_sample.shadow_state = "";
        log_sample.producer_state = "";
        log_sample.last_error = scan_running ? 0
            : (player_error_code ? (int64_t)(int32_t)player_error_code
                                 : (int64_t)tune.result);
        log_sample.periodic = state == LIVE_APP_LOADING || scan_running;
        miniiptv_telemetry_log_record(osGetTime(), &log_sample);
    }

    page_start = count ? (selected / CHANNELS_PER_PAGE) * CHANNELS_PER_PAGE : 0;
    page_end = page_start + CHANNELS_PER_PAGE;
    if (page_end > count) page_end = count;
    page_count = count
        ? (count + CHANNELS_PER_PAGE - 1u) / CHANNELS_PER_PAGE : 0u;
    page_number = count ? page_start / CHANNELS_PER_PAGE + 1u : 0u;

    if (top_screen) {
        if (state == LIVE_APP_LOADING ||
            (state == LIVE_APP_IDLE && scan_running))
            draw_tuning_static(&pixel, osGetTime());
        else
            Draw_texture(&pixel, UI_INK, 0, 15, 400, 225);

        Draw_texture(&pixel, 0xE0121110u, 0, 15, 400, 31);
        Draw_texture(&pixel, UI_CREAM, 12, 43, 376, 1);
        Draw_c("RETRO TUNER", 12, 23, 11.0f, UI_CREAM);
        Draw_align_c(scan_running ? "SCANNING" :
                         (state == LIVE_APP_LOADING ? "TUNING" :
                          (state == LIVE_APP_ERROR ? "NO SIGNAL" :
                           "CHANNELS")),
                     292, 22, 9.0f,
                     state == LIVE_APP_ERROR ? UI_PINK : UI_MINT,
                     DRAW_X_ALIGN_RIGHT, DRAW_Y_ALIGN_CENTER, 94, 14);

        if (scan_running && state == LIVE_APP_IDLE) {
            unsigned int phase = (unsigned int)((osGetTime() / 130u) % 18u);
            Draw_texture(&pixel, 0xD0121110u, 34, 72, 332, 113);
            Draw_align_c("AUTO TUNING", 0, 80, 13.0f, UI_CREAM,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 18);
            snprintf(line, sizeof(line), "%02lu / %02lu   %.28s",
                     (unsigned long)(scan_current_index + 1),
                     (unsigned long)source_count, scan_channel_name);
            Draw_align_c(line, 34, 111, 12.0f, UI_CREAM,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 332, 28);
            snprintf(line, sizeof(line), "%lu CHANNELS FOUND",
                     (unsigned long)count);
            Draw_align_c(line, 34, 143, 9.5f, UI_MINT,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 332, 12);
            Draw_texture(&pixel, UI_PANEL, 54, 164, 292, 5);
            Draw_texture(&pixel, UI_CREAM, 56 + (float)phase * 16,
                         165, 14, 3);
            Draw_align_c(count ? "A  WATCH A FOUND CHANNEL"
                               : "CHANNELS APPEAR AS THEY ARE FOUND",
                         0, 198, 9.0f, UI_CREAM, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 12);
            Draw_set_refresh_needed(true);
        } else if (state == LIVE_APP_LOADING) {
            uint64_t elapsed = osGetTime() - tuning_started_ms;
            unsigned int phase = (unsigned int)((elapsed / 130u) % 18u);
            Draw_texture(&pixel, 0xD0121110u, 34, 70, 332, 118);
            snprintf(line, sizeof(line), "TUNING  CH %02lu",
                     (unsigned long)(selected + 1u));
            Draw_align_c(line, 0, 78, 14.0f, UI_CREAM,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 22);
            Draw_align_c(count ? channel_names[selected] : "", 34, 105,
                         12.0f, UI_CREAM, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 332, 24);
            format_tune_status(line, sizeof(line), &tune);
            Draw_align_c(line, 0, 137, 9.5f, UI_MINT,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 16);
            Draw_texture(&pixel, UI_PANEL, 54, 161, 292, 5);
            Draw_texture(&pixel, UI_CREAM, 56 + (float)phase * 16,
                         162, 14, 3);
            Draw_align_c("B  CANCEL     L/R  CHANGE", 0, 199, 9.0f,
                         UI_CREAM,
                         DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 10);
            Draw_set_refresh_needed(true);
        } else if (state == LIVE_APP_ERROR) {
            draw_tuning_static(&pixel, osGetTime());
            Draw_texture(&pixel, 0xE0121110u, 45, 78, 310, 91);
            Draw_align_c("NO SIGNAL", 0, 92, 18.0f,
                         UI_PINK, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 20);
            Draw_align_c(count ? channel_names[selected] : "", 45, 122,
                         11.0f, UI_CREAM, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 310, 18);
            if (tune.phase == MINIIPTV_TUNE_PHASE_FAILED) {
                snprintf(line, sizeof(line), "%s  /  %u.%us",
                         miniiptv_live_tune_phase_label(tune.failure_phase),
                         tune.total_elapsed_milliseconds / 1000u,
                         (tune.total_elapsed_milliseconds % 1000u) / 100u);
                Draw_align_c(line, 0, 147, 8.5f, UI_MINT,
                             DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                             400, 12);
            }
            Draw_align_c("A RETRY   B CHANNELS   L/R CHANGE", 0, 198,
                         9.0f, UI_CREAM, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 12);
        } else if (state == LIVE_APP_NO_PLAYLIST) {
            Draw_align_c("NO CHANNEL LIST", 0, 82, 16.0f,
                         UI_PINK, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 20);
            Draw_align_c("ADD /3ds/retrotuner3ds/channels.m3u", 0, 119,
                         9.5f, UI_CREAM, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 18);
        } else if (count) {
            snprintf(line, sizeof(line), "%02lu",
                     (unsigned long)(selected + 1u));
            Draw_c(line, 26, 65, 34.0f, UI_CREAM);
            Draw_c("CHANNEL", 28, 104, 8.5f, UI_MINT);
            Draw_align_c(channel_names[selected], 116, 70, 16.0f,
                         UI_CREAM, DRAW_X_ALIGN_LEFT, DRAW_Y_ALIGN_CENTER,
                         258, 42);
            if (channel_session_state[selected] == CHANNEL_SESSION_PLAYED)
                snprintf(line, sizeof(line), "PLAYED THIS SESSION");
            else if (channel_session_state[selected] ==
                     CHANNEL_SESSION_FAILED)
                snprintf(line, sizeof(line), "FAILED LAST TRY  /  RETRY OK");
            else
                snprintf(line, sizeof(line), "%lu FOUND  /  PAGE %lu OF %lu",
                         (unsigned long)count, (unsigned long)page_number,
                         (unsigned long)page_count);
            Draw_c(line, 118, 118, 9.0f, UI_MINT);
            Draw_texture(&pixel, UI_CREAM, 24, 151, 352, 1);
            Draw_align_c("A WATCH     D-PAD BROWSE     START EXIT", 0, 174,
                         9.5f, UI_CREAM, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 16);
        } else {
            Draw_align_c("NO CHANNELS FOUND", 0, 102, 14.0f,
                         UI_CREAM, DRAW_X_ALIGN_CENTER,
                         DRAW_Y_ALIGN_CENTER, 400, 18);
        }
        Draw_align_c(RETROTUNER_VERSION, 0, 220, 8.0f, UI_MINT,
                     DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 400, 10);
        return;
    }

    Draw_texture(&pixel, UI_INK, 0, 0, 320, 225);
    Draw_texture(&pixel, UI_CREAM, 12, 12, 296, 1);

    if (state == LIVE_APP_LOADING && switching_from_player && count > 0) {
        uint64_t elapsed = osGetTime() - tuning_started_ms;
        unsigned int phase = (unsigned int)((elapsed / 120u) % 18u);
        Draw_c("TUNING", 14, 22, 14.0f, UI_CREAM);
        snprintf(line, sizeof(line), "CH %02lu",
                 (unsigned long)(switch_to_index + 1));
        Draw_c(line, 14, 54, 26.0f, UI_CREAM);
        Draw_align_c(channel_names[switch_to_index], 83, 52, 13.0f,
                     UI_CREAM, DRAW_X_ALIGN_LEFT, DRAW_Y_ALIGN_CENTER,
                     222, 32);
        snprintf(line, sizeof(line), "FROM  CH %02lu  %.28s",
                 (unsigned long)(switch_from_index + 1),
                 channel_names[switch_from_index]);
        Draw_c(line, 15, 93, 9.0f, UI_MINT);
        Draw_texture(&pixel, UI_PANEL, 16, 124, 288, 5);
        Draw_texture(&pixel, UI_CREAM, 18 + (float)phase * 15,
                     125, 13, 3);
        format_tune_status(line, sizeof(line), &tune);
        Draw_align_c(line, 12, 147, 10.0f, UI_MINT,
                     DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 296, 18);
        Draw_texture(&pixel, UI_CREAM, 12, 192, 296, 1);
        Draw_align_c("B CANCEL        L/R CHANGE", 12, 204, 9.5f,
                     UI_CREAM, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                     296, 14);
        Draw_set_refresh_needed(true);
        return;
    }

    Draw_c(scan_running ? "AUTO TUNING" : "CHANNELS",
           14, 20, 13.0f, UI_CREAM);
    snprintf(line, sizeof(line), "PAGE %lu OF %lu",
             (unsigned long)page_number, (unsigned long)page_count);
    Draw_align_c(line, 216, 20, 9.5f, UI_MINT,
                 DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 92, 15);
    if (count) {
        snprintf(line, sizeof(line), "%lu FOUND", (unsigned long)count);
        Draw_c(line, 14, 35, 8.5f, UI_MINT);
    }

    if (count == 0 && scan_running)
        Draw_align_c("SEARCHING... CHANNELS APPEAR HERE", 10, 91, 11.0f,
                     UI_MINT, DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER,
                     300, 30);

    for (i = page_start; i < page_end; i++) {
        float y = 50 + (float)(i - page_start) * 12;
        if (i == selected)
            Draw_texture(&pixel, UI_CREAM, 10, y, 300, 11);
        snprintf(line, sizeof(line), "%c %02lu  %.35s",
                 channel_session_state[i] == CHANNEL_SESSION_PLAYED ? '+' :
                 (channel_session_state[i] == CHANNEL_SESSION_FAILED ? '!' :
                  (channel_scan_status[i] == MINIIPTV_SCAN_READY ? '*' : '?')),
                 (unsigned long)(i + 1), channel_names[i]);
        Draw_c(line, 16, y, 9.5f,
               i == selected ? UI_INK : UI_CREAM);
    }

    Draw_texture(&pixel, UI_CREAM, 12, 176, 296, 1);
    if (state == LIVE_APP_LOADING)
        format_tune_status(line, sizeof(line), &tune);
    else if (state == LIVE_APP_ERROR)
        snprintf(line, sizeof(line), "NO SIGNAL // RETRY OR PICK ANOTHER");
    else if (count && page_count > 1u && state == LIVE_APP_IDLE)
        snprintf(line, sizeof(line),
                 "+ PLAYED   ! FAILED   * VERIFIED   ? CHECK");
    else
        snprintf(line, sizeof(line), "%.111s", status);
    Draw_align_c(line, 14, 183, 8.5f,
                 state == LIVE_APP_ERROR || state == LIVE_APP_NO_PLAYLIST
                     ? UI_PINK : UI_MINT,
                 DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 292, 14);
    if (state == LIVE_APP_LOADING)
        snprintf(line, sizeof(line), "B CANCEL   L/R CHANGE");
    else if (state == LIVE_APP_ERROR)
        snprintf(line, sizeof(line), "A RETRY   B CHANNELS   L/R CHANGE");
    else if (count)
        snprintf(line, sizeof(line), "A WATCH   D-PAD BROWSE   START EXIT");
    else
        snprintf(line, sizeof(line), "START EXIT");
    Draw_align_c(line, 8, 207, 9.0f, UI_CREAM,
                 DRAW_X_ALIGN_CENTER, DRAW_Y_ALIGN_CENTER, 304, 12);
}

void MiniIptv_live_app_init(void) {
    int telemetry_result;
    memset(&app, 0, sizeof(app));
    LightLock_Init(&app.lock);
    miniiptv_live_tune_telemetry_init();
    (void)miniiptv_telemetry_log_rotate(TELEMETRY_LOG,
                                        PREVIOUS_TELEMETRY_LOG);
    telemetry_result = miniiptv_telemetry_log_open(
        TELEMETRY_LOG, RETROTUNER_VERSION, osGetTime());

	if (playlist_load_file(USER_PLAYLIST, &app.source_playlist) != 0 ||
        app.source_playlist.count == 0) {
		set_status_locked(LIVE_APP_NO_PLAYLIST,
						  "ADD /3ds/retrotuner3ds/channels.m3u");
    } else {
        set_status_locked(LIVE_APP_IDLE,
            telemetry_result == 0
                ? "SCANNING AIRWAVES // TELEMETRY LOG ON"
                : "SCANNING AIRWAVES // LOG OFF (SD WRITE FAILED)");
    }

    Vid_set_idle_hooks(live_hid, live_draw);
    Vid_set_live_error_hook(player_error);
    Vid_set_live_channel_hook(player_channel_request);
    Vid_set_live_drawer_hooks(live_drawer_hid, live_drawer_draw);
    launch_scan_worker();
}

void MiniIptv_live_app_exit(void) {
    Thread worker = NULL;
    Thread scan_worker = NULL;
    bool network_ready;

    LightLock_Lock(&app.lock);
    app.exit_requested = true;
    app.scan_stop_requested = true;
    app.pending_channel_step = 0;
    app.pending_channel_target_valid = false;
    app.switching_from_player = false;
    worker = app.worker;
    app.worker = NULL;
    scan_worker = app.scan_worker;
    app.scan_worker = NULL;
    LightLock_Unlock(&app.lock);

    Vid_set_live_error_hook(NULL);
    Vid_set_live_channel_hook(NULL);
    Vid_set_live_drawer_hooks(NULL, NULL);
    Vid_set_idle_hooks(NULL, NULL);
    miniiptv_live_stream_request_stop();
    if (scan_worker) {
        threadJoin(scan_worker, UINT64_MAX);
        threadFree(scan_worker);
    }
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

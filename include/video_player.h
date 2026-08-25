#if !defined(DEF_VIDEO_PLAYER_HPP)
#define DEF_VIDEO_PLAYER_HPP
#include <stdbool.h>
#include <stdint.h>
#include "miniiptv/version.h"
#include "system/util/hid_types.h"

#define DEF_VID_ENABLE

#define DEF_VID_ENABLE_ICON
//#define DEF_VID_ENABLE_NAME
#define DEF_VID_ICON_PATH				/*(const char*)(*/"romfs:/gfx/draw/icon/vid_icon.t3x"/*)*/
#define DEF_VID_NAME					/*(const char*)(*/"Video\nplayer"/*)*/
#define DEF_VID_VER						/*(const char*)(*/"RetroTuner3DS Pixel Deck " RETROTUNER_VERSION/*)*/
#define DEF_VID_SPEAKER_SESSION_ID		(uint8_t)(0)
#define DEF_VID_DECORDER_SESSION_ID		(uint8_t)(0)

typedef bool (*Vid_idle_hid_hook)(const Hid_info* key);
typedef void (*Vid_idle_draw_hook)(bool top_screen, uint32_t color, uint32_t back_color);
typedef void (*Vid_init_draw_hook)(void);
typedef void (*Vid_live_error_hook)(uint32_t error_code);
typedef void (*Vid_live_channel_hook)(int direction);

typedef enum {
	VID_LIVE_DRAWER_HANDLED = 0,
	VID_LIVE_DRAWER_CLOSE,
	VID_LIVE_DRAWER_TUNE
} Vid_live_drawer_result;

typedef Vid_live_drawer_result (*Vid_live_drawer_hid_hook)(
	const Hid_info* key);
typedef void (*Vid_live_drawer_draw_hook)(uint32_t color,
	uint32_t back_color);

typedef enum {
	VID_LIVE_AUDIO_SCANNING = 0,
	VID_LIVE_AUDIO_NONE,
	VID_LIVE_AUDIO_DEMUXED,
	VID_LIVE_AUDIO_READY,
	VID_LIVE_AUDIO_INIT_FAILED,
	VID_LIVE_AUDIO_DECODE_FAILED,
	VID_LIVE_AUDIO_CONVERT_FAILED,
	VID_LIVE_AUDIO_OUTPUT_FAILED
} Vid_live_audio_state;

typedef struct {
	uint32_t video_packets;
	uint32_t decoded_frames;
	uint32_t textures;
	uint32_t presented_frames;
	uint32_t audio_demux_packets;
	uint32_t audio_frames;
	uint32_t audio_buffers;
	uint32_t audio_last_error;
	uint8_t audio_tracks;
	Vid_live_audio_state audio_state;
	bool presented;
} Vid_live_diagnostics;

bool Vid_query_init_flag(void);

bool Vid_query_running_flag(void);

void Vid_hid(const Hid_info* key);

void Vid_resume(void);

void Vid_suspend(void);

uint32_t Vid_load_msg(const char* lang);

void Vid_init(bool draw);

void Vid_exit(bool draw);

void Vid_enable_standalone_mode(void);

//Keep a standalone splash refreshed while the inherited decoder initializes.
void Vid_set_init_draw_hook(Vid_init_draw_hook draw_hook);

void Vid_set_idle_hooks(Vid_idle_hid_hook hid_hook, Vid_idle_draw_hook draw_hook);

//Route live-playback failures back to the RetroTuner3DS channel deck.
void Vid_set_live_error_hook(Vid_live_error_hook error_hook);

//Request a clean live-player handoff (-1 previous, +1 next, 0 channel deck).
void Vid_set_live_channel_hook(Vid_live_channel_hook channel_hook);

//Keep playback active while RetroTuner's bottom-screen channel drawer is open.
void Vid_set_live_drawer_hooks(Vid_live_drawer_hid_hook hid_hook,
	Vid_live_drawer_draw_hook draw_hook);

//Prepare a bounded file for the existing player while it is idle.
bool Vid_prepare_file(const char* directory, const char* name);

//Prepare a bounded file and explicitly enqueue playback in one operation.
bool Vid_prepare_and_start_file(const char* directory, const char* name);

bool Vid_query_idle_flag(void);

//Incremented whenever an active playback session returns to the idle screen.
uint32_t Vid_query_playback_return_generation(void);

//Snapshot the current live startup/audio pipeline without exposing stream URLs.
void Vid_query_live_diagnostics(Vid_live_diagnostics* diagnostics);

//True after START is pressed while the embedded test is active.
bool Vid_query_embedded_exit_requested(void);

void Vid_main(void);

#endif //!defined(DEF_VIDEO_PLAYER_HPP)

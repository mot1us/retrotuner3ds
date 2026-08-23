#if !defined(DEF_VIDEO_PLAYER_HPP)
#define DEF_VIDEO_PLAYER_HPP
#include <stdbool.h>
#include <stdint.h>
#include "system/util/hid_types.h"

#define DEF_VID_ENABLE

#define DEF_VID_ENABLE_ICON
//#define DEF_VID_ENABLE_NAME
#define DEF_VID_ICON_PATH				/*(const char*)(*/"romfs:/gfx/draw/icon/vid_icon.t3x"/*)*/
#define DEF_VID_NAME					/*(const char*)(*/"Video\nplayer"/*)*/
#define DEF_VID_VER						/*(const char*)(*/"RetroTuner3DS Pixel Deck 0.5.1-rc5"/*)*/
#define DEF_VID_SPEAKER_SESSION_ID		(uint8_t)(0)
#define DEF_VID_DECORDER_SESSION_ID		(uint8_t)(0)

typedef bool (*Vid_idle_hid_hook)(const Hid_info* key);
typedef void (*Vid_idle_draw_hook)(bool top_screen, uint32_t color, uint32_t back_color);
typedef void (*Vid_live_error_hook)(uint32_t error_code);
typedef void (*Vid_live_channel_hook)(int direction);

bool Vid_query_init_flag(void);

bool Vid_query_running_flag(void);

void Vid_hid(const Hid_info* key);

void Vid_resume(void);

void Vid_suspend(void);

uint32_t Vid_load_msg(const char* lang);

void Vid_init(bool draw);

void Vid_exit(bool draw);

//Configure the standalone RetroTuner3DS player.
void Vid_prepare_embedded_test(void);

void Vid_enable_standalone_mode(void);

void Vid_set_idle_hooks(Vid_idle_hid_hook hid_hook, Vid_idle_draw_hook draw_hook);

//Route live-playback failures back to the RetroTuner3DS channel deck.
void Vid_set_live_error_hook(Vid_live_error_hook error_hook);

//Request the previous (-1) or next (+1) playlist entry during live playback.
void Vid_set_live_channel_hook(Vid_live_channel_hook channel_hook);

//Prepare a bounded file for the existing player while it is idle.
bool Vid_prepare_file(const char* directory, const char* name);

//Prepare a bounded file and explicitly enqueue playback in one operation.
bool Vid_prepare_and_start_file(const char* directory, const char* name);

bool Vid_query_idle_flag(void);

//Incremented whenever an active playback session returns to the idle screen.
uint32_t Vid_query_playback_return_generation(void);

//True after START is pressed while the embedded test is active.
bool Vid_query_embedded_exit_requested(void);

void Vid_main(void);

#endif //!defined(DEF_VIDEO_PLAYER_HPP)

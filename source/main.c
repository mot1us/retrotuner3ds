//Includes.
#include <stdbool.h>
#include <stdint.h>

#include "3ds.h"

#include "system/menu.h"
#include "system/sem.h"
#include "video_player.h"
#include "miniiptv/live_app.h"
#include "miniiptv/theme_ui.h"

//Defines.
//N/A.

//Typedefs.
//N/A.

//Prototypes.
//N/A.

//Variables.
//N/A.

//Code.
int main(void)
{
	uint64_t boot_animation_ends_ms;
	bool previous_sleep_allowed;

	MiniIptv_theme_ui_init("sdmc:/3ds/retrotuner3ds/theme.cfg");

	/* RetroTuner owns its always-awake playback policy. The inherited settings
	 * service remains available for battery/Wi-Fi status only. */
	Sem_set_display_power_management_enabled(false);
	Menu_init();
	previous_sleep_allowed = aptIsSleepAllowed();
	aptSetSleepAllowed(false);
	/* Select standalone behavior before player initialization so the inherited
	 * player can skip UI/settings work RetroTuner never exposes. */
	Vid_enable_standalone_mode();
	Vid_set_init_draw_hook(MiniIptv_live_app_draw_boot_screen);
	MiniIptv_live_app_reset_boot_screen();
	MiniIptv_live_app_draw_boot_screen();
	/* Keep the RetroTuner splash visible while the synchronous player init
	 * runs; the inherited init renderer would otherwise repaint both screens. */
	Vid_init(false);
	Vid_set_init_draw_hook(NULL);
	if(!Vid_query_init_flag())
	{
		/* Do not run normal destructors if a timed-out init worker can still
		 * reference their services. Process termination is the only safe owner
		 * boundary in that exceptional state. */
		if(!Vid_query_cleanup_safe())
			svcExitProcess();
		aptSetSleepAllowed(previous_sleep_allowed);
		Menu_exit();
		return 1;
	}
	/* Player initialization may invoke its draw hook only once on fast boots.
	 * Give the code-drawn television aperture one deliberate, bounded pass so
	 * it reads as an animation instead of a single horizontal flash. */
	MiniIptv_live_app_reset_boot_screen();
	boot_animation_ends_ms = osGetTime() + 1700u;
	while(osGetTime() < boot_animation_ends_ms)
	{
		MiniIptv_live_app_draw_boot_screen();
		svcSleepThread(16000000LL);
	}
	MiniIptv_live_app_init();

	// Standalone bounded live test using the proven playback pipeline.
	while (aptMainLoop())
	{
		if (Menu_query_must_exit_flag() || Vid_query_embedded_exit_requested())
			break;

		Menu_main();
	}

	MiniIptv_live_app_exit();
	if(!Vid_query_cleanup_safe())
		svcExitProcess();
	aptSetSleepAllowed(previous_sleep_allowed);
	Menu_exit();
	return 0;
}

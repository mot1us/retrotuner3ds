//Includes.
#include <stdbool.h>
#include <stdint.h>

#include "3ds.h"

#include "system/menu.h"
#include "video_player.h"
#include "miniiptv/live_app.h"

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

	Menu_init();
	Vid_set_init_draw_hook(MiniIptv_live_app_draw_boot_screen);
	MiniIptv_live_app_reset_boot_screen();
	MiniIptv_live_app_draw_boot_screen();
	/* Keep the RetroTuner splash visible while the synchronous player init
	 * runs; the inherited init renderer would otherwise repaint both screens. */
	Vid_init(false);
	Vid_set_init_draw_hook(NULL);
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
	Vid_enable_standalone_mode();
	MiniIptv_live_app_init();

	// Standalone bounded live test using the proven playback pipeline.
	while (aptMainLoop())
	{
		if (Menu_query_must_exit_flag() || Vid_query_embedded_exit_requested())
			break;

		Menu_main();
	}

	MiniIptv_live_app_exit();
	Menu_exit();
	return 0;
}

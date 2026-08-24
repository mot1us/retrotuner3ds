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
	Menu_init();
	Vid_set_init_draw_hook(MiniIptv_live_app_draw_boot_screen);
	MiniIptv_live_app_draw_boot_screen();
	/* Keep the RetroTuner splash visible while the synchronous player init
	 * runs; the inherited init renderer would otherwise repaint both screens. */
	Vid_init(false);
	Vid_set_init_draw_hook(NULL);
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

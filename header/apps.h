#pragma once
#include <stdint.h>
#include "header/fat32.h"

// Each app owns one window. The open functions are what the desktop icons
// call; each allocates its own state, hands it to wm_open() as the window's
// user pointer, and frees it again on WM_EV_CLOSE.
void app_taskman_open(void);
void app_files_open(void);
void app_term_open(void);
void app_about_open(void);
void app_browser_open(void);
void app_settings_open(void);

// A blank editor window, and an editor on an existing file. The second takes
// the folder the file is in (a cluster number) and its directory entry, and
// returns 0, or a negative code: -1 too large for the editor, -2 not a text
// file, -3 could not be read.
void app_edit_open(void);
int  app_edit_open_file(uint32_t dir, const fat32_dirent_t* d);

// Rebuilds every open page. A laid-out page carries the colours it was laid
// out with -- the display list stores a colour per run, which is what makes
// painting a loop with no style lookups in it -- so changing the system
// appearance has to run the layout again rather than just repainting.
void app_browser_relayout(void);

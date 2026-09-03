#pragma once

// Each app owns one window. The open functions are what the desktop icons
// call; each allocates its own state, hands it to wm_open() as the window's
// user pointer, and frees it again on WM_EV_CLOSE.
void app_taskman_open(void);
void app_files_open(void);
void app_term_open(void);
void app_about_open(void);
void app_browser_open(void);

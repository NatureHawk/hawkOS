#pragma once
#include <stdint.h>

// Preferences that survive a reboot. They live in a small text file at the root
// of the disk (HAWKOS.CFG, one `key=value` per line), which keeps them
// readable and editable from the outside, and means a corrupt or missing file
// costs nothing worse than falling back to the defaults.

typedef struct {
    int dark;          // 1 = dark appearance
    int wallpaper;     // index into the theme's wallpapers
    int clock24;       // 1 = 24-hour clock in the menu bar, 0 = 12-hour
    int dock_zoom;     // 1 = icons grow under the pointer
} settings_t;

extern settings_t settings;

// Reads the file if there is one and applies whatever it says. Safe to call
// with no disk: the defaults stay in force.
void settings_load(void);

// Applies `settings` to the running system (appearance, wallpaper).
void settings_apply(void);

// Writes `settings` to disk. Returns 0 on success, -1 if the disk is missing
// or read-only -- the change still takes effect for this session.
int  settings_save(void);

// Applies and saves in one step -- what a control in the Settings app or a menu
// item calls after changing a field. Also rebuilds any open web page, which
// carries the colours it was laid out with.
void settings_commit(void);

// 1 when the last settings_load/save reached the disk, for the Settings app
// to say whether changes will be remembered.
int  settings_persistent(void);

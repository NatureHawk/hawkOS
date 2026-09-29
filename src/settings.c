// src/settings.c — saved preferences
#include <stdint.h>
#include "header/settings.h"
#include "header/theme.h"
#include "header/fat32.h"
#include "header/shell.h"
#include "header/kstring.h"
#include "header/apps.h"

#define CFG_NAME "HAWKOS.CFG"
#define CFG_MAX  512

settings_t settings = { 0, 0, 1, 1 };

static int persistent = 0;

void settings_apply(void){
    theme_set_dark(settings.dark);
    theme_set_wallpaper(settings.wallpaper);
}

// Parses `key=value` lines. Unknown keys and malformed lines are skipped, so a
// file written by a newer build is still readable by an older one.
static void parse(const char* buf, uint32_t n){
    uint32_t i = 0;
    while (i < n){
        uint32_t ls = i;
        while (i < n && buf[i] != '\n') i++;
        uint32_t le = i;
        if (i < n) i++;

        const char* eq = 0;
        for (uint32_t k = ls; k < le; k++) if (buf[k] == '='){ eq = buf + k; break; }
        if (!eq) continue;

        uint32_t klen = (uint32_t)(eq - (buf + ls));
        int val = 0, any = 0;
        for (const char* p = eq + 1; p < buf + le; p++){
            if (*p >= '0' && *p <= '9'){ val = val * 10 + (*p - '0'); any = 1; }
            else if (*p != '\r' && *p != ' ') { any = 0; break; }
        }
        if (!any) continue;

        const char* k = buf + ls;
        if      (klen == 4  && strncmp(k, "dark", 4) == 0)       settings.dark      = val ? 1 : 0;
        else if (klen == 9  && strncmp(k, "wallpaper", 9) == 0)  settings.wallpaper =
                    (val >= 0 && val < theme_wallpaper_count()) ? val : 0;
        else if (klen == 7  && strncmp(k, "clock24", 7) == 0)    settings.clock24   = val ? 1 : 0;
        else if (klen == 9  && strncmp(k, "dock_zoom", 9) == 0)  settings.dock_zoom = val ? 1 : 0;
    }
}

void settings_load(void){
    shell_init();
    if (!shell_fs_ready()){ persistent = 0; return; }

    fat32_dirent_t e;
    if (fat32_find(fat32_root_cluster(), CFG_NAME, &e) == 0 && !e.is_dir){
        char buf[CFG_MAX];
        uint32_t cap = e.size < CFG_MAX ? e.size : CFG_MAX;
        uint32_t got = fat32_read_file(&e, (uint8_t*)buf, cap);
        parse(buf, got);
    }
    persistent = fat32_writable();
}

int settings_save(void){
    if (!shell_fs_ready() || !fat32_writable()){ persistent = 0; return -1; }

    char buf[CFG_MAX];
    int n = ksnprintf(buf, sizeof(buf),
                      "dark=%d\nwallpaper=%d\nclock24=%d\ndock_zoom=%d\n",
                      settings.dark, settings.wallpaper, settings.clock24, settings.dock_zoom);
    if (n < 0) return -1;
    persistent = (fat32_write_file(fat32_root_cluster(), CFG_NAME, (const uint8_t*)buf,
                                   (uint32_t)n) == 0);
    return persistent ? 0 : -1;
}

void settings_commit(void){
    settings_apply();
    app_browser_relayout();
    settings_save();
}

int settings_persistent(void){ return persistent; }

// src/clipboard.c — the system clipboard
#include <stdint.h>
#include "header/clipboard.h"
#include "header/kstring.h"

static char     text[CLIP_TEXT_MAX + 1];
static uint32_t text_len = 0;

static char     fnames[CLIP_FILES_MAX][CLIP_NAME_MAX];
static int      fcount = 0;
static uint32_t fdir = 0;
static int      fcut = 0;

void clip_set_text(const char* s, uint32_t n){
    if (n > CLIP_TEXT_MAX) n = CLIP_TEXT_MAX;
    if (s && n) memcpy(text, s, n);
    text_len = s ? n : 0;
    text[text_len] = 0;
}

const char* clip_text(uint32_t* n){
    if (n) *n = text_len;
    return text;
}

uint32_t clip_text_len(void){ return text_len; }

void clip_set_files(uint32_t dir, const char names[][CLIP_NAME_MAX], int n, int cut){
    if (n > CLIP_FILES_MAX) n = CLIP_FILES_MAX;
    for (int i = 0; i < n; i++){
        strncpy(fnames[i], names[i], CLIP_NAME_MAX - 1);
        fnames[i][CLIP_NAME_MAX - 1] = 0;
    }
    fcount = n;
    fdir   = dir;
    fcut   = cut ? 1 : 0;
}

int         clip_file_count(void){ return fcount; }
uint32_t    clip_files_dir(void){ return fdir; }
const char* clip_file_name(int i){ return (i >= 0 && i < fcount) ? fnames[i] : ""; }
int         clip_files_cut(void){ return fcut; }
void        clip_files_clear(void){ fcount = 0; fcut = 0; }

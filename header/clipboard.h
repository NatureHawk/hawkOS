#pragma once
#include <stdint.h>

// The system clipboard. One shared slot, like every desktop has, in two kinds:
// text (Ctrl+C in the editor, terminal, address bar) and a list of files
// (Copy / Cut in the Files app). They are kept apart because they are pasted
// by different code and a copy of one should not destroy the other -- copying
// a sentence while a cut is pending must not cancel the cut.

#define CLIP_TEXT_MAX   (64u * 1024u)
#define CLIP_FILES_MAX  64
#define CLIP_NAME_MAX   128

// ---- text
// Replaces the clipboard text with n bytes of s (truncated to CLIP_TEXT_MAX).
void        clip_set_text(const char* s, uint32_t n);
// The current text, NUL-terminated, valid until the next clip_set_text. *n is
// its length; an empty clipboard returns "" with *n == 0.
const char* clip_text(uint32_t* n);
uint32_t    clip_text_len(void);

// ---- files
// Records `n` names from directory `dir` (a cluster number). `cut` says the
// paste should move them rather than copy them. Names beyond CLIP_FILES_MAX
// are dropped.
void        clip_set_files(uint32_t dir, const char names[][CLIP_NAME_MAX], int n, int cut);
int         clip_file_count(void);
uint32_t    clip_files_dir(void);
const char* clip_file_name(int i);
int         clip_files_cut(void);
void        clip_files_clear(void);

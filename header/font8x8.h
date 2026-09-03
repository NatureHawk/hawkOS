#pragma once
#include <stdint.h>

#define FONT_GLYPH_W 5
#define FONT_GLYPH_H 7

// Fills rows[0..6] (top to bottom); each byte's bits 4..0 are columns
// left..right. Lowercase letters are folded to uppercase — this is a v1
// glyph set (A-Z, 0-9, space, and the punctuation kprintf strings actually
// use), not a full ASCII table. Anything unmapped renders as a hollow box
// rather than being silently dropped.
void font_glyph(char c, uint8_t rows[FONT_GLYPH_H]);

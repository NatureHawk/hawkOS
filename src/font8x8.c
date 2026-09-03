// src/font8x8.c — minimal 5x7 bitmap font (see header/font8x8.h for scope)
#include "header/font8x8.h"

static const uint8_t DEFAULT_GLYPH[FONT_GLYPH_H] = {0x1F,0x11,0x11,0x11,0x11,0x11,0x1F};

void font_glyph(char c, uint8_t rows[FONT_GLYPH_H]){
    switch (c) {
        case ' ': { static const uint8_t g[7]={0,0,0,0,0,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '0': { static const uint8_t g[7]={0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '1': { static const uint8_t g[7]={0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '2': { static const uint8_t g[7]={0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '3': { static const uint8_t g[7]={0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '4': { static const uint8_t g[7]={0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '5': { static const uint8_t g[7]={0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '6': { static const uint8_t g[7]={0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '7': { static const uint8_t g[7]={0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '8': { static const uint8_t g[7]={0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '9': { static const uint8_t g[7]={0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }

        case 'A': { static const uint8_t g[7]={0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'B': { static const uint8_t g[7]={0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'C': { static const uint8_t g[7]={0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'D': { static const uint8_t g[7]={0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'E': { static const uint8_t g[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'F': { static const uint8_t g[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'G': { static const uint8_t g[7]={0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'H': { static const uint8_t g[7]={0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'I': { static const uint8_t g[7]={0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'J': { static const uint8_t g[7]={0x07,0x02,0x02,0x02,0x02,0x12,0x0C}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'K': { static const uint8_t g[7]={0x11,0x12,0x14,0x18,0x14,0x12,0x11}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'L': { static const uint8_t g[7]={0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'M': { static const uint8_t g[7]={0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'N': { static const uint8_t g[7]={0x11,0x19,0x15,0x15,0x13,0x11,0x11}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'O': { static const uint8_t g[7]={0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'P': { static const uint8_t g[7]={0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'Q': { static const uint8_t g[7]={0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'R': { static const uint8_t g[7]={0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'S': { static const uint8_t g[7]={0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'T': { static const uint8_t g[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'U': { static const uint8_t g[7]={0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'V': { static const uint8_t g[7]={0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'W': { static const uint8_t g[7]={0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'X': { static const uint8_t g[7]={0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'Y': { static const uint8_t g[7]={0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'Z': { static const uint8_t g[7]={0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }

        case 'a': { static const uint8_t g[7]={0x00,0x0E,0x01,0x0F,0x11,0x0F,0x00}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'b': { static const uint8_t g[7]={0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'c': { static const uint8_t g[7]={0x00,0x00,0x0F,0x10,0x10,0x10,0x0F}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'd': { static const uint8_t g[7]={0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'e': { static const uint8_t g[7]={0x00,0x0E,0x11,0x1F,0x10,0x10,0x0F}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'f': { static const uint8_t g[7]={0x06,0x09,0x08,0x1E,0x08,0x08,0x08}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'g': { static const uint8_t g[7]={0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'h': { static const uint8_t g[7]={0x10,0x10,0x16,0x19,0x11,0x11,0x11}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'i': { static const uint8_t g[7]={0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'j': { static const uint8_t g[7]={0x02,0x00,0x06,0x02,0x02,0x12,0x0C}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'k': { static const uint8_t g[7]={0x10,0x10,0x12,0x14,0x18,0x14,0x12}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'l': { static const uint8_t g[7]={0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'm': { static const uint8_t g[7]={0x00,0x00,0x1A,0x15,0x15,0x15,0x15}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'n': { static const uint8_t g[7]={0x00,0x00,0x16,0x19,0x11,0x11,0x11}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'o': { static const uint8_t g[7]={0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'p': { static const uint8_t g[7]={0x00,0x00,0x1E,0x11,0x11,0x1E,0x10}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'q': { static const uint8_t g[7]={0x00,0x00,0x0F,0x11,0x11,0x0F,0x01}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'r': { static const uint8_t g[7]={0x00,0x00,0x16,0x19,0x10,0x10,0x10}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 's': { static const uint8_t g[7]={0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 't': { static const uint8_t g[7]={0x08,0x08,0x1E,0x08,0x08,0x09,0x06}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'u': { static const uint8_t g[7]={0x00,0x00,0x11,0x11,0x11,0x13,0x0D}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'v': { static const uint8_t g[7]={0x00,0x00,0x11,0x11,0x11,0x0A,0x04}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'w': { static const uint8_t g[7]={0x00,0x00,0x11,0x15,0x15,0x15,0x0A}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'x': { static const uint8_t g[7]={0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'y': { static const uint8_t g[7]={0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case 'z': { static const uint8_t g[7]={0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }

        case '.': { static const uint8_t g[7]={0,0,0,0,0,0,0x04}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case ',': { static const uint8_t g[7]={0,0,0,0,0,0x04,0x08}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case ':': { static const uint8_t g[7]={0,0x04,0,0,0x04,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '-': { static const uint8_t g[7]={0,0,0,0x1F,0,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '_': { static const uint8_t g[7]={0,0,0,0,0,0,0x1F}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '/': { static const uint8_t g[7]={0x01,0x02,0x02,0x04,0x08,0x08,0x10}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '\\':{ static const uint8_t g[7]={0x10,0x08,0x08,0x04,0x02,0x02,0x01}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '(': { static const uint8_t g[7]={0x02,0x04,0x08,0x08,0x08,0x04,0x02}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case ')': { static const uint8_t g[7]={0x08,0x04,0x02,0x02,0x02,0x04,0x08}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '[': { static const uint8_t g[7]={0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case ']': { static const uint8_t g[7]={0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '<': { static const uint8_t g[7]={0x01,0x02,0x04,0x08,0x04,0x02,0x01}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '>': { static const uint8_t g[7]={0x10,0x08,0x04,0x02,0x04,0x08,0x10}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '=': { static const uint8_t g[7]={0,0,0x1F,0,0x1F,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '+': { static const uint8_t g[7]={0,0x04,0x04,0x1F,0x04,0x04,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '*': { static const uint8_t g[7]={0x15,0x0E,0x1F,0x0E,0x15,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '#': { static const uint8_t g[7]={0x0A,0x1F,0x0A,0x1F,0x0A,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '%': { static const uint8_t g[7]={0x19,0x1A,0x04,0x0B,0x13,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '\'':{ static const uint8_t g[7]={0x08,0x04,0,0,0,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '"': { static const uint8_t g[7]={0x0A,0x0A,0,0,0,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '!': { static const uint8_t g[7]={0x04,0x04,0x04,0x04,0x04,0,0x04}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '?': { static const uint8_t g[7]={0x0E,0x11,0x02,0x04,0x04,0,0x04}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '|': { static const uint8_t g[7]={0x04,0x04,0x04,0x04,0x04,0x04,0x04}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '@': { static const uint8_t g[7]={0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '~': { static const uint8_t g[7]={0,0,0x08,0x15,0x02,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '^': { static const uint8_t g[7]={0x04,0x0A,0x11,0,0,0,0}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case '&': { static const uint8_t g[7]={0x08,0x14,0x08,0x15,0x12,0x12,0x0D}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }
        case ';': { static const uint8_t g[7]={0,0x04,0,0,0x04,0x04,0x08}; for(int i=0;i<7;i++) rows[i]=g[i]; return; }

        default:
            for (int i = 0; i < FONT_GLYPH_H; i++) rows[i] = DEFAULT_GLYPH[i];
            return;
    }
}

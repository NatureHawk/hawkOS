#pragma once
#include <stdint.h>

// A single-line text field with a caret, a selection and the clipboard
// shortcuts. The Files dialogs and the editor's Save As bar are built on it
// so that every place you can type a name behaves the same way -- shift+arrows
// select, Ctrl+A/C/X/V do what they do everywhere else -- instead of each app
// growing its own half-working version of "backspace and append".
//
// The text is drawn in the 8x16 console font, so a column is a character and
// converting a pixel position to a caret position is a division.

#define LE_MAX 128

typedef struct {
    char text[LE_MAX];
    int  len;
    int  cur;         // caret, 0..len
    int  anc;         // the other end of the selection; == cur when nothing is selected
    int  scroll;      // first visible column
} lineedit_t;

void le_set(lineedit_t* le, const char* s, int select_all);
int  le_has_selection(const lineedit_t* le);

// Handles a key. Returns 1 if the field consumed it. Enter, Escape and Tab
// are never consumed: what they mean belongs to whoever owns the field.
int  le_key(lineedit_t* le, int key);

// Pointer input, with px measured from the left edge of the text area.
void le_click(lineedit_t* le, int px, int extend);
void le_drag(lineedit_t* le, int px);

// Draws the text, its selection and (if `focused`) the caret inside the
// w x h box at x,y. It does not draw the box itself.
void le_draw(lineedit_t* le, int x, int y, int w, int h, int focused);

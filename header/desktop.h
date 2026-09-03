#pragma once
// Draws a static desktop scene (taskbar + a couple of window boxes) and
// tracks the mouse cursor until ESC is pressed, then hands the framebuffer
// back to the text console.
void desktop_run(void);

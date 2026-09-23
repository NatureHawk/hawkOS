#pragma once
// Runs the desktop shell -- wallpaper, menu bar, dock and the window manager
// on top of them -- until the last of it is quit, then hands the framebuffer
// back to the text console.
void desktop_run(void);

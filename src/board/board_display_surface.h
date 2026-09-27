#pragma once

// Called once after the board acquires the DPI panel's own RGB565 framebuffer.
// Boot drawing and emulation then share this surface and its publication path.
bool BoardDisplay_AttachSurface(void *framebuffer, int panel_width, int panel_height);

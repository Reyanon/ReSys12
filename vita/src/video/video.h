// Video output layer (vitaGL). The emulated GPU writes straight into texture memory, the main thread presents.
#ifndef RS12_VIDEO_H
#define RS12_VIDEO_H

#include <stdint.h>

// Texture size of one output frame (System 12 display is at most 640x480)
#define RS12_VIDEO_TEX_W 1024
#define RS12_VIDEO_TEX_H 512

void rs12_video_init(void);
void rs12_video_shutdown(void);

// Start a frame of w x h pixels: returns the texture pixels (ABGR8888, stride RS12_VIDEO_TEX_W) not used by the GPU
uint32_t *rs12_video_frame_begin(int w, int h);
// Same for 15-bit PS1 VRAM pixels (A1B5G5R5, R in the low bits; stride RS12_VIDEO_TEX_W): no colour conversion needed
uint16_t *rs12_video_frame_begin16(int w, int h);
// The frame being written holds one field of a 480-line picture (0 even lines, 1 odd), shown alone
void rs12_video_frame_field(int odd);
// Frame finished: becomes the frame shown at the next present
void rs12_video_frame_end(void);
// gun aim of player 0/1 at a fraction of the game picture (0..1), drawn over it while on
void rs12_video_crosshair(int player, int on, float fx, float fy);

// Small text at the top left (debug OSD, 5x7 font). NULL hides it
void rs12_video_set_osd(const char *text);

// Draw the latest finished frame and queue it for display (waits for vblank)
void rs12_video_present(void);

// ---- overlay layers (menu, notices, FPS box; same model as ReST-V rstv_video_layer) ----
// CPU-writable RGBA8 texture (memory order R,G,B,A = uint32 0xAABBGGRR, premultiplied alpha for layers)
typedef struct RS12Tex {
	unsigned int gl; // GL texture name
	uint32_t *data;  // pixels
	int w, h, stride; // stride in pixels
} RS12Tex;
RS12Tex *rs12_video_tex(int w, int h);
// Layer slot 0..3 (higher on top): top-left w x h of texture t at screen (x, y) 1:1, premultiplied alpha blend. t NULL removes it
void rs12_video_layer(int slot, RS12Tex *t, int x, int y, int w, int h);

// Screen size: aspect 0 = 4:3 (fit height), 1 = integer scale, 2 = full screen. smooth: 1 = bilinear, 0 = nearest (sharp)
void rs12_video_set_mode(int aspect, int smooth);
// Outside the game picture (4:3 / integer): border 0 = black, 1 = artwork (falls back to colour), 2 = colour (index 0..5)
void rs12_video_set_border(int border, int border_color);
// Border artwork: ux0:data/resys12/artwork/<set>.png, else <parent>.png. set NULL drops it
void rs12_video_set_art(const char *set, const char *parent);
// Monitor orientation of the game (MAME ROT90 = 90: the picture turned clockwise, a vertical screen), else 0
void rs12_video_set_rotation(int rot);

// Test: draw the screen as rs12_video_present would and write it to a PPM (960x544) instead of showing it. 0 on success
int rs12_video_capture_ppm(const char *path);

#endif

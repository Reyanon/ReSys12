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
// Layer slot 0..3 (higher on top; slot 4 = the touch keys, under all of them): top-left w x h of texture t at screen (x, y)
// 1:1, premultiplied alpha blend. t NULL removes it
void rs12_video_layer(int slot, RS12Tex *t, int x, int y, int w, int h);
// strips drawn right over slot 0 (the game list rows: drawn once each, moved by the GPU while the list scrolls; ReiREM's
// way), cut to rows y0..y1 of the screen. n 0 removes them
#define RS12_NSTRIP 24
typedef struct { const RS12Tex *t; int x, y; } RS12Strip;
void rs12_video_strips(const RS12Strip *s, int n, int y0, int y1);
// layer slot opacity 0..1 (1 = as drawn)
void rs12_video_layer_alpha(int slot, float a);
// layer slot turned clockwise by rot * 90 degrees (drawn at x, y as an h x w rectangle when rot is odd)
void rs12_video_layer_rot(int slot, int rot);
// one frame while a system dialog (the keyboard) is open: it draws over the picture
void rs12_video_present_dialog(void);
// the app's LiveArea background as RGBA (malloc'd, NULL if missing)
uint8_t *rs12_video_load_bg(int *w, int *h);

// The touch effect over everything, light added to the picture: in each of RS12_FX_LAYERS slots texture t centred at x, y,
// r from centre to edge, strength a (0: none). Called once a frame while it shows: it goes by itself a few frames after the
// last call
#define RS12_FX_LAYERS 2
void rs12_video_touch_fx(int slot, RS12Tex *t, float x, float y, float r, float a);

// Screen size: aspect 0 = 4:3 (fit height), 1 = integer scale, 2 = full screen. smooth: 1 = bilinear, 0 = nearest (sharp)
void rs12_video_set_mode(int aspect, int smooth);
// Outside the game picture (4:3 / integer): border 0 = black, 1 = artwork (else the moving corridor), 2 = colour (index 0..5)
void rs12_video_set_border(int border, int border_color);
// Border artwork: ux0:data/resys12/artwork/<set>.png, else <parent>.png, else none (the moving corridor). set NULL drops it
void rs12_video_set_art(const char *set, const char *parent);
// The game picture turned clockwise by 0, 90, 180 or 270 degrees (the game's monitor orientation, MAME ROT90 = 90,
// plus the Rotate setting)
void rs12_video_set_rotation(int rot);

// Screen point (pixels) -> game picture fraction (0..1, the game's own orientation). 1 if the point is on the picture
int rs12_video_screen_to_game(float sx, float sy, float *fx, float *fy);
// where the game picture is on the screen (pixels); 0 before the first frame
int rs12_video_picture_rect(int *x, int *y, int *w, int *h);

// Test: draw the screen as rs12_video_present would and write it to a PPM (960x544) instead of showing it. 0 on success
int rs12_video_capture_ppm(const char *path);

#endif

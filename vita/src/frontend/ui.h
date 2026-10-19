// Drawing layer for menu, notices, FPS (ported from ReST-V vita/src/frontend/ui.h). Draws the system default PGF font
// (SceLibPgf) to a CPU canvas and uploads it to a vitaGL texture overlaid on the game screen (rs12_video_layer).
#ifndef RS12_UI_H
#define RS12_UI_H

#include <stdint.h>

// color: 0xAABBGGRR (same layout as vita2d RGBA8)
#define UI_RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

typedef struct UiCanvas UiCanvas;

void ui_init(void);
void ui_fini(void);

// canvas: w×h at screen position x,y (overlay layer slot 0..3, higher number on top)
UiCanvas *ui_canvas(int slot, int x, int y, int w, int h);
void ui_canvas_move(UiCanvas *c, int x, int y);
void ui_clear(UiCanvas *c);                    // to transparent
void ui_clear_rect(UiCanvas *c, int x, int y, int w, int h);
void ui_commit(UiCanvas *c);                   // show drawn content (from the next display)
void ui_hide(UiCanvas *c);                     // remove from screen

// drawing (coords inside the canvas, alpha blended)
void ui_rect(UiCanvas *c, int x, int y, int w, int h, uint32_t col);
void ui_line(UiCanvas *c, float x0, float y0, float x1, float y1, float width, uint32_t col);
void ui_circle(UiCanvas *c, float cx, float cy, float r, uint32_t col);
// text: y = baseline (same as vita2d_pgf_draw_text). Returns the drawn width
int ui_text(UiCanvas *c, int x, int y, uint32_t col, float scale, const char *s);
int ui_text_width(float scale, const char *s);
// draw text only within horizontal [cx0, cx1) (scrolling title)
int ui_text_clip(UiCanvas *c, int x, int y, uint32_t col, float scale, const char *s, int cx0, int cx1);
// test: composite the canvas over background color bg and write a PPM
void ui_save_ppm(UiCanvas *c, const char *path, uint32_t bg);

#endif

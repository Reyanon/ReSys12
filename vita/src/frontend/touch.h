// Front touch screen: on-screen key areas (any number, made in the menu's touch layout editor) and the touch point for
// gun games
#ifndef RS12_TOUCH_H
#define RS12_TOUCH_H

#include <stdint.h>

// keys an area can press: RS12_KEY_* (Cross .. Start), then the d-pad
enum { RS12_TKEY_UP = 8, RS12_TKEY_DOWN, RS12_TKEY_LEFT, RS12_TKEY_RIGHT, RS12_TKEY_COUNT };

typedef struct { int16_t x, y, w, h; int8_t key; } Rs12TouchArea; // screen pixels (960x544)

void rs12_touch_init(void);                 // start sampling, read the layout (ux0:data/resys12/touch.cfg)
int rs12_touch_count(void);
Rs12TouchArea *rs12_touch_area(int i);
int rs12_touch_add(int x, int y, int w, int h, int key); // index of the new area (-1: no memory)
void rs12_touch_remove(int i);
void rs12_touch_clear(void);
void rs12_touch_save(void);                 // to the layout file in use
// the layout file in use: the game's (per-game settings on; game_path) or the global one (NULL). keep: the layout now
// is saved to it (per-game settings made); else it is read (a game without its own yet: the global layout)
void rs12_touch_select(const char *game_path, int keep);
int rs12_touch_on_area(int x, int y);       // a touch point on a button
int rs12_touch_gen(void);                   // changes whenever the layout changes
void rs12_touch_changed(void);              // an area was edited in place
uint32_t rs12_touch_key_code(int key);      // RS12_TKEY / RS12_KEY -> SCE_CTRL_* bits
const char *rs12_touch_key_name(int key);

// the touches now (screen pixels, up to 6); returns the count
int rs12_touch_read(int *x, int *y);
// SCE_CTRL_* bits of the areas touched now
uint32_t rs12_touch_buttons(void);

#endif

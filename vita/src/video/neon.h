// Neon corridor: the moving background beside the game picture (Border = Artwork, games without their own artwork)
#pragma once

#include <stdint.h>

// draws the whole screen (the picture goes over the middle). game_x / game_w: the picture's columns; accent: the theme's
// colour (ABGR) the lines take their hue from. GL state as draw_scene keeps it: texturing on, blending off
void rs12_neon_draw(float game_x, float game_w, uint32_t accent);

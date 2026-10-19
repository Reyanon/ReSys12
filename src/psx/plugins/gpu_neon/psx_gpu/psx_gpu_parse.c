/*
 * Copyright (C) 2011 Gilead Kutnick "Exophase" <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <stdio.h>

#include "common.h"
#include "../../gpulib/gpu_timing.h"
#include "../../gpulib/gpu.h"

#ifndef command_lengths
const u8 command_lengths[256] =
{
	0,  0,  2,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // 00
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // 10
	3,  3,  3,  3,  6,  6,  6,  6,  4,  4,  4,  4,  8,  8,  8,  8,   // 20
	5,  5,  5,  5,  8,  8,  8,  8,  7,  7,  7,  7,  11, 11, 11, 11,  // 30
	2,  2,  2,  2,  2,  2,  2,  2,  3,  3,  3,  3,  3,  3,  3,  3,   // 40
	3,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,   // 50
	2,  2,  2,  2,  3,  3,  3,  3,  1,  1,  1,  1,  0,  0,  0,  0,   // 60
	1,  1,  1,  1,  2,  2,  2,  2,  1,  1,  1,  1,  2,  2,  2,  2,   // 70
	3,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // 80
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // 90
	2,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // a0
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // b0
	2,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // c0
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // d0
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // e0
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0    // f0
};
#endif

static void update_texture_ptr(psx_gpu_struct *psx_gpu)
{
  u8 *texture_base;
  u8 *texture_ptr;

  switch((psx_gpu->render_state_base >> 8) & 0x3)
  {
    case TEXTURE_MODE_4BPP:
      // ReSys12: TEXCACHE = shared caches of split rendering (psx_gpu.h)
      texture_base = TEXCACHE(psx_gpu)->texture_4bpp_cache[psx_gpu->current_texture_page];

      texture_ptr = texture_base;
      texture_ptr += psx_gpu->texture_window_x & 0xF;
      texture_ptr += (psx_gpu->texture_window_y & 0xF) << 4;
      texture_ptr += (psx_gpu->texture_window_x >> 4) << 8;
      texture_ptr += (psx_gpu->texture_window_y >> 4) << 12;
      break;

    case TEXTURE_MODE_8BPP:
      if(psx_gpu->current_texture_page & 0x1)
      {
        texture_base =
         TEXCACHE(psx_gpu)->texture_8bpp_odd_cache[psx_gpu->current_texture_page >> 1];
      }
      else
      {
        texture_base =
         TEXCACHE(psx_gpu)->texture_8bpp_even_cache[psx_gpu->current_texture_page >> 1];
      }
      
      texture_ptr = texture_base;
      texture_ptr += psx_gpu->texture_window_x & 0xF;
      texture_ptr += (psx_gpu->texture_window_y & 0xF) << 4;
      texture_ptr += (psx_gpu->texture_window_x >> 4) << 8;
      texture_ptr += (psx_gpu->texture_window_y >> 4) << 12;
      break;

    default:
    case TEXTURE_MODE_16BPP:
      texture_base = (u8 *)(psx_gpu->vram_ptr);
      texture_base += (psx_gpu->current_texture_page & 0xF) * 128;
      texture_base += ((psx_gpu->current_texture_page >> 4) * 256) * 2048;

      texture_ptr = texture_base;
      texture_ptr += psx_gpu->texture_window_x * 2;
      texture_ptr += (psx_gpu->texture_window_y) * 2048;
      break;
  }

  psx_gpu->texture_page_base = texture_base;
  psx_gpu->texture_page_ptr = texture_ptr;  
}

static void set_texture(psx_gpu_struct *psx_gpu, u32 texture_settings)
{
  // ReSys12: System 12 GPU (CXD8654Q, MAME psxgpu type 2) takes texture page
  // Y bit 9 (+512 lines) from tpage bit 11, so keep it. Page index bits:
  // 0-3 = X / 64, 4 = Y bit 8 (tpage bit 4), 5 = Y bit 9 (tpage bit 11).
  // Bits 12/13 flip textured rectangles (x / y), from any tpage like MAME's decode_tpage (tenkomor's text)
  psx_gpu->rect_flip = (texture_settings >> 12) & 3;
  texture_settings &= 0x9FF;
  if(psx_gpu->texture_settings != texture_settings)
  {
    u32 new_texture_page = (texture_settings & 0x1F) |
     ((texture_settings >> 6) & 0x20);
    u32 texture_mode = (texture_settings >> 7) & 0x3;
    u32 render_state_base = psx_gpu->render_state_base;

    flush_render_block_buffer(psx_gpu);

    render_state_base &= ~(0xF << 6);
    render_state_base |= ((texture_settings >> 5) & 0xF) << 6;

    psx_gpu->render_state_base = render_state_base;

    psx_gpu->current_texture_mask = 1ull << new_texture_page;

    if(texture_mode == TEXTURE_MODE_8BPP)
    {
      // In 8bpp mode 256x256 takes up two pages. If it's on the right edge it
      // wraps back around to the left edge.
      u32 adjacent_texture_page = ((new_texture_page + 1) & 0xF) | (new_texture_page & 0x30);
      psx_gpu->current_texture_mask |= 1ull << adjacent_texture_page;

      if((psx_gpu->last_8bpp_texture_page ^ new_texture_page) & 0x1)
      {
        u64 dirty_textures_8bpp_alternate_mask =
         psx_gpu->dirty_textures_8bpp_alternate_mask;
        psx_gpu->dirty_textures_8bpp_alternate_mask =
         psx_gpu->dirty_textures_8bpp_mask;
        psx_gpu->dirty_textures_8bpp_mask = dirty_textures_8bpp_alternate_mask;
      }

      psx_gpu->last_8bpp_texture_page = new_texture_page;
    }

    psx_gpu->current_texture_page = new_texture_page;
    psx_gpu->texture_settings = texture_settings;

    update_texture_ptr(psx_gpu);
  }
}

static void set_clut(psx_gpu_struct *psx_gpu, u32 clut_settings)
{
  clut_settings &= 0xFFFF; // ReSys12: 10-bit CLUT Y (bits 6-15), 1024-line VRAM
  if (psx_gpu->clut_settings != clut_settings)
  {
    flush_render_block_buffer(psx_gpu);
    psx_gpu->clut_settings = clut_settings;
    psx_gpu->clut_ptr = psx_gpu->vram_ptr + clut_settings * 16;
  }
}

static void set_triangle_color(psx_gpu_struct *psx_gpu, u32 triangle_color)
{
  if(psx_gpu->triangle_color != triangle_color)
  {
    flush_render_block_buffer(psx_gpu);
    psx_gpu->triangle_color = triangle_color;
  }
}

// ReSys12: split rendering. A fill (0x02) ignores the draw area, so its rows
// are clipped to the instance's band here: top = rows < split_y, bottom =
// rows >= split_y. Both instances invalidate the whole region so their
// texture caches stay identical.
static void render_block_fill_band(psx_gpu_struct *psx_gpu, u32 color,
 u32 x, u32 y, u32 width, u32 height)
{
  s32 b0, b1;

  if(likely(psx_gpu->split_mode == SPLIT_OFF))
  {
    render_block_fill(psx_gpu, color, x, y, width, height);
    return;
  }
  if((width == 0) || (height == 0))
    return;

  invalidate_texture_cache_region(psx_gpu, x, y, x + width - 1, y + height - 1);
  if(psx_gpu->split_mode == SPLIT_TOP)
  {
    b0 = 0;
    b1 = psx_gpu->split_y - 1;
  }
  else
  {
    b0 = psx_gpu->split_y;
    b1 = 1023;
  }

  if(psx_gpu->render_mode & RENDER_INTERLACE_ENABLED)
  {
    // render_block_fill writes rows y + odd + 2k, k < height / 2
    s32 r0 = (s32)y + ((psx_gpu->render_mode & RENDER_INTERLACE_ODD) ? 1 : 0);
    s32 k0 = 0, k1 = (s32)(height / 2) - 1;
    if(r0 < b0)
      k0 = (b0 - r0 + 1) / 2;
    if(r0 + 2 * k1 > b1)
    {
      if(b1 < r0)
        return;
      k1 = (b1 - r0) / 2;
    }
    if(k1 < k0)
      return;
    render_block_fill(psx_gpu, color, x, y + 2 * k0, width, 2 * (k1 - k0 + 1));
  }
  else
  {
    s32 y0 = (s32)y, y1 = (s32)(y + height) - 1;
    if(y0 < b0)
      y0 = b0;
    if(y1 > b1)
      y1 = b1;
    if(y1 < y0)
      return;
    render_block_fill(psx_gpu, color, x, y0, width, y1 - y0 + 1);
  }
}

#define render_block_fill render_block_fill_band // ReSys12: for do_fill below

static void do_fill(psx_gpu_struct *psx_gpu, u32 x, u32 y,
 u32 width, u32 height, u32 color)
{
  x &= ~0xF;
  width = ((width + 0xF) & ~0xF);

  flush_render_block_buffer(psx_gpu);

  if(unlikely((x + width) > 1024))
  {
    u32 width_a = 1024 - x;
    u32 width_b = width - width_a;

    if(unlikely((y + height) > 1024)) // ReSys12: fills wrap at line 1024
    {
      u32 height_a = 1024 - y;
      u32 height_b = height - height_a;

      render_block_fill(psx_gpu, color, x, y, width_a, height_a);
      render_block_fill(psx_gpu, color, 0, y, width_b, height_a);
      render_block_fill(psx_gpu, color, x, 0, width_a, height_b);
      render_block_fill(psx_gpu, color, 0, 0, width_b, height_b);
    }
    else
    {
      render_block_fill(psx_gpu, color, x, y, width_a, height);
      render_block_fill(psx_gpu, color, 0, y, width_b, height);
    }
  }
  else
  {
    if(unlikely((y + height) > 1024)) // ReSys12: fills wrap at line 1024
    {
      u32 height_a = 1024 - y;
      u32 height_b = height - height_a;

      render_block_fill(psx_gpu, color, x, y, width, height_a);
      render_block_fill(psx_gpu, color, x, 0, width, height_b);
    }
    else
    {
      render_block_fill(psx_gpu, color, x, y, width, height);
    }
  }
}

#undef render_block_fill // ReSys12

#define get_vertex_data_xy(vertex_number, offset16)                            \
  vertexes[vertex_number].x = sign_extend_11bit(list_s16[offset16]);           \
  vertexes[vertex_number].y = sign_extend_11bit(list_s16[(offset16) + 1]);     \

#define get_vertex_data_uv(vertex_number, offset16)                            \
  vertexes[vertex_number].u = list_s16[offset16] & 0xFF;                       \
  vertexes[vertex_number].v = (list_s16[offset16] >> 8) & 0xFF                 \

#define get_vertex_data_rgb(vertex_number, offset32)                           \
  vertexes[vertex_number].r = list[offset32] & 0xFF;                           \
  vertexes[vertex_number].g = (list[offset32] >> 8) & 0xFF;                    \
  vertexes[vertex_number].b = (list[offset32] >> 16) & 0xFF                    \

#define get_vertex_data_xy_uv(vertex_number, offset16)                         \
  get_vertex_data_xy(vertex_number, offset16);                                 \
  get_vertex_data_uv(vertex_number, (offset16) + 2)                            \

#define get_vertex_data_xy_rgb(vertex_number, offset16)                        \
  get_vertex_data_rgb(vertex_number, (offset16) / 2);                          \
  get_vertex_data_xy(vertex_number, (offset16) + 2);                           \

#define get_vertex_data_xy_uv_rgb(vertex_number, offset16)                     \
  get_vertex_data_rgb(vertex_number, (offset16) / 2);                          \
  get_vertex_data_xy(vertex_number, (offset16) + 2);                           \
  get_vertex_data_uv(vertex_number, (offset16) + 4);                           \

#define set_vertex_color_constant(vertex_number, color)                        \
  vertexes[vertex_number].r = color & 0xFF;                                    \
  vertexes[vertex_number].g = (color >> 8) & 0xFF;                             \
  vertexes[vertex_number].b = (color >> 16) & 0xFF                             \

#define get_vertex_data_xy_rgb_constant(vertex_number, offset16, color)        \
  get_vertex_data_xy(vertex_number, offset16);                                 \
  set_vertex_color_constant(vertex_number, color)                              \

static void do_quad(psx_gpu_struct *psx_gpu, vertex_struct *vertexes,
 u32 current_command);

// ReSys12: a flipped textured rectangle (System 12 GPU, MAME psxgpu type 2: the texel steps back from u / v) as
// textured quads whose UVs run backwards. Pixel i of an axis samples start -/+ i, so the far vertex holds start -/+ size,
// which must stay in 0..255: a piece reaching texel 0 (or 255) leaves its last row / column to a piece of its own
// (one texel wide, drawn with a gradient that stays in range)
static void flipped_piece(psx_gpu_struct *psx_gpu, u32 cmd, s32 x, s32 y, s32 w, s32 h, s32 u, s32 v,
 s32 fx, s32 fy)
{
  vertex_struct vertexes[4] __attribute__((aligned(16))) = {};
  s32 u1, v1;
  int i;

  // texture coordinates wrap at 256: split where they do
  if(fx ? u - w < -1 : u + w > 256)
  {
    const s32 k = fx ? u + 1 : 256 - u;
    flipped_piece(psx_gpu, cmd, x, y, k, h, u, v, fx, fy);
    flipped_piece(psx_gpu, cmd, x + k, y, w - k, h, fx ? 255 : 0, v, fx, fy);
    return;
  }
  if(fy ? v - h < -1 : v + h > 256)
  {
    const s32 k = fy ? v + 1 : 256 - v;
    flipped_piece(psx_gpu, cmd, x, y, w, k, u, v, fx, fy);
    flipped_piece(psx_gpu, cmd, x, y + k, w, h - k, u, fy ? 255 : 0, fx, fy);
    return;
  }
  if(w > 1 && ((fx && u - w < 0) || (!fx && u + w > 255)))
  {
    flipped_piece(psx_gpu, cmd, x, y, w - 1, h, u, v, fx, fy);
    flipped_piece(psx_gpu, cmd, x + w - 1, y, 1, h, fx ? u - (w - 1) : u + (w - 1), v, fx, fy);
    return;
  }
  if(h > 1 && ((fy && v - h < 0) || (!fy && v + h > 255)))
  {
    flipped_piece(psx_gpu, cmd, x, y, w, h - 1, u, v, fx, fy);
    flipped_piece(psx_gpu, cmd, x, y + h - 1, w, 1, u, fy ? v - (h - 1) : v + (h - 1), fx, fy);
    return;
  }
  // one texel wide: either direction samples the start texel
  u1 = w == 1 ? (u < 255 ? u + 1 : u - 1) : fx ? u - w : u + w;
  v1 = h == 1 ? (v < 255 ? v + 1 : v - 1) : fy ? v - h : v + h;
  for(i = 0; i < 4; i++)
  {
    vertexes[i].x = x + ((i & 1) ? w : 0);
    vertexes[i].y = y + ((i & 2) ? h : 0);
    vertexes[i].u = (i & 1) ? u1 : u;
    vertexes[i].v = (i & 2) ? v1 : v;
  }
  do_quad(psx_gpu, vertexes, cmd);
}

static int flipped_sprite(psx_gpu_struct *psx_gpu, const u32 *list, s32 width, s32 height)
{
  if(width <= 0 || height <= 0)
    return 1;
  set_triangle_color(psx_gpu, list[0] & 0xFFFFFF);
  // sprites are not dithered
  const u32 dither = psx_gpu->render_state_base & RENDER_STATE_DITHER;
  psx_gpu->render_state_base &= ~RENDER_STATE_DITHER;
  // textured quads with the rectangle's raw-texture (bit 0) and semi-transparency (bit 1) bits
  flipped_piece(psx_gpu, 0x2C | ((list[0] >> 24) & 3), sign_extend_11bit(list[1]), sign_extend_11bit(list[1] >> 16),
    width, height, list[2] & 0xff, (list[2] >> 8) & 0xff, psx_gpu->rect_flip & 1, (psx_gpu->rect_flip >> 1) & 1);
  psx_gpu->render_state_base |= dither;
  return 1;
}

static void textured_sprite(psx_gpu_struct *psx_gpu, const u32 *list,
  s32 width, s32 height, u32 *cpu_cycles_sum, u32 *cpu_cycles)
{
  s32 x = sign_extend_11bit(list[1] + psx_gpu->offset_x);
  s32 y = sign_extend_11bit((list[1] >> 16) + psx_gpu->offset_y);
  u8 v = (list[2] >> 8) & 0xff;
  u8 u = list[2] & 0xff;

  set_clut(psx_gpu, list[2] >> 16);

  if(unlikely(psx_gpu->rect_flip) && flipped_sprite(psx_gpu, list, width, height))
  {
    gput_sum(*cpu_cycles_sum, *cpu_cycles, gput_sprite(width, height));
    return;
  }

  render_sprite(psx_gpu, x, y, u, v, &width, &height, list[0] >> 24, list[0]);
  gput_sum(*cpu_cycles_sum, *cpu_cycles, gput_sprite(width, height));
}

static void undo_offset(vertex_struct *vertexes, prepared_triangle *triangle)
{
  s32 i;
  for (i = 0; i < 3; i++)
  {
    vertexes[i].x -= triangle->offset_x;
    vertexes[i].y -= triangle->offset_y;
  }
}

static void do_triangle(psx_gpu_struct *psx_gpu, vertex_struct *vertexes,
 u32 current_command)
{
  prepared_triangle triangle;
  RS12_STAGE(5);
  if (prepare_triangle(psx_gpu, vertexes, &triangle))
    render_triangle_p(psx_gpu, triangle.vertexes, current_command);
}

static void do_quad(psx_gpu_struct *psx_gpu, vertex_struct *vertexes,
 u32 current_command)
{
  prepared_triangle triangle;
  RS12_STAGE(5);
  if (prepare_triangle(psx_gpu, vertexes, &triangle))
  {
    render_triangle_p(psx_gpu, triangle.vertexes, current_command);
    undo_offset(vertexes, &triangle);
  }
  RS12_STAGE(5);
  if (prepare_triangle(psx_gpu, vertexes + 1, &triangle))
    render_triangle_p(psx_gpu, triangle.vertexes, current_command);
}

// ReSys12: what the renderer is doing (command byte, 0x100 = block flush/rasterize), read by the frontend's sampler
volatile u32 rs12_gpu_act;

// ReSys12: split rendering helpers. Two instances parse the same lists; each
// draws only into its band of rows. Within one gpu_parse call ("chunk") the
// instances run unsynchronized, so a command that reads VRAM (texture page,
// CLUT) written earlier in the chunk, or writes VRAM read earlier in the
// chunk, ends the chunk (SPLIT_BARRIER_CMD) - the caller joins both instances
// and restarts at that command. Both make the same decision (same list, same
// state), so they always stop at the same place. Writes are tracked as
// rectangles (draw area, fills), texture page reads as a page mask (they are
// whole 64x256 pages), CLUT reads as rectangles. A primitive that reads what
// it writes itself is drawn by the top instance alone in a chunk of its own
// ("solo"), as the bands would otherwise read each other's pixels mid-draw.

// ReSys12: band ratio per draw area. Rows do not cost the same and the
// draw areas of a frame (scene, offscreen effects, HUD) differ, so one shared
// ratio was pulled both ways. Both instances read the table during a chunk;
// the rendering thread updates it only between chunks (psx_gpu_if.c).
split_ratio_entry rs12_split_ratio_tab[SPLIT_RATIO_TAB];

static u32 split_area_key(const psx_gpu_struct *psx_gpu)
{
  return 0x80000000u | (psx_gpu->viewport_start_x & 0x3FF) |
   ((psx_gpu->logical_vy0 & 0x3FF) << 10) |
   ((u32)(psx_gpu->logical_vy1 & 0x3FF) << 20);
}

// band of the current chunk: split_y comes from the draw area (logical_vy*);
// solo = everything to the top instance (split_y = 1024)
static void split_apply_band(psx_gpu_struct *psx_gpu, int solo)
{
  s32 y0 = psx_gpu->logical_vy0, y1 = psx_gpu->logical_vy1;
  s32 s = y0;
  s32 ratio = psx_gpu->split_ratio;
  u32 i, key = split_area_key(psx_gpu);

  psx_gpu->split_key = key;
  for(i = 0; i < SPLIT_RATIO_TAB; i++)
    if(rs12_split_ratio_tab[i].key == key)
    {
      ratio = rs12_split_ratio_tab[i].ratio;
      break;
    }

  if(solo)
    s = 1024;
  else if(y1 >= y0)
    s = y0 + (((y1 - y0 + 1) * ratio) >> 8);
  psx_gpu->split_y = s;
  if(psx_gpu->split_mode == SPLIT_TOP)
  {
    psx_gpu->viewport_start_y = y0;
    psx_gpu->viewport_end_y = y1 < s - 1 ? y1 : s - 1;
  }
  else
  {
    psx_gpu->viewport_start_y = y0 > s ? y0 : s;
    psx_gpu->viewport_end_y = y1;
  }
  psx_gpu->split_set = 1;
}

// rectangles are inclusive: x0, y0, x1, y1
static int split_hit(const s16 *r, const s32 *q)
{
  return !(q[2] < r[0] || r[2] < q[0] || q[3] < r[1] || r[3] < q[1]);
}

static int split_hit_any(const s16 (*rects)[4], u32 n, const s32 *q)
{
  u32 i;
  for(i = 0; i < n; i++)
    if(split_hit(rects[i], q))
      return 1;
  return 0;
}

static int split_hit_q(const s32 *a, const s32 *q)
{
  return !(q[2] < a[0] || a[2] < q[0] || q[3] < a[1] || a[3] < q[1]);
}

static int split_inside_any(const s16 (*rects)[4], u32 n, const s32 *q)
{
  u32 i;
  for(i = 0; i < n; i++)
    if(rects[i][0] <= q[0] && rects[i][1] <= q[1] &&
       q[2] <= rects[i][2] && q[3] <= rects[i][3])
      return 1;
  return 0;
}

static void split_add(s16 (*rects)[4], u32 *n, u32 max, const s32 *q)
{
  u32 i = *n;
  if(i == max)
  {
    // full: grow the last one to the bounding box
    i = max - 1;
    if(q[0] < rects[i][0]) rects[i][0] = q[0];
    if(q[1] < rects[i][1]) rects[i][1] = q[1];
    if(q[2] > rects[i][2]) rects[i][2] = q[2];
    if(q[3] > rects[i][3]) rects[i][3] = q[3];
    return;
  }
  rects[i][0] = q[0]; rects[i][1] = q[1];
  rects[i][2] = q[2]; rects[i][3] = q[3];
  *n = i + 1;
}

#define SPLIT_SET_RECT(r_, x0_, y0_, x1_, y1_) \
  do { (r_)[0] = (x0_); (r_)[1] = (y0_); (r_)[2] = (x1_); (r_)[3] = (y1_); } while(0)

// regions a textured primitive reads: texture page (1-2 rects, wrapping at
// x 1024) and, for 4/8bpp, the CLUT (1-2 rects); returns the count of each
static void split_texture_regions(u32 tpage, u32 clut, s32 (*tex)[4],
 u32 *n_tex, s32 (*cl)[4], u32 *n_cl)
{
  s32 x = (tpage & 0xF) * 64;
  s32 y = ((tpage >> 4) & 1) * 256 + ((tpage >> 11) & 1) * 512;
  u32 mode = (tpage >> 7) & 3;
  s32 w = mode == 0 ? 64 : mode == 1 ? 128 : 256;

  *n_tex = 1;
  SPLIT_SET_RECT(tex[0], x, y, x + w - 1 < 1023 ? x + w - 1 : 1023, y + 255);
  if(x + w > 1024)
  {
    SPLIT_SET_RECT(tex[1], 0, y, x + w - 1 - 1024, y + 255);
    *n_tex = 2;
  }
  *n_cl = 0;
  if(mode < 2)
  {
    s32 cx = (clut & 0x3F) * 16, cy = (clut >> 6) & 0x3FF;
    s32 cw = mode == 0 ? 16 : 256;
    *n_cl = 1;
    SPLIT_SET_RECT(cl[0], cx, cy, cx + cw - 1 < 1023 ? cx + cw - 1 : 1023, cy);
    if(cx + cw > 1024)
    {
      SPLIT_SET_RECT(cl[1], 0, cy, cx + cw - 1 - 1024, cy);
      *n_cl = 2;
    }
  }
}

// ReSys12: the VRAM rect a textured polygon actually samples, from the
// vertex UVs (n uv words, low 16 bits each); 0 if unknown (texture window,
// wrap at x 1024). Interpolated UVs stay within the vertices' range; one
// texel of margin covers rounding.
static int split_uv_rect(u32 tpage, const u32 *uvw, u32 n, u32 stride,
 u32 texture_window, s32 *r)
{
  s32 x = (tpage & 0xF) * 64;
  s32 y = ((tpage >> 4) & 1) * 256 + ((tpage >> 11) & 1) * 512;
  u32 mode = (tpage >> 7) & 3;
  s32 u0 = 255, u1 = 0, v0 = 255, v1 = 0;
  u32 i;

  if(texture_window & 0x3FF)
    return 0;
  for(i = 0; i < n; i++)
  {
    const s32 u = uvw[i * stride] & 0xFF, v = (uvw[i * stride] >> 8) & 0xFF;
    if(u < u0) u0 = u;
    if(u > u1) u1 = u;
    if(v < v0) v0 = v;
    if(v > v1) v1 = v;
  }
  u0 = u0 > 0 ? u0 - 1 : 0;
  u1 = u1 < 255 ? u1 + 1 : 255;
  v0 = v0 > 0 ? v0 - 1 : 0;
  v1 = v1 < 255 ? v1 + 1 : 255;
  if(mode == 0)
    u0 >>= 2, u1 >>= 2;
  else if(mode == 1)
    u0 >>= 1, u1 >>= 1;
  if(x + u1 > 1023)
    return 0;
  SPLIT_SET_RECT(r, x + u0, y + v0, x + u1, y + v1);
  return 1;
}

// called before every drawing command in split mode; returns 1 if the
// command must start a new chunk
static noinline int split_check(psx_gpu_struct *psx_gpu, const u32 *list,
 u32 cmd)
{
  const s16 *list_s16 = (const s16 *)list;
  s32 wr[4], tex[2][4], cl[2][4];
  u32 n_wr = 0, n_tex = 0, n_cl = 0, i, tpage = 0, clut = 0, textured = 1;
  u32 nw, nc;
  int solo = 0;
  // polygons: vertex UV words, so the check can use the rect actually
  // sampled instead of the whole texture page
  const u32 *uvw = NULL;
  u32 n_uv = 0, uv_stride = 0;
  int used_uvr = 0;

  // after a solo command, the next drawing command starts a new chunk
  if(psx_gpu->split_solo)
    return 1;

  if(cmd == 0x02)
  {
    s32 x = (list_s16[2] & 0x3FF) & ~0xF;
    s32 y = list_s16[3] & 0x3FF;
    s32 w = ((list_s16[4] & 0x3FF) + 0xF) & ~0xF;
    s32 h = list_s16[5] & 0x3FF;
    textured = 0;
    if(w != 0 && h != 0)
    {
      n_wr = 1;
      if(x + w > 1024 || y + h > 1024)
        SPLIT_SET_RECT(wr, 0, 0, 1023, 1023); // wrapping fill: be conservative
      else
        SPLIT_SET_RECT(wr, x, y, x + w - 1, y + h - 1);
    }
  }
  else
  {
    if(psx_gpu->viewport_start_x <= psx_gpu->viewport_end_x &&
       psx_gpu->logical_vy0 <= psx_gpu->logical_vy1)
    {
      n_wr = 1;
      SPLIT_SET_RECT(wr, psx_gpu->viewport_start_x, psx_gpu->logical_vy0,
       psx_gpu->viewport_end_x, psx_gpu->logical_vy1);
    }
    switch(cmd)
    {
      case 0x24 ... 0x27:
        tpage = (u16)list_s16[9]; clut = (u16)list_s16[5];
        uvw = list + 2; n_uv = 3; uv_stride = 2;
        break;
      case 0x2C ... 0x2F:
        tpage = list[4] >> 16; clut = list[2] >> 16;
        uvw = list + 2; n_uv = 4; uv_stride = 2;
        break;
      case 0x34 ... 0x37:
        tpage = (u16)list_s16[11]; clut = (u16)list_s16[5];
        uvw = list + 2; n_uv = 3; uv_stride = 3;
        break;
      case 0x3C ... 0x3F:
        tpage = list[5] >> 16; clut = list[2] >> 16;
        uvw = list + 2; n_uv = 4; uv_stride = 3;
        break;
      case 0x64 ... 0x67:
      case 0x74 ... 0x77:
      case 0x7C ... 0x7F:
        tpage = psx_gpu->texture_settings; clut = list[2] >> 16;
        break;
      default:
        textured = 0;
        break;
    }
    // ReSys12: same texture page, CLUT, texture window and draw area as the last polygon that passed, and no
    // write / CLUT rect added since: only the UV rect can differ (ehrgeiz: the check was ~8% of the renderer)
    if(textured && uvw && n_wr && psx_gpu->split_set &&
       psx_gpu->split_fast_ver == psx_gpu->split_ver && psx_gpu->split_fast_tpage == tpage &&
       psx_gpu->split_fast_clut == clut &&
       psx_gpu->split_fast_tw == psx_gpu->texture_window_settings &&
       !memcmp(psx_gpu->split_fast_wr, wr, sizeof(wr)))
    {
      s32 uvr[4];
      if(split_uv_rect(tpage, uvw, n_uv, uv_stride,
         psx_gpu->texture_window_settings, uvr))
      {
        if(split_hit_any(psx_gpu->split_w, psx_gpu->split_nw, uvr) || split_hit_q(wr, uvr))
          return 1;
        psx_gpu->split_read_pages |= texture_region_mask(uvr[0], uvr[1], uvr[2], uvr[3]);
        return 0;
      }
    }
    if(textured)
    {
      split_texture_regions(tpage, clut, tex, &n_tex, cl, &n_cl);
      // a page that overlaps this chunk's writes (the draw area, fills)
      // while the UVs stay clear of them (toukon3: ~40 polygons a frame)
      // need not be drawn solo; both instances may refresh the page's cache
      // meanwhile, so after the chunk it is converted again (split_refresh)
      s32 uvr[4];
      if(uvw && split_uv_rect(tpage, uvw, n_uv, uv_stride,
         psx_gpu->texture_window_settings, uvr))
      {
        for(i = 0; i < n_tex; i++)
          if((n_wr && split_hit_q(wr, tex[i])) ||
             split_hit_any(psx_gpu->split_w, psx_gpu->split_nw, tex[i]))
            psx_gpu->split_refresh = 1;
        memcpy(tex[0], uvr, sizeof(uvr));
        n_tex = 1;
        used_uvr = 1;
      }
    }
  }

  nw = psx_gpu->split_nw;
  nc = psx_gpu->split_nc;

  // reads of something written earlier in this chunk
  for(i = 0; i < n_tex; i++)
    if(split_hit_any(psx_gpu->split_w, nw, tex[i]))
      return 1;
  for(i = 0; i < n_cl; i++)
    if(split_hit_any(psx_gpu->split_w, nw, cl[i]))
      return 1;

  if(n_wr)
  {
    // writes of something read earlier in this chunk (only new area matters)
    if(!split_inside_any(psx_gpu->split_w, nw, wr))
    {
      if(texture_region_mask(wr[0], wr[1], wr[2], wr[3]) &
         psx_gpu->split_read_pages)
        return 1;
      if(split_hit_any(psx_gpu->split_c, nc, wr))
        return 1;
    }
    // reads of what this primitive writes itself
    for(i = 0; i < n_tex; i++)
      solo |= split_hit_q(wr, tex[i]);
    for(i = 0; i < n_cl; i++)
      solo |= split_hit_q(wr, cl[i]);
    if(solo && psx_gpu->split_set)
      return 1;
  }

  if(!psx_gpu->split_set)
  {
    split_apply_band(psx_gpu, solo);
    psx_gpu->split_solo = solo;
  }

  if(n_wr && !split_inside_any(psx_gpu->split_w, nw, wr))
  {
    split_add(psx_gpu->split_w, &nw, 4, wr);
    psx_gpu->split_ver++;
    psx_gpu->split_nw = nw;
  }
  for(i = 0; i < n_tex; i++)
    psx_gpu->split_read_pages |=
     texture_region_mask(tex[i][0], tex[i][1], tex[i][2], tex[i][3]);
  for(i = 0; i < n_cl; i++)
  {
    if(split_inside_any(psx_gpu->split_c, nc, cl[i]))
      continue;
    if(nc < 16)
    {
      split_add(psx_gpu->split_c, &nc, 16, cl[i]);
      psx_gpu->split_ver++;
    }
    else // too many CLUTs: fall back to whole pages
      psx_gpu->split_read_pages |=
       texture_region_mask(cl[i][0], cl[i][1], cl[i][2], cl[i][3]);
  }
  psx_gpu->split_nc = nc;
  if(used_uvr && n_wr && !solo && psx_gpu->split_set)
  {
    psx_gpu->split_fast_ver = psx_gpu->split_ver;
    psx_gpu->split_fast_tpage = tpage;
    psx_gpu->split_fast_clut = clut;
    psx_gpu->split_fast_tw = psx_gpu->texture_window_settings;
    memcpy(psx_gpu->split_fast_wr, wr, sizeof(wr));
  }
  return 0;
}

// ReSys12: split_check() marked for the sampling profiler (stage 4)
static inline int split_check_staged(psx_gpu_struct *psx_gpu, const u32 *list, u32 cmd)
{
  RS12_STAGE(4);
  const int r = split_check(psx_gpu, list, cmd);
  RS12_STAGE(0);
  return r;
}

u32 gpu_parse(psx_gpu_struct *psx_gpu, u32 *list, u32 size, u32 *ex_regs,
 s32 *cpu_cycles_sum_out, s32 *cpu_cycles_last, u32 *last_command)
{
  vertex_struct vertexes[4] __attribute__((aligned(16))) = {};
  u32 current_command = 0, command_length;
  u32 cpu_cycles_sum = 0, cpu_cycles = *cpu_cycles_last;
  u32 simplified_prim[4*4];

  u32 *list_start = list;
  u32 *list_end = list + (size / 4);
  // ReSys12: per-instance marker
  volatile u32 *act_ptr = psx_gpu->act_ptr ? psx_gpu->act_ptr : &rs12_gpu_act;

  // ReSys12: split rendering - every call starts a new chunk (the caller has
  // joined both instances before)
  if(unlikely(psx_gpu->split_mode != SPLIT_OFF))
  {
    psx_gpu->split_set = 0;
    psx_gpu->split_solo = 0;
    psx_gpu->split_refresh = 0;
    psx_gpu->split_key = 0;
    psx_gpu->split_nw = 0;
    psx_gpu->split_nc = 0;
    psx_gpu->split_read_pages = 0;
    psx_gpu->split_ver++;
  }

  for(; list < list_end; list += 1 + command_length)
  {
    s16 *list_s16 = (void *)list;
    current_command = *list >> 24;
    *act_ptr = current_command; // ReSys12: sampling profiler marker
    psx_gpu->cmd_seq++;
    command_length = command_lengths[current_command];
    if (list + 1 + command_length > list_end) {
      current_command = (u32)-1;
      break;
    }

    // ReSys12: split rendering - chunk boundaries before drawing commands
    if(unlikely(psx_gpu->split_mode != SPLIT_OFF) &&
       (current_command == 0x02 ||
        (current_command >= 0x20 && current_command < 0x80)) &&
       split_check_staged(psx_gpu, list, current_command))
    {
      current_command = SPLIT_BARRIER_CMD;
      goto breakloop;
    }

    switch(current_command)
    {
      case 0x00:
        break;

      case 0x02:
      {
        u32 x = list_s16[2] & 0x3FF;
        u32 y = list_s16[3] & 0x3FF; // ReSys12: 10-bit Y/height (1024 lines)
        u32 width = list_s16[4] & 0x3FF;
        u32 height = list_s16[5] & 0x3FF;
        u32 color = list[0] & 0xFFFFFF;

        do_fill(psx_gpu, x, y, width, height, color);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_fill(width, height));
        break;
      }

      case 0x20 ... 0x23:
      {
        set_triangle_color(psx_gpu, list[0] & 0xFFFFFF);
  
        get_vertex_data_xy(0, 2);
        get_vertex_data_xy(1, 4);
        get_vertex_data_xy(2, 6);
          
        do_triangle(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base());
        break;
      }
  
      case 0x24 ... 0x27:
      {
        set_clut(psx_gpu, list_s16[5]);
        set_texture(psx_gpu, list_s16[9]);
        set_triangle_color(psx_gpu, list[0] & 0xFFFFFF);
  
        get_vertex_data_xy_uv(0, 2);
        get_vertex_data_xy_uv(1, 6);
        get_vertex_data_xy_uv(2, 10);
  
        do_triangle(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_t());
        break;
      }
  
      case 0x28 ... 0x2B:
      {
        set_triangle_color(psx_gpu, list[0] & 0xFFFFFF);
  
        get_vertex_data_xy(0, 2);
        get_vertex_data_xy(1, 4);
        get_vertex_data_xy(2, 6);
        get_vertex_data_xy(3, 8);

        do_quad(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base());
        break;
      }
  
      case 0x2C ... 0x2F:
      {
        u32 i, simplified_count;
        set_texture(psx_gpu, list[4] >> 16);
        if (!(psx_gpu->render_state_base & RENDER_STATE_DITHER) &&
            (simplified_count = prim_try_simplify_quad_t(simplified_prim, list)))
        {
          for (i = 0; i < simplified_count; i++) {
            const u32 *list_ = &simplified_prim[i * 4];
            textured_sprite(psx_gpu, list_, list_[3] & 0x3FF,
              (list_[3] >> 16) & 0x1FF, &cpu_cycles_sum, &cpu_cycles);
          }
          break;
        }

        set_clut(psx_gpu, list[2] >> 16);
        set_triangle_color(psx_gpu, list[0] & 0xFFFFFF);
  
        get_vertex_data_xy_uv(0, 2);   
        get_vertex_data_xy_uv(1, 6);   
        get_vertex_data_xy_uv(2, 10);  
        get_vertex_data_xy_uv(3, 14);

        do_quad(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_t());
        break;
      }
  
      case 0x30 ... 0x33:
      {
        get_vertex_data_xy_rgb(0, 0);
        get_vertex_data_xy_rgb(1, 4);
        get_vertex_data_xy_rgb(2, 8);
  
        do_triangle(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_g());
        break;
      }
  
      case 0x34 ... 0x37:
      {
        set_clut(psx_gpu, list_s16[5]);
        set_texture(psx_gpu, list_s16[11]);
  
        get_vertex_data_xy_uv_rgb(0, 0);
        get_vertex_data_xy_uv_rgb(1, 6);
        get_vertex_data_xy_uv_rgb(2, 12);

        do_triangle(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_gt());
        break;
      }
  
      case 0x38 ... 0x3B:
      {
        get_vertex_data_xy_rgb(0, 0);
        get_vertex_data_xy_rgb(1, 4);
        get_vertex_data_xy_rgb(2, 8);
        get_vertex_data_xy_rgb(3, 12);

        do_quad(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_g());
        break;
      }
  
      case 0x3C ... 0x3F:
      {
        u32 i, simplified_count;
        set_texture(psx_gpu, list[5] >> 16);
        if (!(psx_gpu->render_state_base & RENDER_STATE_DITHER) &&
            (simplified_count = prim_try_simplify_quad_gt(simplified_prim, list)))
        {
          for (i = 0; i < simplified_count; i++) {
            const u32 *list_ = &simplified_prim[i * 4];
            textured_sprite(psx_gpu, list_, list_[3] & 0x3FF,
              (list_[3] >> 16) & 0x1FF, &cpu_cycles_sum, &cpu_cycles);
          }
          break;
        }

        set_clut(psx_gpu, list[2] >> 16);
  
        get_vertex_data_xy_uv_rgb(0, 0);
        get_vertex_data_xy_uv_rgb(1, 6);
        get_vertex_data_xy_uv_rgb(2, 12);
        get_vertex_data_xy_uv_rgb(3, 18);

        do_quad(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_gt());
        break;
      }
  
      case 0x40 ... 0x47:
      {
        vertexes[0].x = list_s16[2] + psx_gpu->offset_x;
        vertexes[0].y = list_s16[3] + psx_gpu->offset_y;
        vertexes[1].x = list_s16[4] + psx_gpu->offset_x;
        vertexes[1].y = list_s16[5] + psx_gpu->offset_y;

        render_line(psx_gpu, vertexes, current_command, list[0], 0);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));
        break;
      }
  
      case 0x48 ... 0x4F:
      {
        u32 num_vertexes = 1;
        u32 *list_position = &(list[2]);
        u32 xy = list[1];

        vertexes[1].x = (xy & 0xFFFF) + psx_gpu->offset_x;
        vertexes[1].y = (xy >> 16) + psx_gpu->offset_y;
      
        xy = *list_position;
        while(1)
        {
          vertexes[0] = vertexes[1];

          vertexes[1].x = (xy & 0xFFFF) + psx_gpu->offset_x;
          vertexes[1].y = (xy >> 16) + psx_gpu->offset_y;

          render_line(psx_gpu, vertexes, current_command, list[0], 0);
          gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));

          list_position++;
          num_vertexes++;

          if(list_position >= list_end)
          {
            current_command = (u32)-1;
            goto breakloop;
          }

          xy = *list_position;
          if((xy & 0xF000F000) == 0x50005000)
            break;
        }

        command_length += (num_vertexes - 2);
        break;
      }
  
      case 0x50 ... 0x57:
      {
        vertexes[0].r = list[0] & 0xFF;
        vertexes[0].g = (list[0] >> 8) & 0xFF;
        vertexes[0].b = (list[0] >> 16) & 0xFF;
        vertexes[0].x = list_s16[2] + psx_gpu->offset_x;
        vertexes[0].y = list_s16[3] + psx_gpu->offset_y;

        vertexes[1].r = list[2] & 0xFF;
        vertexes[1].g = (list[2] >> 8) & 0xFF;
        vertexes[1].b = (list[2] >> 16) & 0xFF;
        vertexes[1].x = list_s16[6] + psx_gpu->offset_x;
        vertexes[1].y = list_s16[7] + psx_gpu->offset_y;

        render_line(psx_gpu, vertexes, current_command, 0, 0);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));
        break;
      }
 
      case 0x58 ... 0x5F:
      {
        u32 num_vertexes = 1;
        u32 *list_position = &(list[2]);
        u32 color = list[0];
        u32 xy = list[1];

        vertexes[1].r = color & 0xFF;
        vertexes[1].g = (color >> 8) & 0xFF;
        vertexes[1].b = (color >> 16) & 0xFF;
        vertexes[1].x = (xy & 0xFFFF) + psx_gpu->offset_x;
        vertexes[1].y = (xy >> 16) + psx_gpu->offset_y;
      
        color = list_position[0];
        while(1)
        {
          xy = list_position[1];

          vertexes[0] = vertexes[1];

          vertexes[1].r = color & 0xFF;
          vertexes[1].g = (color >> 8) & 0xFF;
          vertexes[1].b = (color >> 16) & 0xFF;
          vertexes[1].x = (xy & 0xFFFF) + psx_gpu->offset_x;
          vertexes[1].y = (xy >> 16) + psx_gpu->offset_y;

          render_line(psx_gpu, vertexes, current_command, 0, 0);
          gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));

          list_position += 2;
          num_vertexes++;

          if(list_position >= list_end)
          {
            current_command = (u32)-1;
            goto breakloop;
          }

          color = list_position[0];
          if((color & 0xF000F000) == 0x50005000)
            break;
        }

        command_length += ((num_vertexes - 2) * 2);
        break;
      }
  
      case 0x60 ... 0x63:
      {        
        u32 x = sign_extend_11bit(list_s16[2] + psx_gpu->offset_x);
        u32 y = sign_extend_11bit(list_s16[3] + psx_gpu->offset_y);
        s32 width = list_s16[4] & 0x3FF;
        s32 height = list_s16[5] & 0x1FF;

        render_sprite(psx_gpu, x, y, 0, 0, &width, &height,
           current_command, list[0]);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(width, height));
        break;
      }

      case 0x64 ... 0x67:
        textured_sprite(psx_gpu, list, list[3] & 0x3FF, (list[3] >> 16) & 0x1FF,
          &cpu_cycles_sum, &cpu_cycles);
        break;

      case 0x68 ... 0x6B:
      {
        s32 x = sign_extend_11bit(list_s16[2] + psx_gpu->offset_x);
        s32 y = sign_extend_11bit(list_s16[3] + psx_gpu->offset_y);
        s32 width = 1, height = 1;

        render_sprite(psx_gpu, x, y, 0, 0, &width, &height,
           current_command, list[0]);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(1, 1));
        break;
      }
  
      case 0x70 ... 0x73:
      {        
        s32 x = sign_extend_11bit(list_s16[2] + psx_gpu->offset_x);
        s32 y = sign_extend_11bit(list_s16[3] + psx_gpu->offset_y);
        s32 width = 8, height = 8;

        render_sprite(psx_gpu, x, y, 0, 0, &width, &height,
           current_command, list[0]);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(width, height));
        break;
      }

      case 0x74 ... 0x77:
        textured_sprite(psx_gpu, list, 8, 8, &cpu_cycles_sum, &cpu_cycles);
        break;

      case 0x78 ... 0x7B:
      {        
        s32 x = sign_extend_11bit(list_s16[2] + psx_gpu->offset_x);
        s32 y = sign_extend_11bit(list_s16[3] + psx_gpu->offset_y);
        s32 width = 16, height = 16;

        render_sprite(psx_gpu, x, y, 0, 0, &width, &height,
           current_command, list[0]);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(width, height));
        break;
      }
  
      case 0x7C ... 0x7F:
        textured_sprite(psx_gpu, list, 16, 16, &cpu_cycles_sum, &cpu_cycles);
        break;
  
#ifdef PCSX
      case 0x1F:                   //  irq?
      case 0x80 ... 0x9F:          //  vid -> vid
      case 0xA0 ... 0xBF:          //  sys -> vid
      case 0xC0 ... 0xDF:          //  vid -> sys
        goto breakloop;
#else
      case 0x80 ... 0x9F:          //  vid -> vid
      {
        u32 sx = list_s16[2] & 0x3FF;
        u32 sy = list_s16[3] & 0x1FF;
        u32 dx = list_s16[4] & 0x3FF;
        u32 dy = list_s16[5] & 0x1FF;
        u32 w = ((list_s16[6] - 1) & 0x3FF) + 1;
        u32 h = ((list_s16[7] - 1) & 0x1FF) + 1;

        if (sx == dx && sy == dy && psx_gpu->mask_msb == 0)
          break;

        render_block_move(psx_gpu, sx, sy, dx, dy, w, h);
        break;
      } 

      case 0xA0 ... 0xBF:          //  sys -> vid
      {
        u32 load_x = list_s16[2] & 0x3FF;
        u32 load_y = list_s16[3] & 0x1FF;
        u32 load_width = list_s16[4] & 0x3FF;
        u32 load_height = list_s16[5] & 0x1FF;
        u32 load_size = load_width * load_height;
  
        command_length += load_size / 2;

        if(load_size & 1)
          command_length++;

        render_block_copy(psx_gpu, (u16 *)&(list_s16[6]), load_x, load_y,
         load_width, load_height, load_width);
        break;
      }

      case 0xC0 ... 0xDF:          //  vid -> sys
        break;
#endif

      case 0xE1:
        set_texture(psx_gpu, list[0]);

        if ((psx_gpu->allow_dithering && (list[0] & (1 << 9)))
            || psx_gpu->force_dithering)
          psx_gpu->render_state_base |= RENDER_STATE_DITHER;
        else
          psx_gpu->render_state_base &= ~RENDER_STATE_DITHER;

        //psx_gpu->display_area_draw_enable = (list[0] >> 10) & 0x1;
        ex_regs[1] = list[0];
        break;
  
      case 0xE2:
      {
        // TODO: Clean
        u32 texture_window_settings = list[0];
        u32 tmp, x, y, w, h;

        if(texture_window_settings != psx_gpu->texture_window_settings)
        {
          tmp = (texture_window_settings & 0x1F) | 0x20;
          for(w = 8; (tmp & 1) == 0; tmp >>= 1, w <<= 1);

          tmp = ((texture_window_settings >> 5) & 0x1f) | 0x20;
          for (h = 8; (tmp & 1) == 0; tmp >>= 1, h <<= 1);

          tmp = 32 - (w >> 3);
          x = ((texture_window_settings >> 10) & tmp) << 3;

          tmp = 32 - (h >> 3);
          y = ((texture_window_settings >> 15) & tmp) << 3;

          flush_render_block_buffer(psx_gpu);
          
          psx_gpu->texture_window_settings = texture_window_settings;
          psx_gpu->texture_window_x = x;
          psx_gpu->texture_window_y = y;
          psx_gpu->texture_mask_width = w - 1;
          psx_gpu->texture_mask_height = h - 1;

          update_texture_ptr(psx_gpu);
        }
        ex_regs[2] = list[0];
        break;
      }

      case 0xE3:
      {
        s16 viewport_start_x = list[0] & 0x3FF;
        s16 viewport_start_y = (list[0] >> 10) & 0x3FF; // ReSys12: 10-bit draw area Y

        // ReSys12: compare with the draw area as programmed (split rendering
        // keeps the band-clipped range in viewport_start_y)
        if(viewport_start_x == psx_gpu->viewport_start_x &&
         viewport_start_y == psx_gpu->logical_vy0)
        {
          break;
        }

        // ReSys12: split rendering - a new draw area starts a new chunk
        // (the band is derived from it)
        if(unlikely(psx_gpu->split_mode != SPLIT_OFF) && psx_gpu->split_set)
        {
          current_command = SPLIT_BARRIER_CMD;
          goto breakloop;
        }

        psx_gpu->viewport_start_x = viewport_start_x;
        psx_gpu->logical_vy0 = viewport_start_y;
        if(psx_gpu->split_mode == SPLIT_OFF)
          psx_gpu->viewport_start_y = viewport_start_y;

#ifdef TEXTURE_CACHE_4BPP
        psx_gpu->viewport_mask =
         texture_region_mask(psx_gpu->viewport_start_x,
         psx_gpu->logical_vy0, psx_gpu->viewport_end_x,
         psx_gpu->logical_vy1);
#endif
        ex_regs[3] = list[0];
        break;
      }

      case 0xE4:
      {
        s16 viewport_end_x = list[0] & 0x3FF;
        s16 viewport_end_y = (list[0] >> 10) & 0x3FF; // ReSys12: 10-bit draw area Y

        // ReSys12: compare with the draw area as programmed (see E3)
        if(viewport_end_x == psx_gpu->viewport_end_x &&
         viewport_end_y == psx_gpu->logical_vy1)
        {
          break;
        }

        // ReSys12: split rendering - a new draw area starts a new chunk
        if(unlikely(psx_gpu->split_mode != SPLIT_OFF) && psx_gpu->split_set)
        {
          current_command = SPLIT_BARRIER_CMD;
          goto breakloop;
        }

        psx_gpu->viewport_end_x = viewport_end_x;
        psx_gpu->logical_vy1 = viewport_end_y;
        if(psx_gpu->split_mode == SPLIT_OFF)
          psx_gpu->viewport_end_y = viewport_end_y;

#ifdef TEXTURE_CACHE_4BPP
        psx_gpu->viewport_mask =
         texture_region_mask(psx_gpu->viewport_start_x,
         psx_gpu->logical_vy0, psx_gpu->viewport_end_x,
         psx_gpu->logical_vy1);
#endif
        ex_regs[4] = list[0];
        break;
      }
  
      case 0xE5:
      {
        psx_gpu->offset_x = sign_extend_11bit(list[0]);
        psx_gpu->offset_y = sign_extend_11bit(list[0] >> 11);
  
        ex_regs[5] = list[0];
        break;
      }

      case 0xE6:
      {
        u32 mask_settings = list[0];
        u16 mask_msb = mask_settings << 15;

        if(list[0] & 0x2)
          psx_gpu->render_state_base |= RENDER_STATE_MASK_EVALUATE;
        else
          psx_gpu->render_state_base &= ~RENDER_STATE_MASK_EVALUATE;

        if(mask_msb != psx_gpu->mask_msb)
        {
          flush_render_block_buffer(psx_gpu);
          psx_gpu->mask_msb = mask_msb;
        }

        ex_regs[6] = list[0];
        break;
      }
  
      default:
        break;
    }
  }

breakloop:
  *cpu_cycles_sum_out += cpu_cycles_sum;
  *cpu_cycles_last = cpu_cycles;
  *last_command = current_command;
  return list - list_start;
}

#ifdef PCSX

// this thing has become such a PITA, should just handle the 2048 width really
static void update_enhancement_buf_scanouts(psx_gpu_struct *psx_gpu,
    int x, int y, int w, int h)
{
  int max_bufs = ARRAY_SIZE(psx_gpu->enhancement_scanouts);
  struct psx_gpu_scanout *s;
  int i, sel, right, bottom;
  u32 tol_x = 48, tol_y = 16;
  u32 intersection;

  //w = (w + 15) & ~15;
  psx_gpu->saved_hres = w;
  assert(!(max_bufs & (max_bufs - 1)));
  for (i = 0; i < max_bufs; i++) {
    s = &psx_gpu->enhancement_scanouts[i];
    if (s->x == x && s->y == y && w - s->w <= tol_x && h - s->h <= tol_y)
      return;
  }

  // evict any scanout that intersects
  right = x + w;
  bottom = y + h;
  for (i = 0, sel = -1; i < max_bufs; i++) {
    s = &psx_gpu->enhancement_scanouts[i];
    if (s->x >= right) continue;
    if (s->x + s->w <= x) continue;
    if (s->y >= bottom) continue;
    if (s->y + s->h <= y) continue;
    // ... but allow upto 16 pixels intersection that some games do
    if ((intersection = s->x + s->w - x) - 1u <= tol_x) {
      s->w -= intersection;
      continue;
    }
    if ((intersection = s->y + s->h - y) - 1u <= tol_y) {
      s->h -= intersection;
      continue;
    }
    //printf("%4d%4d%4dx%d evicted\n", s->x, s->y, s->w, s->h);
    s->w = 0;
    sel = i;
    break;
  }
  if (sel >= 0) {
    // 2nd intersection check
    for (i = 0; i < max_bufs; i++) {
      s = &psx_gpu->enhancement_scanouts[i];
      if (!s->w)
        continue;
      if ((intersection = right - s->x) - 1u <= tol_x) {
        w -= intersection;
        break;
      }
      if ((intersection = bottom - s->y) - 1u <= tol_y) {
        h -= intersection;
        break;
      }
    }
  }
  else
    sel = psx_gpu->enhancement_scanout_eselect++;
  psx_gpu->enhancement_scanout_eselect &= max_bufs - 1;
  s = &psx_gpu->enhancement_scanouts[sel];
  s->x = x;
  s->y = y;
  s->w = w;
  s->h = h;

  sync_enhancement_buffers(x, y, w, h);
#if 0
  printf("scanouts:\n");
  for (i = 0; i < ARRAY_SIZE(psx_gpu->enhancement_scanouts); i++) {
    s = &psx_gpu->enhancement_scanouts[i];
    if (s->w)
      printf("%4d%4d%4dx%d\n", s->x, s->y, s->w, s->h);
  }
#endif
}

static int select_enhancement_buf_index(psx_gpu_struct *psx_gpu, s32 x, s32 y)
{
  int i;
  for (i = 0; i < ARRAY_SIZE(psx_gpu->enhancement_scanouts); i++) {
    const struct psx_gpu_scanout *s = &psx_gpu->enhancement_scanouts[i];
    if (s->x <= x && x < s->x + s->w &&
        s->y <= y && y < s->y + s->h)
      return i;
  }
  return -1;
}

#define select_enhancement_buf_by_index(psx_gpu_, i_) \
  ((psx_gpu_)->enhancement_buf_ptr + ((i_) << 20))

static void *select_enhancement_buf_ptr(psx_gpu_struct *psx_gpu, s32 x, s32 y)
{
  int i = select_enhancement_buf_index(psx_gpu, x, y);
  return i >= 0 ? select_enhancement_buf_by_index(psx_gpu, i) : NULL;
}

static void select_enhancement_buf(psx_gpu_struct *psx_gpu)
{
  s32 x = psx_gpu->saved_viewport_start_x + 16;
  s32 y = psx_gpu->saved_viewport_start_y + 16;
  psx_gpu->enhancement_current_buf_ptr = select_enhancement_buf_ptr(psx_gpu, x, y);
}

#define enhancement_disable() { \
  psx_gpu->vram_out_ptr = psx_gpu->vram_ptr; \
  psx_gpu->viewport_start_x = psx_gpu->saved_viewport_start_x; \
  psx_gpu->viewport_start_y = psx_gpu->saved_viewport_start_y; \
  psx_gpu->viewport_end_x = psx_gpu->saved_viewport_end_x; \
  psx_gpu->viewport_end_y = psx_gpu->saved_viewport_end_y; \
  psx_gpu->hacks_active = 0; \
  psx_gpu->uvrgb_phase = 0x8000; \
}

static int enhancement_enable(psx_gpu_struct *psx_gpu)
{
  if (!psx_gpu->enhancement_current_buf_ptr)
    return 0;
  psx_gpu->vram_out_ptr = psx_gpu->enhancement_current_buf_ptr;
  psx_gpu->viewport_start_x = psx_gpu->saved_viewport_start_x * 2;
  psx_gpu->viewport_start_y = psx_gpu->saved_viewport_start_y * 2;
  psx_gpu->viewport_end_x = psx_gpu->saved_viewport_end_x * 2 + 1;
  psx_gpu->viewport_end_y = psx_gpu->saved_viewport_end_y * 2 + 1;
  if (psx_gpu->viewport_end_x - psx_gpu->viewport_start_x + 1 > 1024)
    psx_gpu->viewport_end_x = psx_gpu->viewport_start_x + 1023;
  //psx_gpu->uvrgb_phase = 0x7fff;
  return 1;
}

#define shift_vertices3(v) { \
  v[0]->x <<= 1; \
  v[0]->y <<= 1; \
  v[1]->x <<= 1; \
  v[1]->y <<= 1; \
  v[2]->x <<= 1; \
  v[2]->y <<= 1; \
}

#define unshift_vertices3(v) { \
  v[0]->x >>= 1; \
  v[0]->y >>= 1; \
  v[1]->x >>= 1; \
  v[1]->y >>= 1; \
  v[2]->x >>= 1; \
  v[2]->y >>= 1; \
}

#define shift_triangle_area() \
  psx_gpu->triangle_area *= 4

#ifndef NEON_BUILD
void scale2x_tiles8(void *dst, const void *src, int w8, int h)
{
  uint16_t* d = (uint16_t*)dst;
  const uint16_t* s = (const uint16_t*)src;

  while ( h-- )
  {
    uint16_t* d_save = d;
    const uint16_t* s_save = s;
    int w = w8;

    while ( w-- )
    {
      d[    0 ] = *s;
      d[    1 ] = *s;
      d[ 1024 ] = *s;
      d[ 1025 ] = *s;
      d += 2; s++;

      d[    0 ] = *s;
      d[    1 ] = *s;
      d[ 1024 ] = *s;
      d[ 1025 ] = *s;
      d += 2; s++;

      d[    0 ] = *s;
      d[    1 ] = *s;
      d[ 1024 ] = *s;
      d[ 1025 ] = *s;
      d += 2; s++;

      d[    0 ] = *s;
      d[    1 ] = *s;
      d[ 1024 ] = *s;
      d[ 1025 ] = *s;
      d += 2; s++;

      d[    0 ] = *s;
      d[    1 ] = *s;
      d[ 1024 ] = *s;
      d[ 1025 ] = *s;
      d += 2; s++;

      d[    0 ] = *s;
      d[    1 ] = *s;
      d[ 1024 ] = *s;
      d[ 1025 ] = *s;
      d += 2; s++;

      d[    0 ] = *s;
      d[    1 ] = *s;
      d[ 1024 ] = *s;
      d[ 1025 ] = *s;
      d += 2; s++;

      d[    0 ] = *s;
      d[    1 ] = *s;
      d[ 1024 ] = *s;
      d[ 1025 ] = *s;
      d += 2; s++;
    }

    d = d_save + 2048;
    s = s_save + 1024; /* or 512? */
  }
}
#endif

// simple check for a case where no clipping is used
//  - now handled by adjusting the viewport
static int check_enhanced_range(psx_gpu_struct *psx_gpu, int x, int y)
{
  return 1;
}

static u32 uv_hack(psx_gpu_struct *psx_gpu, const vertex_struct *vertex_ptrs)
{
  int i, have_right_edge = 0, have_bottom_edge = 0, bad_u = 0, bad_v = 0;
  u32 hacks = 0;

  for (i = 0; i < 3; i++) {
    int j = (i + 1) % 3, k = (i + 2) % 3;
    int du = abs((int)vertex_ptrs[i].u - (int)vertex_ptrs[j].u);
    int dv = abs((int)vertex_ptrs[i].v - (int)vertex_ptrs[j].v);
    if (du && (du & 7) != 7)
      bad_u = 1;
    if (dv && (dv & 7) != 7)
      bad_v = 1;
    if (vertex_ptrs[i].x == vertex_ptrs[j].x && vertex_ptrs[k].x < vertex_ptrs[j].x)
      have_right_edge = 1;
    if (vertex_ptrs[i].y == vertex_ptrs[j].y)// && vertex_ptrs[k].y < vertex_ptrs[j].y)
      have_bottom_edge = 1;
  }
  if (have_right_edge && bad_u)
    hacks |= AHACK_TEXTURE_ADJ_U;
  if (have_bottom_edge && bad_v)
    hacks |= AHACK_TEXTURE_ADJ_V;
  return hacks;
}

static void do_triangle_enhanced(psx_gpu_struct *psx_gpu,
 vertex_struct *vertexes, u32 current_command)
{
  prepared_triangle triangle;

  if (!prepare_triangle(psx_gpu, vertexes, &triangle))
    return;

  if (!psx_gpu->hack_disable_main)
    render_triangle_p(psx_gpu, triangle.vertexes, current_command);

  if (!check_enhanced_range(psx_gpu, triangle.vertexes[0]->x,
        triangle.vertexes[2]->x))
    return;

  if (!enhancement_enable(psx_gpu))
    return;

  if ((current_command & RENDER_FLAGS_TEXTURE_MAP) && psx_gpu->hack_texture_adj)
    psx_gpu->hacks_active |= uv_hack(psx_gpu, vertexes);
  shift_vertices3(triangle.vertexes);
  shift_triangle_area();
  render_triangle_p(psx_gpu, triangle.vertexes, current_command);
  //unshift_vertices3(triangle.vertexes);
}

static void do_quad_enhanced(psx_gpu_struct *psx_gpu, vertex_struct *vertexes,
 u32 current_command)
{
  s16 x12_save[2] = { vertexes[1].x, vertexes[2].x };
  s16 y12_save[2] = { vertexes[1].y, vertexes[2].y };
  do_triangle_enhanced(psx_gpu, vertexes, current_command);
  enhancement_disable();
  vertexes[1].x = x12_save[0], vertexes[2].x = x12_save[1];
  vertexes[1].y = y12_save[0], vertexes[2].y = y12_save[1];
  do_triangle_enhanced(psx_gpu, &vertexes[1], current_command);
}

#if 0

#define fill_vertex(i, x_, y_, u_, v_, rgb_) \
  vertexes[i].x = x_; \
  vertexes[i].y = y_; \
  vertexes[i].u = u_; \
  vertexes[i].v = v_; \
  vertexes[i].r = rgb_; \
  vertexes[i].g = (rgb_) >> 8; \
  vertexes[i].b = (rgb_) >> 16

static void do_sprite_enhanced(psx_gpu_struct *psx_gpu, int x, int y,
 u32 u, u32 v, u32 w, u32 h, u32 cmd_rgb)
{
  vertex_struct *vertex_ptrs[3];
  u32 flags = (cmd_rgb >> 24);
  u32 color = cmd_rgb & 0xffffff;
  u32 render_state_base_saved = psx_gpu->render_state_base;
  int x1, y1;
  u8 u1, v1;

  flags &=
   (RENDER_FLAGS_MODULATE_TEXELS | RENDER_FLAGS_BLEND |
   RENDER_FLAGS_TEXTURE_MAP);

  set_triangle_color(psx_gpu, color);
  if(color == 0x808080)
    flags |= RENDER_FLAGS_MODULATE_TEXELS;

  psx_gpu->render_state_base &= ~RENDER_STATE_DITHER;
  enhancement_enable();

  x1 = x + w;
  y1 = y + h;
  u1 = u + w;
  v1 = v + h;
  // FIXME..
  if (u1 < u) u1 = 0xff;
  if (v1 < v) v1 = 0xff;

  // 0-2
  // |/
  // 1
  fill_vertex(0, x,  y,  u,  v,  color);
  fill_vertex(1, x,  y1, u,  v1, color);
  fill_vertex(2, x1, y,  u1, v,  color);
  if (prepare_triangle(psx_gpu, vertexes, vertex_ptrs)) {
    shift_vertices3(vertex_ptrs);
    shift_triangle_area();
    render_triangle_p(psx_gpu, vertex_ptrs, flags);
  }

  //   0
  //  /|
  // 1-2
  fill_vertex(0, x1, y,  u1, v,  color);
  fill_vertex(1, x,  y1, u,  v1, color);
  fill_vertex(2, x1, y1, u1, v1, color);
  if (prepare_triangle(psx_gpu, vertexes, vertex_ptrs)) {
    shift_vertices3(vertex_ptrs);
    shift_triangle_area();
    render_triangle_p(psx_gpu, vertex_ptrs, flags);
  }

  psx_gpu->render_state_base = render_state_base_saved;
}
#else
static void do_sprite_enhanced(psx_gpu_struct *psx_gpu, int x, int y,
 u32 u, u32 v, u32 w, u32 h, u32 cmd_rgb)
{
  u32 flags = (cmd_rgb >> 24);
  u32 color = cmd_rgb & 0xffffff;

  render_sprite_4x(psx_gpu, x, y, u, v, w, h, flags, color);
}
#endif

static void textured_sprite_enh(psx_gpu_struct *psx_gpu, const u32 *list,
  s32 width, s32 height, u32 *cpu_cycles_sum, u32 *cpu_cycles)
{
  s32 x = sign_extend_11bit(list[1] + psx_gpu->offset_x);
  s32 y = sign_extend_11bit((list[1] >> 16) + psx_gpu->offset_y);
  s32 width_b = width, height_b = height;
  u8 v = (list[2] >> 8) & 0xff;
  u8 u = list[2] & 0xff;

  set_clut(psx_gpu, list[2] >> 16);

  render_sprite(psx_gpu, x, y, u, v, &width, &height, list[0] >> 24, list[0]);
  gput_sum(*cpu_cycles_sum, *cpu_cycles, gput_sprite(width, height));

  if (check_enhanced_range(psx_gpu, x, x + width))
    do_sprite_enhanced(psx_gpu, x, y, u, v, width_b, height_b, list[0]);
}

u32 gpu_parse_enhanced(psx_gpu_struct *psx_gpu, u32 *list, u32 size, u32 *ex_regs,
 s32 *cpu_cycles_sum_out, s32 *cpu_cycles_last, u32 *last_command)
{
  vertex_struct vertexes[4] __attribute__((aligned(16))) = {};
  u32 current_command = 0, command_length;
  u32 cpu_cycles_sum = 0, cpu_cycles = *cpu_cycles_last;
  u32 simplified_prim[4*4];

  u32 *list_start = list;
  u32 *list_end = list + (size / 4);

  psx_gpu->saved_viewport_start_x = psx_gpu->viewport_start_x;
  psx_gpu->saved_viewport_start_y = psx_gpu->viewport_start_y;
  psx_gpu->saved_viewport_end_x = psx_gpu->viewport_end_x;
  psx_gpu->saved_viewport_end_y = psx_gpu->viewport_end_y;
  select_enhancement_buf(psx_gpu);

  for(; list < list_end; list += 1 + command_length)
  {
    s16 *list_s16 = (void *)list;
    current_command = *list >> 24;
    command_length = command_lengths[current_command];
    if (list + 1 + command_length > list_end) {
      current_command = (u32)-1;
      break;
    }

    enhancement_disable();

    switch(current_command)
    {
      case 0x00:
        break;
  
      case 0x02:
      {
        u32 x = list_s16[2] & 0x3FF;
        u32 y = list_s16[3] & 0x1FF;
        u32 width = list_s16[4] & 0x3FF;
        u32 height = list_s16[5] & 0x1FF;
        u32 color = list[0] & 0xFFFFFF;
        s32 i1, i2;

        x &= ~0xF;
        width = ((width + 0xF) & ~0xF);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_fill(width, height));
        if (width == 0 || height == 0)
          break;

        do_fill(psx_gpu, x, y, width, height, color);

        i1 = select_enhancement_buf_index(psx_gpu, x, y);
        i2 = select_enhancement_buf_index(psx_gpu, x + width - 1, y + height - 1);
        if (i1 < 0 || i1 != i2) {
          sync_enhancement_buffers(x, y, width, height);
          break;
        }

        psx_gpu->vram_out_ptr = select_enhancement_buf_by_index(psx_gpu, i1);
        x *= 2;
        y *= 2;
        width *= 2;
        height *= 2;
        render_block_fill_enh(psx_gpu, color, x, y, width, height);
        break;
      }
  
      case 0x20 ... 0x23:
      {
        set_triangle_color(psx_gpu, list[0] & 0xFFFFFF);
  
        get_vertex_data_xy(0, 2);
        get_vertex_data_xy(1, 4);
        get_vertex_data_xy(2, 6);

        do_triangle_enhanced(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base());
        break;
      }
  
      case 0x24 ... 0x27:
      {
        set_clut(psx_gpu, list_s16[5]);
        set_texture(psx_gpu, list_s16[9]);
        set_triangle_color(psx_gpu, list[0] & 0xFFFFFF);
  
        get_vertex_data_xy_uv(0, 2);
        get_vertex_data_xy_uv(1, 6);
        get_vertex_data_xy_uv(2, 10);
  
        do_triangle_enhanced(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_t());
        break;
      }
  
      case 0x28 ... 0x2B:
      {
        set_triangle_color(psx_gpu, list[0] & 0xFFFFFF);
  
        get_vertex_data_xy(0, 2);
        get_vertex_data_xy(1, 4);
        get_vertex_data_xy(2, 6);
        get_vertex_data_xy(3, 8);

        do_quad_enhanced(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base());
        break;
      }
  
      case 0x2C ... 0x2F:
      {
        u32 i, simplified_count;
        set_texture(psx_gpu, list[4] >> 16);
        if (!(psx_gpu->render_state_base & RENDER_STATE_DITHER) &&
            (simplified_count = prim_try_simplify_quad_t(simplified_prim, list)))
        {
          for (i = 0; i < simplified_count; i++) {
            const u32 *list_ = &simplified_prim[i * 4];
            textured_sprite_enh(psx_gpu, list_, list_[3] & 0x3FF,
              (list_[3] >> 16) & 0x1FF, &cpu_cycles_sum, &cpu_cycles);
          }
          break;
        }

        set_clut(psx_gpu, list[2] >> 16);
        set_triangle_color(psx_gpu, list[0] & 0xFFFFFF);
  
        get_vertex_data_xy_uv(0, 2);   
        get_vertex_data_xy_uv(1, 6);   
        get_vertex_data_xy_uv(2, 10);  
        get_vertex_data_xy_uv(3, 14);
  
        do_quad_enhanced(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_t());
        break;
      }
  
      case 0x30 ... 0x33:
      {
        get_vertex_data_xy_rgb(0, 0);
        get_vertex_data_xy_rgb(1, 4);
        get_vertex_data_xy_rgb(2, 8);
  
        do_triangle_enhanced(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_g());
        break;
      }
  
      case 0x34 ... 0x37:
      {
        set_clut(psx_gpu, list_s16[5]);
        set_texture(psx_gpu, list_s16[11]);
  
        get_vertex_data_xy_uv_rgb(0, 0);
        get_vertex_data_xy_uv_rgb(1, 6);
        get_vertex_data_xy_uv_rgb(2, 12);

        do_triangle_enhanced(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_gt());
        break;
      }
  
      case 0x38 ... 0x3B:
      {
        get_vertex_data_xy_rgb(0, 0);
        get_vertex_data_xy_rgb(1, 4);
        get_vertex_data_xy_rgb(2, 8);
        get_vertex_data_xy_rgb(3, 12);
  
        do_quad_enhanced(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_g());
        break;
      }
  
      case 0x3C ... 0x3F:
      {
        u32 i, simplified_count;
        set_texture(psx_gpu, list[5] >> 16);
        if (!(psx_gpu->render_state_base & RENDER_STATE_DITHER) &&
            (simplified_count = prim_try_simplify_quad_gt(simplified_prim, list)))
        {
          for (i = 0; i < simplified_count; i++) {
            const u32 *list_ = &simplified_prim[i * 4];
            textured_sprite_enh(psx_gpu, list_, list_[3] & 0x3FF,
              (list_[3] >> 16) & 0x1FF, &cpu_cycles_sum, &cpu_cycles);
          }
          break;
        }

        set_clut(psx_gpu, list[2] >> 16);
  
        get_vertex_data_xy_uv_rgb(0, 0);
        get_vertex_data_xy_uv_rgb(1, 6);
        get_vertex_data_xy_uv_rgb(2, 12);
        get_vertex_data_xy_uv_rgb(3, 18);

        do_quad_enhanced(psx_gpu, vertexes, current_command);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_gt());
        break;
      }
  
      case 0x40 ... 0x47:
      {
        vertexes[0].x = list_s16[2] + psx_gpu->offset_x;
        vertexes[0].y = list_s16[3] + psx_gpu->offset_y;
        vertexes[1].x = list_s16[4] + psx_gpu->offset_x;
        vertexes[1].y = list_s16[5] + psx_gpu->offset_y;

        render_line(psx_gpu, vertexes, current_command, list[0], 0);
        if (enhancement_enable(psx_gpu))
          render_line(psx_gpu, vertexes, current_command, list[0], 1);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));
        break;
      }
  
      case 0x48 ... 0x4F:
      {
        u32 num_vertexes = 1;
        u32 *list_position = &(list[2]);
        u32 xy = list[1];

        vertexes[1].x = (xy & 0xFFFF) + psx_gpu->offset_x;
        vertexes[1].y = (xy >> 16) + psx_gpu->offset_y;
      
        xy = *list_position;
        while(1)
        {
          vertexes[0] = vertexes[1];

          vertexes[1].x = (xy & 0xFFFF) + psx_gpu->offset_x;
          vertexes[1].y = (xy >> 16) + psx_gpu->offset_y;

          enhancement_disable();
          render_line(psx_gpu, vertexes, current_command, list[0], 0);
          if (enhancement_enable(psx_gpu))
            render_line(psx_gpu, vertexes, current_command, list[0], 1);
          gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));

          list_position++;
          num_vertexes++;

          if(list_position >= list_end)
          {
            current_command = (u32)-1;
            goto breakloop;
          }

          xy = *list_position;
          if((xy & 0xF000F000) == 0x50005000)
            break;
        }

        command_length += (num_vertexes - 2);
        break;
      }
  
      case 0x50 ... 0x57:
      {
        vertexes[0].r = list[0] & 0xFF;
        vertexes[0].g = (list[0] >> 8) & 0xFF;
        vertexes[0].b = (list[0] >> 16) & 0xFF;
        vertexes[0].x = list_s16[2] + psx_gpu->offset_x;
        vertexes[0].y = list_s16[3] + psx_gpu->offset_y;

        vertexes[1].r = list[2] & 0xFF;
        vertexes[1].g = (list[2] >> 8) & 0xFF;
        vertexes[1].b = (list[2] >> 16) & 0xFF;
        vertexes[1].x = list_s16[6] + psx_gpu->offset_x;
        vertexes[1].y = list_s16[7] + psx_gpu->offset_y;

        render_line(psx_gpu, vertexes, current_command, 0, 0);
        if (enhancement_enable(psx_gpu))
          render_line(psx_gpu, vertexes, current_command, 0, 1);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));
        break;
      }
 
      case 0x58 ... 0x5F:
      {
        u32 num_vertexes = 1;
        u32 *list_position = &(list[2]);
        u32 color = list[0];
        u32 xy = list[1];

        vertexes[1].r = color & 0xFF;
        vertexes[1].g = (color >> 8) & 0xFF;
        vertexes[1].b = (color >> 16) & 0xFF;
        vertexes[1].x = (xy & 0xFFFF) + psx_gpu->offset_x;
        vertexes[1].y = (xy >> 16) + psx_gpu->offset_y;
      
        color = list_position[0];
        while(1)
        {
          xy = list_position[1];

          vertexes[0] = vertexes[1];

          vertexes[1].r = color & 0xFF;
          vertexes[1].g = (color >> 8) & 0xFF;
          vertexes[1].b = (color >> 16) & 0xFF;
          vertexes[1].x = (xy & 0xFFFF) + psx_gpu->offset_x;
          vertexes[1].y = (xy >> 16) + psx_gpu->offset_y;

          enhancement_disable();
          render_line(psx_gpu, vertexes, current_command, 0, 0);
          if (enhancement_enable(psx_gpu))
            render_line(psx_gpu, vertexes, current_command, 0, 1);
          gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));

          list_position += 2;
          num_vertexes++;

          if(list_position >= list_end)
          {
            current_command = (u32)-1;
            goto breakloop;
          }

          color = list_position[0];
          if((color & 0xF000F000) == 0x50005000)
            break;
        }

        command_length += ((num_vertexes - 2) * 2);
        break;
      }
  
      case 0x60 ... 0x63:
      {        
        u32 x = sign_extend_11bit(list_s16[2] + psx_gpu->offset_x);
        u32 y = sign_extend_11bit(list_s16[3] + psx_gpu->offset_y);
        s32 width = list_s16[4] & 0x3FF;
        s32 height = list_s16[5] & 0x1FF;

        render_sprite(psx_gpu, x, y, 0, 0, &width, &height,
           current_command, list[0]);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(width, height));

        if (check_enhanced_range(psx_gpu, x, x + width)) {
          width = list_s16[4] & 0x3FF;
          height = list_s16[5] & 0x1FF;
          do_sprite_enhanced(psx_gpu, x, y, 0, 0, width, height, list[0]);
        }
        break;
      }

      case 0x64 ... 0x67:
        textured_sprite_enh(psx_gpu, list, list[3] & 0x3FF, (list[3] >> 16) & 0x1FF,
          &cpu_cycles_sum, &cpu_cycles);
        break;

      case 0x68 ... 0x6B:
      {
        s32 x = sign_extend_11bit(list_s16[2] + psx_gpu->offset_x);
        s32 y = sign_extend_11bit(list_s16[3] + psx_gpu->offset_y);
        s32 width = 1, height = 1;

        render_sprite(psx_gpu, x, y, 0, 0, &width, &height,
           current_command, list[0]);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(1, 1));

        if (check_enhanced_range(psx_gpu, x, x + 1))
          do_sprite_enhanced(psx_gpu, x, y, 0, 0, 1, 1, list[0]);
        break;
      }
  
      case 0x70 ... 0x73:
      {        
        s32 x = sign_extend_11bit(list_s16[2] + psx_gpu->offset_x);
        s32 y = sign_extend_11bit(list_s16[3] + psx_gpu->offset_y);
        s32 width = 8, height = 8;

        render_sprite(psx_gpu, x, y, 0, 0, &width, &height,
           current_command, list[0]);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(width, height));

        if (check_enhanced_range(psx_gpu, x, x + 8))
          do_sprite_enhanced(psx_gpu, x, y, 0, 0, 8, 8, list[0]);
        break;
      }

      case 0x74 ... 0x77:
        textured_sprite_enh(psx_gpu, list, 8, 8, &cpu_cycles_sum, &cpu_cycles);
        break;

      case 0x78 ... 0x7B:
      {        
        s32 x = sign_extend_11bit(list_s16[2] + psx_gpu->offset_x);
        s32 y = sign_extend_11bit(list_s16[3] + psx_gpu->offset_y);
        s32 width = 16, height = 16;

        render_sprite(psx_gpu, x, y, 0, 0, &width, &height,
           current_command, list[0]);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(width, height));

        if (check_enhanced_range(psx_gpu, x, x + 16))
          do_sprite_enhanced(psx_gpu, x, y, 0, 0, 16, 16, list[0]);
        break;
      }

      case 0x7C ... 0x7F:
        textured_sprite_enh(psx_gpu, list, 16, 16, &cpu_cycles_sum, &cpu_cycles);
        break;

      case 0x80 ... 0x9F:          //  vid -> vid
      case 0xA0 ... 0xBF:          //  sys -> vid
      case 0xC0 ... 0xDF:          //  vid -> sys
        goto breakloop;

      case 0xE1:
        set_texture(psx_gpu, list[0]);

        if ((psx_gpu->allow_dithering && (list[0] & (1 << 9)))
            || psx_gpu->force_dithering)
          psx_gpu->render_state_base |= RENDER_STATE_DITHER;
        else
          psx_gpu->render_state_base &= ~RENDER_STATE_DITHER;

        //psx_gpu->display_area_draw_enable = (list[0] >> 10) & 0x1;
        ex_regs[1] = list[0];
        break;
  
      case 0xE2:
      {
        // TODO: Clean
        u32 texture_window_settings = list[0];
        u32 tmp, x, y, w, h;

        if(texture_window_settings != psx_gpu->texture_window_settings)
        {
          tmp = (texture_window_settings & 0x1F) | 0x20;
          for(w = 8; (tmp & 1) == 0; tmp >>= 1, w <<= 1);

          tmp = ((texture_window_settings >> 5) & 0x1f) | 0x20;
          for (h = 8; (tmp & 1) == 0; tmp >>= 1, h <<= 1);

          tmp = 32 - (w >> 3);
          x = ((texture_window_settings >> 10) & tmp) << 3;

          tmp = 32 - (h >> 3);
          y = ((texture_window_settings >> 15) & tmp) << 3;

          flush_render_block_buffer(psx_gpu);
          
          psx_gpu->texture_window_settings = texture_window_settings;
          psx_gpu->texture_window_x = x;
          psx_gpu->texture_window_y = y;
          psx_gpu->texture_mask_width = w - 1;
          psx_gpu->texture_mask_height = h - 1;

          update_texture_ptr(psx_gpu);
        }
        ex_regs[2] = list[0];
        break;
      }
  
      case 0xE3:
      {
        s16 viewport_start_x = list[0] & 0x3FF;
        s16 viewport_start_y = (list[0] >> 10) & 0x1FF;

        if(viewport_start_x == psx_gpu->viewport_start_x &&
         viewport_start_y == psx_gpu->viewport_start_y)
        {
          break;
        }
        psx_gpu->viewport_start_x = viewport_start_x;
        psx_gpu->viewport_start_y = viewport_start_y;
        psx_gpu->saved_viewport_start_x = viewport_start_x;
        psx_gpu->saved_viewport_start_y = viewport_start_y;

        select_enhancement_buf(psx_gpu);

#ifdef TEXTURE_CACHE_4BPP
        psx_gpu->viewport_mask =
         texture_region_mask(psx_gpu->viewport_start_x,
         psx_gpu->viewport_start_y, psx_gpu->viewport_end_x,
         psx_gpu->viewport_end_y);
#endif
        ex_regs[3] = list[0];
        break;
      }

      case 0xE4:
      {
        s16 viewport_end_x = list[0] & 0x3FF;
        s16 viewport_end_y = (list[0] >> 10) & 0x1FF;

        if(viewport_end_x == psx_gpu->viewport_end_x &&
         viewport_end_y == psx_gpu->viewport_end_y)
        {
          break;
        }

        psx_gpu->viewport_end_x = viewport_end_x;
        psx_gpu->viewport_end_y = viewport_end_y;
        psx_gpu->saved_viewport_end_x = viewport_end_x;
        psx_gpu->saved_viewport_end_y = viewport_end_y;

        // needed for multithreaded mode where the main thread will start
        // scanout if it sees no intersect with the latest draw area
        flush_render_block_buffer(psx_gpu);

        select_enhancement_buf(psx_gpu);
#if 0
        if (!psx_gpu->enhancement_current_buf_ptr)
          log_anomaly("vp %3d,%3d %3d,%d - no buf\n",
              psx_gpu->viewport_start_x, psx_gpu->viewport_start_y,
              viewport_end_x, viewport_end_y);
#endif
#ifdef TEXTURE_CACHE_4BPP
        psx_gpu->viewport_mask =
         texture_region_mask(psx_gpu->viewport_start_x,
         psx_gpu->viewport_start_y, psx_gpu->viewport_end_x,
         psx_gpu->viewport_end_y);
#endif
        ex_regs[4] = list[0];
        break;
      }
  
      case 0xE5:
      {
        psx_gpu->offset_x = sign_extend_11bit(list[0]);
        psx_gpu->offset_y = sign_extend_11bit(list[0] >> 11);
  
        ex_regs[5] = list[0];
        break;
      }

      case 0xE6:
      {
        u32 mask_settings = list[0];
        u16 mask_msb = mask_settings << 15;

        if(list[0] & 0x2)
          psx_gpu->render_state_base |= RENDER_STATE_MASK_EVALUATE;
        else
          psx_gpu->render_state_base &= ~RENDER_STATE_MASK_EVALUATE;

        if(mask_msb != psx_gpu->mask_msb)
        {
          flush_render_block_buffer(psx_gpu);
          psx_gpu->mask_msb = mask_msb;
        }

        ex_regs[6] = list[0];
        break;
      }
  
      default:
        break;
    }
  }

  enhancement_disable();

breakloop:
  *cpu_cycles_sum_out += cpu_cycles_sum;
  *cpu_cycles_last = cpu_cycles;
  *last_command = current_command;
  return list - list_start;
}

#endif /* PCSX */

// vim:ts=2:shiftwidth=2:expandtab

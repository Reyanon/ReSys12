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

#ifndef PSX_GPU_H
#define PSX_GPU_H

#define MAX_SPANS             512
#define MAX_BLOCKS            64
#define MAX_BLOCKS_PER_ROW    128

#define SPAN_DATA_BLOCKS_SIZE 32

#define AHACK_TEXTURE_ADJ_U   (1 << 0)
#define AHACK_TEXTURE_ADJ_V   (1 << 1)

#ifndef __ASSEMBLER__

#include "vector_types.h"

#ifndef unlikely
#define unlikely(x) __builtin_expect((x), 0)
#endif

#define sign_extend_11bit(value) \
  (((s32)((value) << 21)) >> 21)

typedef enum
{
  PRIMITIVE_TYPE_TRIANGLE = 0,
  PRIMITIVE_TYPE_SPRITE = 1,
  PRIMITIVE_TYPE_LINE = 2,
  PRIMITIVE_TYPE_UNKNOWN = 3
} primitive_type_enum;

typedef enum
{
  TEXTURE_MODE_4BPP  = 0,
  TEXTURE_MODE_8BPP  = 1,
  TEXTURE_MODE_16BPP = 2
} texture_mode_enum;

typedef enum
{
  BLEND_MODE_AVERAGE    = 0,
  BLEND_MODE_ADD        = 1,
  BLEND_MODE_SUBTRACT   = 2,
  BLEND_MODE_ADD_FOURTH = 3
} blend_mode_enum;

typedef enum
{
  RENDER_FLAGS_MODULATE_TEXELS = 0x1,
  RENDER_FLAGS_BLEND           = 0x2,
  RENDER_FLAGS_TEXTURE_MAP     = 0x4,
  RENDER_FLAGS_QUAD            = 0x8,
  RENDER_FLAGS_SHADE           = 0x10,
} render_flags_enum;

typedef enum
{
  RENDER_STATE_DITHER          = 0x8,
  RENDER_STATE_MASK_EVALUATE   = 0x20,
} render_state_enum;

typedef enum
{
  RENDER_INTERLACE_ENABLED     = 0x1,
  RENDER_INTERLACE_ODD         = 0x2,
} render_mode_enum;

typedef struct
{
  u16 left_x;
  u16 num_blocks;
  u16 right_mask;
  u16 y;
} edge_data_struct;

// 64 (72) bytes total
typedef struct
{
  // 16 bytes
  union
  {
    vec_8x16u uv;
    vec_8x16u texels;
    vec_8x16u draw_mask;
  };

  // 24 bytes
  union
  {
    struct
    {
      vec_8x8u r;
      vec_8x8u g;
      vec_8x8u b;
    };

    vec_8x16u pixels;
  };

  // 8 (16) bytes
  u32 draw_mask_bits;
  u16 *fb_ptr;

  // 16 bytes
  vec_8x16u dither_offsets;  
} block_struct;

typedef struct render_block_handler_struct render_block_handler_struct;

typedef struct
{
  // 144 bytes
  vec_8x16u test_mask;

  vec_4x32u uvrg;
  vec_4x32u uvrg_dx;
  vec_4x32u uvrg_dy;

  vec_4x32u u_block_span;
  vec_4x32u v_block_span;
  vec_4x32u r_block_span;
  vec_4x32u g_block_span;
  vec_4x32u b_block_span;

  u32 b;
  u32 b_dy;

  u32 triangle_area;

  u32 texture_window_settings;
  // ReSys12: the texture page masks moved below (u64, after the enhancement
  // fields) so the halfword fields here stay within ldrh immediate range.

  u32 triangle_color;
  u32 dither_table[4];

  struct render_block_handler_struct *render_block_handler;
  void *texture_page_ptr;
  void *texture_page_base;
  u16 *clut_ptr;
  u16 *vram_ptr;
  u16 *vram_out_ptr;

  u32 uvrgb_phase;

  u16 render_state_base;
  u16 render_state;

  u16 num_spans;
  u16 num_blocks;

  s16 viewport_start_x;
  s16 viewport_start_y;
  s16 viewport_end_x;
  s16 viewport_end_y;

  u16 mask_msb;

  u8 triangle_winding;

  u8 unused;

  u8 current_texture_page;
  u8 last_8bpp_texture_page;

  u8 texture_mask_width;
  u8 texture_mask_height;
  u8 texture_window_x;
  u8 texture_window_y;

  u8 primitive_type;
  u8 render_mode;

  s16 offset_x;
  s16 offset_y;

  u16 clut_settings;
  u16 texture_settings;

  u32 *reciprocal_table_ptr;

  // enhancement stuff
  u16 *enhancement_buf_ptr;          // main alloc
  u16 *enhancement_current_buf_ptr;  // offset into above, 4 bufs
  u32 hacks_active;                  // AHACK_TEXTURE_ADJ_U ...
  u32 saved_hres;
  s16 saved_viewport_start_x;
  s16 saved_viewport_start_y;
  s16 saved_viewport_end_x;
  s16 saved_viewport_end_y;
  struct psx_gpu_scanout {
    u16 x, y, w, h;
  } enhancement_scanouts[4];         // 0-3 specifying which buf to use
  u16 enhancement_scanout_eselect;   // eviction selector
  u16 enhancement_current_buf;

  u32 allow_dithering:1;
  u32 force_dithering:1;
  u32 hack_disable_main:1;
  u32 hack_texture_adj:1;

  // ReSys12: System 12 has 2MB VRAM (1024 lines) = 64 texture pages
  // (16 columns x 4 rows of 256 lines), so the page masks are 64-bit.
  // Bit = page = (x / 64) + (y / 256) * 16; one page row per 16-bit lane.
  // The asm tests them as two 32-bit halves (rows 0-1 / rows 2-3).
  u64 current_texture_mask;
  u64 viewport_mask;
  u64 dirty_textures_4bpp_mask;
  u64 dirty_textures_8bpp_mask;
  u64 dirty_textures_8bpp_alternate_mask;

  // Align up to 64 byte boundary to keep the upcoming buffers cache line
  // aligned, also make reachable with single immediate addition
  // ReSys12: - 20 for the five masks widened to u64 (blocks stays at 0x200)
  u8 reserved_a[68 + 9*4 - 9*sizeof(void *) - 20];

  // space for saving regs on c call to flush_render_block_buffer() and asm
  u32 saved_tmp[48 / sizeof(u32)];
  u32 saved_q4_q7[64 / sizeof(u32)];

  // 8KB
  block_struct blocks[MAX_BLOCKS_PER_ROW];

  // 14336 bytes
  vec_4x32u span_uvrg_offset[MAX_SPANS];
  edge_data_struct span_edge_data[MAX_SPANS];
  u32 span_b_offset[MAX_SPANS];

  // ReSys12: split rendering. Two instances parse the same command lists and
  // each writes only its own band of VRAM rows (top: y < split_y, bottom:
  // y >= split_y). viewport_start_y/viewport_end_y then hold the draw area
  // clipped to the band; logical_vy0/vy1 keep the draw area as programmed
  // (used for E3/E4 compares, the render-to-texture mask and the split).
  s16 logical_vy0;
  s16 logical_vy1;
  u8 split_mode;                     // SPLIT_OFF / SPLIT_TOP / SPLIT_BOTTOM
  u8 split_set;                      // band chosen for the current chunk
  u8 split_solo;                     // chunk drawn by the top instance alone
  u8 split_nw;                       // used entries of split_w
  u16 split_ratio;                   // top band share of the draw area, /256
  u16 split_nc;                      // used entries of split_c
  s32 split_y;                       // first row of the bottom band
  u64 split_read_pages;              // texture pages read in this chunk
  s16 split_w[4][4];                 // rects written in this chunk (x0,y0,x1,y1)
  s16 split_c[16][4];                // CLUT rects read in this chunk
  u8 split_refresh;                  // a texture page next to this chunk's writes was read (see split_check)
  u32 split_key;                     // draw area of the current chunk (split_ratio_tab)
  u32 cmd_seq;                       // commands parsed so far (the same in both split instances at a command)
  u32 tex8_done[64][2];              // owner: cmd_seq of the last 8bpp cache refresh of each page's two slices
  volatile u32 *act_ptr;             // sampling profiler marker (per instance)
  u16 dirty8_rows[2][64];            // 8bpp cache: dirty 16-line tile rows of each page slice, per layout (page & 1)
  u16 dirty4_rows[64];               // 4bpp cache: dirty 16-line tile rows of each page
  u8 rect_flip;                      // textured rectangles: bit 0 = x flip, bit 1 = y flip (tpage bits 12/13)
  u32 split_ver;                     // bumped when the chunk's write / CLUT rects change (split_check memo)
  u32 split_fast_ver;                // memo: split_ver, texture page, CLUT, texture window and draw area of
  u32 split_fast_tpage;              //  the last textured polygon split_check passed
  u32 split_fast_clut;
  u32 split_fast_tw;
  s32 split_fast_wr[4];
  // Instance whose texture caches are used. The second split instance shares
  // the first one's: cache contents are a function of VRAM only, and both
  // instances only ever store the same values (an instance may refresh a page
  // the other has already refreshed), so sharing halves the cache footprint
  // in L2 and saves ~8MB. Allocations of the second instance end here.
  void *cache_owner;

  // ReSys12: 64 texture pages for the 1024-line VRAM (was 32 / 16 / 16)
  u8 texture_4bpp_cache[64][256 * 256] __attribute__((aligned(64)));
  u8 texture_8bpp_even_cache[32][256 * 256];
  u8 texture_8bpp_odd_cache[32][256 * 256];
} psx_gpu_struct;

// ReSys12: texture caches of an instance (see cache_owner)
#define TEXCACHE(psx_gpu_) \
  ((psx_gpu_)->cache_owner ? (psx_gpu_struct *)(psx_gpu_)->cache_owner : (psx_gpu_))

// ReSys12: split rendering band ratios per draw area (psx_gpu_parse.c)
#define SPLIT_RATIO_TAB 8
typedef struct
{
  u32 key;                           // draw area, 0 = free
  u16 ratio;                         // top band share, /256
  u16 used;                          // for replacement
  u32 sum_a, sum_b, n;               // recent chunks: end of each instance (us)
} split_ratio_entry;
extern split_ratio_entry rs12_split_ratio_tab[SPLIT_RATIO_TAB];

// ReSys12: split rendering modes and the gpu_parse stop code for a chunk
// boundary (both instances must join before the next command)
#define SPLIT_OFF     0
#define SPLIT_TOP     1
#define SPLIT_BOTTOM  2
#define SPLIT_BARRIER_CMD 0x1000u

typedef struct __attribute__((aligned(16)))
{
  u8 u;
  u8 v;

  u8 r;
  u8 g;
  u8 b;

  u8 reserved[3];

  s16 x;
  s16 y;

  u32 padding;
} vertex_struct;

typedef struct
{
  vertex_struct *vertexes[3];
  s16 offset_x;
  s16 offset_y;
} prepared_triangle;

void render_block_fill(psx_gpu_struct *psx_gpu, u32 color, u32 x, u32 y,
 u32 width, u32 height);
void render_block_copy(psx_gpu_struct *psx_gpu, u16 *source, u32 x, u32 y,
 u32 width, u32 height, u32 pitch);
void render_block_move(psx_gpu_struct *psx_gpu, u32 source_x, u32 source_y,
 u32 dest_x, u32 dest_y, u32 width, u32 height);

void render_triangle(psx_gpu_struct *psx_gpu, vertex_struct *vertexes,
 u32 flags);
void render_sprite(psx_gpu_struct *psx_gpu, s32 x, s32 y, u32 u, u32 v,
 s32 *width, s32 *height, u32 flags, u32 color);
void render_line(psx_gpu_struct *gpu, vertex_struct *vertexes, u32 flags,
 u32 color, int double_resolution);

u64 texture_region_mask(s32 x1, s32 y1, s32 x2, s32 y2); // ReSys12: 64 pages

void update_texture_8bpp_cache(psx_gpu_struct *psx_gpu);
void flush_render_block_buffer(psx_gpu_struct *psx_gpu);

void setup_blocks_uv_adj_hack(psx_gpu_struct *psx_gpu, block_struct *block,
    edge_data_struct *span_edge_data, vec_4x32u *span_uvrg_offset);

void initialize_psx_gpu(psx_gpu_struct *psx_gpu, u16 *vram);
u32 gpu_parse(psx_gpu_struct *psx_gpu, u32 *list, u32 size, u32 *ex_regs,
 s32 *cpu_cycles_sum_out, s32 *cpu_cycles_last, u32 *last_command);

void triangle_benchmark(psx_gpu_struct *psx_gpu);

void compute_all_gradients(psx_gpu_struct * __restrict__ psx_gpu,
 const vertex_struct * __restrict__ a, const vertex_struct * __restrict__ b,
 const vertex_struct * __restrict__ c);

#endif // __ASSEMBLER__
#endif // PSX_GPU_H

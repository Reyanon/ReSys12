/*
 * (C) Gražvydas "notaz" Ignotas, 2011
 *
 * This work is licensed under the terms of any of these licenses
 * (at your option):
 *  - GNU GPL, version 2 or later.
 *  - GNU LGPL, version 2.1 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include "../../include/compiler_features.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

extern const unsigned char cmd_lengths[256];
#define command_lengths cmd_lengths

static int initialized;

#define PCSX

static noinline void sync_enhancement_buffers(int x, int y, int w, int h);

#include "../gpulib/gpu.h"
#include "psx_gpu/psx_gpu.c"
#include "psx_gpu/psx_gpu_parse.c"

static psx_gpu_struct egpu __attribute__((aligned(256)));

// ReSys12: split rendering (two CPU cores). A second renderer
// instance (egpu_b) parses the same command lists as egpu; egpu draws the top
// band of rows, egpu_b the bottom band (psx_gpu_parse.c split_check). On the
// Vita egpu_b runs on a helper thread on CPU1 (shim/rs12_gpu_split_vita.c);
// elsewhere both run one after the other on the calling thread, which gives
// the same VRAM contents (used by the PC/ARM harness to check correctness).
static psx_gpu_struct *egpu_b;
static void *egpu_b_alloc;
static volatile int split_request = -1;  // -1 = no change pending
static int split_active;
volatile u32 rs12_gpu_act_b;             // sampling profiler marker of egpu_b
// statistics (read by the frontend perf line)
volatile uint64_t rs12_gpu_split_busy_us; // helper thread busy time
volatile uint64_t rs12_gpu_split_a_us;    // renderer thread: own band (threaded chunks)
volatile uint64_t rs12_gpu_split_join_us; // renderer thread: waiting for / running the helper's job
volatile uint32_t rs12_gpu_split_chunks, rs12_gpu_split_barriers;
volatile uint32_t rs12_gpu_split_solos, rs12_gpu_split_stolen;
volatile uint32_t rs12_gpu_split_mismatch;
volatile uint32_t rs12_gpu_split_ratio = 128;

#ifdef __vita__
// shim/rs12_gpu_split_vita.c
int rs12_split_worker_start(void (*job)(void));
void rs12_split_worker_kick(void);
int rs12_split_worker_finish(uint64_t *job_us);
uint64_t rs12_split_now_us(void);
#define SPLIT_THREADED 1
#else
#define SPLIT_THREADED 0
#endif
static int split_threaded, split_no_thread;

// the job of egpu_b: parse one chunk, write everything out
static struct {
  uint32_t *list;
  int count;
  uint32_t ex_regs[8];
  int ret;
  u32 cmd;
} split_job;

static void split_job_run(void)
{
  s32 dummy_sum = 0, dummy_last = 0;
  split_job.ret = gpu_parse(egpu_b, split_job.list, split_job.count * 4,
    split_job.ex_regs, &dummy_sum, &dummy_last, &split_job.cmd);
  flush_render_block_buffer(egpu_b);
  rs12_gpu_act_b = 0x1ff; // idle, for the sampling profiler
}

// enable: 0 = single instance, 1 = split (helper thread where available),
// 2 = split, both halves on the rendering thread (debug/measurement)
void rs12_gpu_split_enable(int enable)
{
  split_no_thread = enable == 2;
  split_request = enable ? 1 : 0;
}

void rs12_gpu_split_set_ratio(int ratio)
{
  if (ratio < 1) ratio = 1;
  if (ratio > 255) ratio = 255;
  rs12_gpu_split_ratio = ratio;
}

int rs12_gpu_split_active(void)
{
  return split_active;
}

// ReSys12: for the harness VRAM hash
const void *rs12_gpu_vram_ptr(void)
{
  return gpu.vram;
}

// apply a pending enable/disable; runs on the thread that renders
static void split_update(void)
{
  const int req = split_request;
  if (req < 0 || !initialized)
    return;
  split_request = -1;
  if (req == split_active)
    return;
  flush_render_block_buffer(&egpu);
  if (req) {
    // the second instance has no texture caches of its own (it uses egpu's,
    // see cache_owner in psx_gpu.h), so only the part before them is allocated
    const size_t size_b = offsetof(psx_gpu_struct, texture_4bpp_cache);
    if (!egpu_b) {
      egpu_b_alloc = malloc(size_b + 256);
      if (!egpu_b_alloc) {
        SysPrintf("gpu split: no memory for the second renderer\n");
        return;
      }
      egpu_b = (psx_gpu_struct *)(((uintptr_t)egpu_b_alloc + 255) & ~(uintptr_t)255);
    }
    // same state as egpu
    memcpy(egpu_b, &egpu, size_b);
    egpu_b->act_ptr = &rs12_gpu_act_b;
    egpu_b->cache_owner = &egpu;
    egpu.split_mode = SPLIT_TOP;
    egpu_b->split_mode = SPLIT_BOTTOM;
#if SPLIT_THREADED
    if (!split_threaded)
      split_threaded = rs12_split_worker_start(split_job_run);
#endif
    split_active = 1;
  }
  else {
    flush_render_block_buffer(egpu_b);
    egpu.split_mode = SPLIT_OFF;
    egpu.viewport_start_y = egpu.logical_vy0;
    egpu.viewport_end_y = egpu.logical_vy1;
    split_active = 0;
  }
}

// ReSys12: the ratio entry of a draw area (rendering thread, between chunks); a new area replaces the least used
static split_ratio_entry *split_ratio_entry_for(u32 key)
{
  split_ratio_entry *e = &rs12_split_ratio_tab[0];
  int i;
  for (i = 0; i < SPLIT_RATIO_TAB; i++) {
    split_ratio_entry *t = &rs12_split_ratio_tab[i];
    if (t->key == key) {
      if (t->used < 0xffff)
        t->used++;
      return t;
    }
    if (t->used < e->used)
      e = t;
  }
  for (i = 0; i < SPLIT_RATIO_TAB; i++) // age the others so a stale area can be replaced
    rs12_split_ratio_tab[i].used >>= 1;
  e->key = key;
  e->ratio = rs12_gpu_split_ratio;
  e->used = 1;
  e->sum_a = e->sum_b = e->n = 0;
  return e;
}

// lists shorter than this are not worth waking the helper for
#define SPLIT_MIN_THREAD_WORDS 24

static int split_do_cmd_list(uint32_t *list, int count, uint32_t *ex_regs,
 int *cycles_sum, int *cycles_last, int *last_cmd)
{
  int pos = 0;
  u32 cmd;

  do {
    int ret, threaded = split_threaded && !split_no_thread &&
      count - pos >= SPLIT_MIN_THREAD_WORDS;
    egpu.split_ratio = egpu_b->split_ratio = rs12_gpu_split_ratio;
    split_job.list = list + pos;
    split_job.count = count - pos;
    memcpy(split_job.ex_regs, ex_regs, sizeof(split_job.ex_regs));
#if SPLIT_THREADED
    uint64_t t0, ta = 0, tb = 0;
    if (threaded)
      rs12_split_worker_kick();
    t0 = rs12_split_now_us();
#endif
    ret = gpu_parse(&egpu, list + pos, (count - pos) * 4, ex_regs,
      cycles_sum, cycles_last, &cmd);
    flush_render_block_buffer(&egpu);
#if SPLIT_THREADED
    ta = rs12_split_now_us() - t0;
    rs12_gpu_split_a_us += ta;
    if (!threaded) {
      // sequential (short list, or no helper): time the second half too
      split_job_run();
      rs12_gpu_split_busy_us += rs12_split_now_us() - t0 - ta;
    }
    else {
      rs12_gpu_act = 0x1fd; // joining the helper, for the sampling profiler
      const int helper_ran = rs12_split_worker_finish(&tb);
      const uint64_t tj = rs12_split_now_us() - t0 - ta; // this thread waited for the helper
      rs12_gpu_split_join_us += tj;
      if (!helper_ran)
        rs12_gpu_split_stolen++;
      else if (ta + tb > 400 && egpu.split_key && !egpu.split_solo) {
        // balance the bands of this draw area (the result does not depend on the split, only the speed). By the
        // time this thread waits, not the helper's job time: the helper may start late, and that delay is what
        // the renderer waits for
        split_ratio_entry *e = split_ratio_entry_for(egpu.split_key);
        int r = e->ratio;
        // chunks of one area differ a lot (A slower in some, B much slower in others), so act on 8 of them:
        // from each side's cost per share of rows, the share with equal ends, approached a quarter of the way.
        // The helper's end counts from this thread's start when this thread waited for it, else its job time.
        e->sum_a += (u32)ta;
        e->sum_b += (u32)(tj > 30 ? ta + tj : tb);
        if (++e->n >= 8) {
          const int64_t ca = (int64_t)e->sum_a * (256 - r), cb = (int64_t)e->sum_b * r;
          const int t = ca + cb > 0 ? (int)(256 * cb / (ca + cb)) : r;
          r += (t - r) / 4;
          r = r < 32 ? 32 : r > 224 ? 224 : r;
          e->sum_a = e->sum_b = e->n = 0;
        }
        e->ratio = r;
        if (ta + tb > 4000)
          rs12_gpu_split_ratio = r; // the main scene's, for the log and new draw areas
      }
    }
#else
    split_job_run();
#endif

    if (split_job.ret != ret || split_job.cmd != cmd) {
      // must not happen: both parse the same list with the same state
      rs12_gpu_split_mismatch++;
      assert(0);
    }
    if (egpu.split_refresh) {
      // a texture page cached during the chunk next to its writes may have missed the other instance's
      // writes: both convert it again on next use (split_check)
      for (u32 i = 0; i < egpu.split_nw; i++) {
        const s16 *r = egpu.split_w[i];
        invalidate_texture_cache_region(&egpu, r[0], r[1], r[2], r[3]);
        invalidate_texture_cache_region(egpu_b, r[0], r[1], r[2], r[3]);
      }
    }
    rs12_gpu_split_chunks++;
    if (egpu.split_solo)
      rs12_gpu_split_solos++;
    pos += ret;
    if (cmd == SPLIT_BARRIER_CMD) {
      rs12_gpu_split_barriers++;
      if (ret == 0) { // cannot happen (a chunk always takes its first command)
        assert(0);
        cmd = (u32)-1;
        break;
      }
    }
  } while (cmd == SPLIT_BARRIER_CMD && pos < count);

  *last_cmd = (int)cmd;
  return pos;
}

int renderer_do_cmd_list(uint32_t *list, int count, uint32_t *ex_regs,
 int *cycles_sum, int *cycles_last, int *last_cmd)
{
  int ret;

  if (unlikely(split_request >= 0))
    split_update();

  if (split_active && !gpu.state.enhancement_active)
    ret = split_do_cmd_list(list, count, ex_regs, cycles_sum, cycles_last, last_cmd);
  else if (gpu.state.enhancement_active)
    ret = gpu_parse_enhanced(&egpu, list, count * 4, ex_regs,
            cycles_sum, cycles_last, (u32 *)last_cmd);
  else
    ret = gpu_parse(&egpu, list, count * 4, ex_regs,
            cycles_sum, cycles_last, (u32 *)last_cmd);

  // ReSys12: keep tpage bit 11 (texture page Y + 512 on the System 12 GPU)
  ex_regs[1] &= ~0x9ff;
  ex_regs[1] |= egpu.texture_settings & 0x9ff;
  return ret;
}

#define ENHANCEMENT_BUF_SIZE (1024 * 1024 * 2 * 4 + 4096 * 2)

static void *get_enhancement_bufer(int *x, int *y, int *w, int *h,
 int *vram_h)
{
  uint16_t *ret = select_enhancement_buf_ptr(&egpu, *x, *y);
  if (ret == NULL)
    return NULL;

  *x *= 2;
  *y *= 2;
  *w = *w * 2;
  *h = *h * 2;
  *vram_h = 1024;
  return ret;
}

static void map_enhancement_buffer(void)
{
  // currently we use 4x 1024*1024 buffers instead of single 2048*1024
  // to be able to reuse 1024-width code better (triangle setup,
  // dithering phase, lines).
  egpu.enhancement_buf_ptr = gpu.mmap(ENHANCEMENT_BUF_SIZE);
  if (egpu.enhancement_buf_ptr == NULL || egpu.enhancement_buf_ptr == (void *)(intptr_t)-1) {
    SysPrintf("failed to map enhancement buffer\n");
    egpu.enhancement_buf_ptr = NULL;
    gpu.get_enhancement_bufer = NULL;
  }
  else {
    egpu.enhancement_buf_ptr += 4096 / 2;
    gpu.get_enhancement_bufer = get_enhancement_bufer;
  }
}

int renderer_init(void)
{
  if (gpu.vram != NULL) {
    initialize_psx_gpu(&egpu, gpu.vram);
    initialized = 1;
  }

  // ReSys12: no 2x enhancement. Its buffers and scanout logic assume a 512-line
  // VRAM; the System 12 GPU has 1024 lines. Not mapping it also saves ~8MB.
#if 0
  if (gpu.mmap != NULL && egpu.enhancement_buf_ptr == NULL)
    map_enhancement_buffer();
#endif

  return 0;
}

void renderer_finish(void)
{
  if (egpu.enhancement_buf_ptr != NULL) {
    egpu.enhancement_buf_ptr -= 4096 / 2;
    gpu.munmap(egpu.enhancement_buf_ptr, ENHANCEMENT_BUF_SIZE);
  }
  egpu.enhancement_buf_ptr = NULL;
  egpu.enhancement_current_buf_ptr = NULL;
  initialized = 0;
  // ReSys12: egpu gets re-initialized (split off); split again when used next
  if (split_active) {
    split_active = 0;
    if (split_request < 0)
      split_request = 1;
  }
}

static noinline void sync_enhancement_buffers(int x, int y, int w, int h)
{
  int i, right = x + w, bottom = y + h;
  const u16 *src = gpu.vram;
  // use these because the scanout struct may hold reduced w, h
  // due to intersection stuff, see the update_enhancement_buf_scanouts() mess
  int s_w = max(gpu.screen.hres, gpu.screen.w);
  int s_h = gpu.screen.vres;
  if (gpu.screen.y < 0)
    s_h -= gpu.screen.y;
  s_w = min(s_w, 512);
  for (i = 0; i < ARRAY_SIZE(egpu.enhancement_scanouts); i++) {
    const struct psx_gpu_scanout *s = &egpu.enhancement_scanouts[i];
    u16 *dst = select_enhancement_buf_by_index(&egpu, i);
    int x1, x2, y1, y2;
    if (s->w == 0) continue;
    if (s->x >= right) continue;
    if (s->x + s_w <= x) continue;
    if (s->y >= bottom) continue;
    if (s->y + s_h <= y) continue;
    x1 = max(x, s->x);
    x2 = min(right, s->x + s_w);
    y1 = max(y, s->y);
    y2 = min(bottom, s->y + s_h);
    // 16-byte align for the asm version
    x2 += x1 & 7;
    x1 &= ~7;
    scale2x_tiles8(dst + y1 * 1024*2 + x1 * 2,
        src + y1 * 1024 + x1, (x2 - x1 + 7) / 8u, y2 - y1);
  }
}

void renderer_sync_ecmds(uint32_t *ecmds)
{
  s32 dummy0 = 0;
  u32 dummy1 = 0;
  gpu_parse(&egpu, ecmds + 1, 6 * 4, ecmds, &dummy0, &dummy0, &dummy1);
  if (split_active) { // ReSys12: keep the second instance's state the same
    uint32_t ecmds_b[8];
    memcpy(ecmds_b, ecmds, sizeof(ecmds_b));
    gpu_parse(egpu_b, ecmds_b + 1, 6 * 4, ecmds_b, &dummy0, &dummy0, &dummy1);
  }
}

void renderer_update_caches(int x, int y, int w, int h, int state_changed)
{
  update_texture_cache_region(&egpu, x, y, x + w - 1, y + h - 1);
  if (split_active) // ReSys12: VRAM written outside the renderer - both caches
    update_texture_cache_region(egpu_b, x, y, x + w - 1, y + h - 1);

  if (gpu.state.enhancement_active) {
    if (state_changed) {
      int vres = gpu.screen.vres;
      if (gpu.screen.y < 0)
        vres -= gpu.screen.y;
      memset(egpu.enhancement_scanouts, 0, sizeof(egpu.enhancement_scanouts));
      egpu.enhancement_scanout_eselect = 0;
      update_enhancement_buf_scanouts(&egpu,
        gpu.screen.src_x, gpu.screen.src_y, gpu.screen.hres, vres);
      return;
    }
    sync_enhancement_buffers(x, y, w, h);
  }
}

void renderer_flush_queues(void)
{
  flush_render_block_buffer(&egpu);
  if (split_active) // ReSys12 (normally empty: split chunks end flushed)
    flush_render_block_buffer(egpu_b);
}

void renderer_set_interlace(int enable, int is_odd)
{
  egpu.render_mode &= ~(RENDER_INTERLACE_ENABLED|RENDER_INTERLACE_ODD);
  if (enable)
    egpu.render_mode |= RENDER_INTERLACE_ENABLED;
  if (is_odd)
    egpu.render_mode |= RENDER_INTERLACE_ODD;
  if (egpu_b) // ReSys12: split rendering instance (also while inactive)
    egpu_b->render_mode = egpu.render_mode;
}

void renderer_notify_screen_change(const struct psx_gpu_screen *screen)
{
  int x = screen->src_x, y = screen->src_y;
  int vres = screen->vres;
  if (!gpu.state.enhancement_active || !egpu.enhancement_buf_ptr)
    return;

  if (screen->y < 0)
    vres -= screen->y;
  update_enhancement_buf_scanouts(&egpu, x, y, screen->hres, vres);
}

#include "../../frontend/plugin_lib.h"

void renderer_set_config(const struct rearmed_cbs *cbs)
{
  if (!initialized) {
    initialize_psx_gpu(&egpu, gpu.vram);
    initialized = 1;
  }
  // ReSys12: 2x enhancement not supported with the 1024-line VRAM (see renderer_init)
  if (cbs->pl_set_gpu_caps)
    cbs->pl_set_gpu_caps(0);

  egpu.allow_dithering = cbs->dithering;
  egpu.force_dithering = cbs->dithering >> 1;
  /*
  if (!egpu.allow_dithering) {
    egpu.dither_table[0] = dither_table_row(0, 0, 0, 0);
    egpu.dither_table[1] = dither_table_row(0, 0, 0, 0);
    egpu.dither_table[2] = dither_table_row(0, 0, 0, 0);
    egpu.dither_table[3] = dither_table_row(0, 0, 0, 0);
  } else {
    egpu.dither_table[0] = dither_table_row(-4, 0, -3, 1);
    egpu.dither_table[1] = dither_table_row(2, -2, 3, -1);
    egpu.dither_table[2] = dither_table_row(-3, 1, -4, 0);
    egpu.dither_table[3] = dither_table_row(3, -1, 2, -2); 
  }
  */

  egpu.hack_disable_main = cbs->gpu_neon.enhancement_no_main;
  egpu.hack_texture_adj = cbs->gpu_neon.enhancement_tex_adj;
  if (egpu_b) { // ReSys12: split rendering instance
    egpu_b->allow_dithering = egpu.allow_dithering;
    egpu_b->force_dithering = egpu.force_dithering;
    egpu_b->hack_disable_main = egpu.hack_disable_main;
    egpu_b->hack_texture_adj = egpu.hack_texture_adj;
  }
#if 0 // ReSys12: no 2x enhancement (see renderer_init)
  if (gpu.state.enhancement_enable) {
    if (gpu.mmap != NULL && egpu.enhancement_buf_ptr == NULL)
      map_enhancement_buffer();
  }
#endif
}

// vim:ts=2:sw=2:expandtab

#ifdef RS12_HARNESS
// ReSys12 (diagnostics, PC harness): copy an 8bpp texture cache entry (page p, 256x256 texels in 16x16 tiles)
void rs12_dump_tex8(int page, void *out)
{
  memcpy(out, (page & 1) ? egpu.texture_8bpp_odd_cache[page >> 1] : egpu.texture_8bpp_even_cache[page >> 1],
    256 * 256);
}
#endif

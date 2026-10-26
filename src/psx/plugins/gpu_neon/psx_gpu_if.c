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

// ReSys12: split rendering (two or three CPU cores). Helper renderer
// instances (egpu_x[]) parse the same command lists as egpu; each draws its own
// band of rows (psx_gpu_parse.c split_apply_band / split_check), egpu the top
// one. On the Vita each helper runs on a worker thread (CPU1, CPU3:
// shim/rs12_threads_vita.c); elsewhere they run one after the other on the
// calling thread, which gives the same VRAM contents (used by the PC/ARM
// harness to check correctness).
static psx_gpu_struct *egpu_x[SPLIT_MAX - 1];
static void *egpu_x_alloc[SPLIT_MAX - 1];
static int split_n = 2, split_n_req = 2;    // instances (bands)
static volatile int split_request = -1;  // -1 = no change pending
static int split_active;
volatile u32 rs12_gpu_act_b, rs12_gpu_act_c; // sampling profiler markers of the helpers
static volatile u32 *const split_act[SPLIT_MAX - 1] = { &rs12_gpu_act_b, &rs12_gpu_act_c };
// statistics (read by the frontend perf line)
volatile uint64_t rs12_gpu_split_busy_us;   // first helper thread busy time
volatile uint64_t rs12_gpu_split_busy_c_us; // second helper thread busy time
volatile uint64_t rs12_gpu_split_a_us;      // renderer thread: own band (threaded chunks)
volatile uint64_t rs12_gpu_split_join_us;   // renderer thread: waiting for / running the helpers' jobs
volatile uint32_t rs12_gpu_split_chunks, rs12_gpu_split_barriers;
volatile uint32_t rs12_gpu_split_solos, rs12_gpu_split_stolen;
volatile uint32_t rs12_gpu_split_mismatch;
volatile uint32_t rs12_gpu_split_ratio = 128; // top band share of the main scene, for the log
static u16 split_cut_def[2] = { 128, 256 };   // band boundaries of new draw areas, /256

#ifdef __vita__
// shim/rs12_threads_vita.c
int rs12_split_worker_start(int w, void (*job)(int), int cpu);
void rs12_split_worker_kick(int w);
int rs12_split_worker_finish(int w, uint64_t *job_us);
uint64_t rs12_split_now_us(void);
#define SPLIT_THREADED 1
#else
#define SPLIT_THREADED 0
#endif
static int split_threaded[SPLIT_MAX - 1], split_no_thread;

// the job of helper w: parse one chunk, write everything out
static struct {
  uint32_t *list;
  int count;
  uint32_t ex_regs[8];
  int ret;
  u32 cmd;
} split_job[SPLIT_MAX - 1];

static void split_job_run(int w)
{
  s32 dummy_sum = 0, dummy_last = 0;
  split_job[w].ret = gpu_parse(egpu_x[w], split_job[w].list, split_job[w].count * 4,
    split_job[w].ex_regs, &dummy_sum, &dummy_last, &split_job[w].cmd);
  flush_render_block_buffer(egpu_x[w]);
  *split_act[w] = 0x1ff; // idle, for the sampling profiler
}

// enable: 0 = single instance, 1 = split (helper threads where available),
// 2 = split, all bands on the rendering thread (debug/measurement)
void rs12_gpu_split_enable(int enable)
{
  split_no_thread = enable == 2;
  split_request = enable ? 1 : 0;
}

// bands when split: 2 or 3 (applied at the next command list)
void rs12_gpu_split_set_count(int n)
{
  split_n_req = n < 2 ? 2 : n > SPLIT_MAX ? SPLIT_MAX : n;
  if (split_active && split_request < 0)
    split_request = 1;
}

void rs12_gpu_split_set_ratio(int ratio)
{
  if (ratio < 1) ratio = 1;
  if (ratio > 255) ratio = 255;
  rs12_gpu_split_ratio = ratio;
}

// 0 = off, else the number of bands
int rs12_gpu_split_active(void)
{
  return split_active ? split_n : 0;
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
  int w;
  if (req < 0 || !initialized)
    return;
  split_request = -1;
  if (req == split_active && (!req || split_n == split_n_req))
    return;
  flush_render_block_buffer(&egpu);
  for (w = 0; split_active && w < split_n - 1; w++)
    flush_render_block_buffer(egpu_x[w]);
  if (req) {
    // the helper instances have no texture caches of their own (they use egpu's,
    // see cache_owner in psx_gpu.h), so only the part before them is allocated
    const size_t size_b = offsetof(psx_gpu_struct, texture_4bpp_cache);
    const int n = split_n_req;
    for (w = 0; w < n - 1; w++) {
      if (!egpu_x[w]) {
        egpu_x_alloc[w] = malloc(size_b + 256);
        if (!egpu_x_alloc[w]) {
          SysPrintf("gpu split: no memory for helper renderer %d\n", w);
          return;
        }
        egpu_x[w] = (psx_gpu_struct *)(((uintptr_t)egpu_x_alloc[w] + 255) & ~(uintptr_t)255);
      }
    }
    split_n = n;
    split_cut_def[0] = n > 2 ? 86 : rs12_gpu_split_ratio;
    split_cut_def[1] = n > 2 ? 171 : 256;
    memset(rs12_split_ratio_tab, 0, sizeof(rs12_split_ratio_tab)); // (cuts for another band count)
    egpu.split_mode = 1;
    egpu.split_n = n;
    for (w = 0; w < n - 1; w++) {
      // same state as egpu
      memcpy(egpu_x[w], &egpu, size_b);
      egpu_x[w]->act_ptr = split_act[w];
      egpu_x[w]->cache_owner = &egpu;
      egpu_x[w]->split_mode = w + 2;
#if SPLIT_THREADED
      // helpers: CPU1 (below the sound thread) and CPU3 (the recompiler's core, mostly idle during play)
      if (!split_threaded[w])
        split_threaded[w] = rs12_split_worker_start(w, split_job_run, w == 0 ? 1 : 3);
#endif
    }
    split_active = 1;
  }
  else {
    egpu.split_mode = SPLIT_OFF;
    egpu.viewport_start_y = egpu.logical_vy0;
    egpu.viewport_end_y = egpu.logical_vy1;
    split_active = 0;
  }
}

// ReSys12: the band entry of a draw area (rendering thread, between chunks); a new area replaces the least used
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
  e->cut[0] = split_cut_def[0];
  e->cut[1] = split_cut_def[1];
  e->used = 1;
  e->sum[0] = e->sum[1] = e->sum[2] = e->n = 0;
  return e;
}

// balance the bands of a draw area from the ends of its instances over 8 chunks (the result does not depend on the
// bands, only the speed): from each band's cost per share of rows, the shares with equal ends, approached a quarter
// of the way. With 2 bands this is the earlier ratio formula
static void split_balance(split_ratio_entry *e, const uint64_t *end, int n)
{
  int sh[SPLIT_MAX], i;
  for (i = 0; i < n; i++)
    e->sum[i] += (u32)end[i] + 1;
  if (++e->n < 8)
    return;
  sh[0] = e->cut[0];
  if (n > 2) {
    sh[1] = e->cut[1] - e->cut[0];
    sh[2] = 256 - e->cut[1];
  }
  else
    sh[1] = 256 - e->cut[0];
  {
    // target share i ~ sh[i] / sum[i]; as integers sh[i] times the product of the other sums
    int64_t wt[SPLIT_MAX], tot = 0;
    for (i = 0; i < n; i++) {
      int j;
      wt[i] = sh[i];
      for (j = 0; j < n; j++)
        if (j != i)
          wt[i] *= e->sum[j];
      tot += wt[i];
    }
    if (tot > 0) {
      const int min_sh = n > 2 ? 24 : 32;
      int acc = 0;
      for (i = 0; i < n - 1; i++) {
        const int t = (int)(256 * wt[i] / tot);
        sh[i] += (t - sh[i]) / 4;
        if (sh[i] < min_sh)
          sh[i] = min_sh;
        acc += sh[i];
      }
      if (256 - acc < min_sh) // the last band keeps at least min_sh
        sh[0] -= min_sh - (256 - acc);
      e->cut[0] = sh[0];
      e->cut[1] = n > 2 ? sh[0] + sh[1] : 256;
    }
  }
  e->sum[0] = e->sum[1] = e->sum[2] = e->n = 0;
}

// lists shorter than this are not worth waking the helpers for
#define SPLIT_MIN_THREAD_WORDS 24

static int split_do_cmd_list(uint32_t *list, int count, uint32_t *ex_regs,
 int *cycles_sum, int *cycles_last, int *last_cmd)
{
  int pos = 0, w;
  const int nw = split_n - 1;
  u32 cmd;

  do {
    int ret, threaded = split_threaded[0] && (nw < 2 || split_threaded[1]) && !split_no_thread &&
      count - pos >= SPLIT_MIN_THREAD_WORDS;
    egpu.split_cut[0] = split_cut_def[0];
    egpu.split_cut[1] = split_cut_def[1];
    for (w = 0; w < nw; w++) {
      egpu_x[w]->split_cut[0] = split_cut_def[0];
      egpu_x[w]->split_cut[1] = split_cut_def[1];
      split_job[w].list = list + pos;
      split_job[w].count = count - pos;
      memcpy(split_job[w].ex_regs, ex_regs, sizeof(split_job[w].ex_regs));
    }
    split_shared_begin(); // (the chunk's decisions, shared by the instances)
#if SPLIT_THREADED
    uint64_t t0, ta = 0;
    if (threaded)
      for (w = 0; w < nw; w++)
        rs12_split_worker_kick(w);
    t0 = rs12_split_now_us();
#endif
    ret = gpu_parse(&egpu, list + pos, (count - pos) * 4, ex_regs,
      cycles_sum, cycles_last, &cmd);
    flush_render_block_buffer(&egpu);
#if SPLIT_THREADED
    ta = rs12_split_now_us() - t0;
    rs12_gpu_split_a_us += ta;
    if (!threaded) {
      // sequential (short list, or no helper): time the other bands too
      for (w = 0; w < nw; w++)
        split_job_run(w);
      rs12_gpu_split_busy_us += rs12_split_now_us() - t0 - ta;
    }
    else {
      uint64_t end[SPLIT_MAX], tb;
      int all_ran = 1;
      end[0] = ta;
      rs12_gpu_act = 0x1fd; // joining the helpers, for the sampling profiler
      for (w = 0; w < nw; w++) {
        const uint64_t tw = rs12_split_now_us();
        const int helper_ran = rs12_split_worker_finish(w, &tb);
        const uint64_t tn = rs12_split_now_us();
        // a helper's end: from this thread's start when this thread waited for it (the helper may start late, and
        // that delay is what the renderer waits for), else its job time
        end[w + 1] = tn - tw > 30 ? tn - t0 : tb;
        if (!helper_ran) {
          rs12_gpu_split_stolen++;
          all_ran = 0;
        }
      }
      rs12_gpu_split_join_us += rs12_split_now_us() - t0 - ta;
      if (all_ran && egpu.split_key && !egpu.split_solo) {
        uint64_t sum = 0;
        for (w = 0; w <= nw; w++)
          sum += end[w];
        if (sum > 400) {
          split_ratio_entry *e = split_ratio_entry_for(egpu.split_key);
          split_balance(e, end, nw + 1);
          if (sum > 4000) { // the main scene's, for the log and new draw areas
            rs12_gpu_split_ratio = e->cut[0];
            split_cut_def[0] = e->cut[0];
            split_cut_def[1] = e->cut[1];
          }
        }
      }
    }
#else
    for (w = 0; w < nw; w++)
      split_job_run(w);
#endif

    split_shared_end(&egpu); // (the chunk's write rects for split_refresh below)
    for (w = 0; w < nw; w++)
      if (split_job[w].ret != ret || split_job[w].cmd != cmd) {
        // must not happen: all parse the same list with the same state
        rs12_gpu_split_mismatch++;
        assert(0);
      }
    if (egpu.split_refresh) {
      // a texture page cached during the chunk next to its writes may have missed another instance's
      // writes: all convert it again on next use (split_check)
      for (u32 i = 0; i < egpu.split_nw; i++) {
        const s16 *r = egpu.split_w[i];
        invalidate_texture_cache_region(&egpu, r[0], r[1], r[2], r[3]);
        for (w = 0; w < nw; w++)
          invalidate_texture_cache_region(egpu_x[w], r[0], r[1], r[2], r[3]);
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
  for (int w = 0; split_active && w < split_n - 1; w++) { // ReSys12: keep the helper instances' state the same
    uint32_t ecmds_b[8];
    memcpy(ecmds_b, ecmds, sizeof(ecmds_b));
    gpu_parse(egpu_x[w], ecmds_b + 1, 6 * 4, ecmds_b, &dummy0, &dummy0, &dummy1);
  }
}

void renderer_update_caches(int x, int y, int w, int h, int state_changed)
{
  update_texture_cache_region(&egpu, x, y, x + w - 1, y + h - 1);
  for (int k = 0; split_active && k < split_n - 1; k++) // ReSys12: VRAM written outside the renderer - all instances
    update_texture_cache_region(egpu_x[k], x, y, x + w - 1, y + h - 1);

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
  for (int w = 0; split_active && w < split_n - 1; w++) // ReSys12 (normally empty: split chunks end flushed)
    flush_render_block_buffer(egpu_x[w]);
}

void renderer_set_interlace(int enable, int is_odd)
{
  egpu.render_mode &= ~(RENDER_INTERLACE_ENABLED|RENDER_INTERLACE_ODD);
  if (enable)
    egpu.render_mode |= RENDER_INTERLACE_ENABLED;
  if (is_odd)
    egpu.render_mode |= RENDER_INTERLACE_ODD;
  for (int w = 0; w < SPLIT_MAX - 1; w++) // ReSys12: split rendering instances (also while inactive)
    if (egpu_x[w])
      egpu_x[w]->render_mode = egpu.render_mode;
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
  for (int w = 0; w < SPLIT_MAX - 1; w++) // ReSys12: split rendering instances
    if (egpu_x[w]) {
      egpu_x[w]->allow_dithering = egpu.allow_dithering;
      egpu_x[w]->force_dithering = egpu.force_dithering;
      egpu_x[w]->hack_disable_main = egpu.hack_disable_main;
      egpu_x[w]->hack_texture_adj = egpu.hack_texture_adj;
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

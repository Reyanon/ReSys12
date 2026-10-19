/*
 * (C) Gražvydas "notaz" Ignotas, 2025
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
#include <assert.h>
#include "gpu.h"
#include "gpu_async.h"
#include "gpu_timing.h"
#include "../../include/arm_features.h"
#include "../../include/compiler_features.h"
#include "../../frontend/pcsxr-threads.h"
#include <time.h>

// ReSys12: profiling counters (microseconds, read by the frontend perf line):
// time the GPU thread spends rendering, and time the emulation thread waits for it
volatile uint64_t rs12_gpu_busy_us, rs12_gpu_wait_us;
extern volatile uint32_t rs12_gpu_act; // gpu_neon psx_gpu_parse.c
static inline uint64_t agpu_now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
// ReSys12: wait time by reason: 0 queue full, 1 full sync (VRAM read etc.), 2 scanout (partial), 3 scanout (full)
volatile uint64_t rs12_gpu_wait_r[4];
static int rs12_wreason = 1;
static void rs12_agpu_wait(struct psx_gpu_async *agpu);
#define agpu_wait_add(agpu) do { \
  const uint64_t t0_ = agpu_now_us(); \
  rs12_agpu_wait(agpu); \
  const uint64_t d_ = agpu_now_us() - t0_; \
  rs12_gpu_wait_us += d_; \
  rs12_gpu_wait_r[rs12_wreason] += d_; \
} while (0)

//#define agpu_log gpu_log
#define agpu_log(...)

// these constants must be power of 2
#define AGPU_BUF_LEN    (512*1024/4u) // ReSys12: 512KB (128KB filled up in soulclbr fights)
#define AGPU_BUF_MASK   (AGPU_BUF_LEN - 1)
#define AGPU_AREAS_CNT  256u // ReSys12: 8 overflowed while the renderer was behind (ehrgeiz fights: full syncs on VRAM reads, 64 at a fight start)
#define AGPU_AREAS_MASK (AGPU_AREAS_CNT - 1)

#ifndef min
#define min(a, b) ((b) < (a) ? (b) : (a))
#endif

// must be in 0xc0...0xdf range that can't appear in thread's real cmd stream;
// must be at least 3 words due to cmd_lengths[]
#define FAKECMD_SCREEN_CHANGE 0xdfu
#define FAKECMD_SET_INTERLACE 0xdeu
#define FAKECMD_DMA_WRITE     0xddu
#define FAKECMD_BREAK         0xdcu
#define FAKECMD_FLIP          0xdbu // ReSys12: display flip done by the GPU thread when it gets there

#if defined(__aarch64__) || defined(HAVE_ARMV7)
#define BARRIER() __asm__ __volatile__ ("dmb ishst" ::: "memory")
#elif defined(HAVE_ARMV6)
#define BARRIER() __asm__ __volatile__ ("mcr p15, 0, %0, c7, c10, 5" :: "r"(0) : "memory")
#else
#define BARRIER() __asm__ __volatile__ ("" ::: "memory")
#endif
#define RDPOS(pos_) *(volatile uint32_t *)&(pos_)
#define WRPOS(pos_, d_) *(volatile uint32_t *)&(pos_) = (d_)

enum waitmode {
  waitmode_none = 0,
  waitmode_progress,
  waitmode_target,
  waitmode_full,
};

struct pos_drawarea {
  uint32_t pos;
  uint16_t x0, y0;
  uint16_t x1, y1;
  uint32_t dma;    // ReSys12: 1 + queue position of the CPU->VRAM write this area is, 0 = drawing commands
  uint32_t dma_r6; // mask bit setting of that write
};

struct psx_gpu_async
{
  uint32_t pos_added;
  uint32_t pos_used;
  uint32_t pos_target;
  enum waitmode wait_mode;
  uint32_t idle;
  uint32_t exit;
  sthread_t *thread;
  slock_t *lock;
  scond_t *cond_use;
  scond_t *cond_add;
  uint32_t ex_regs[8]; // state at render time, native endian
  uint32_t cmd_buffer[AGPU_BUF_LEN];
  uint32_t pos_area;
  struct pos_drawarea draw_areas[AGPU_AREAS_CNT];
  struct {
    int src_x, src_y;
  } delayed_flip;
};

// cmd_* must be at least 3 words long
union cmd_flip
{
  uint32_t u32s[5];
  struct {
    uint32_t cmd;
    struct vout_flip_req r;
  };
};

union cmd_screen_change
{
  uint32_t u32s[4];
  struct {
    uint32_t cmd;
    short x, y;
    short src_x, src_y;
    short hres, vres;
  };
};

union cmd_set_interlace
{
  uint32_t u32s[3];
  struct {
    uint32_t cmd;
    int enable, is_odd;
  };
};

union cmd_dma_write
{
  uint32_t u32s[3];
  struct {
    uint32_t cmd;
    short x, y, w, h;
  };
};

struct cmd_break
{
  uint32_t u32s[3];
};

static int noinline do_notify_screen_change(struct psx_gpu *gpu,
    const union cmd_screen_change *cmd);
static int do_set_interlace(struct psx_gpu *gpu,
    const union cmd_set_interlace *cmd);
static int do_dma_write(struct psx_gpu *gpu,
    const union cmd_dma_write *cmd, uint32_t pos);

static void run_thread_nolock(struct psx_gpu_async *agpu)
{
  if (agpu->idle) {
    agpu->idle = 0;
    scond_signal(agpu->cond_use);
    //agpu_log(&gpu, "%u/%u kick\n", RDPOS(agpu->pos_used), agpu->pos_added);
  }
}

static void run_thread(struct psx_gpu_async *agpu)
{
  slock_lock(agpu->lock);
  run_thread_nolock(agpu);
  slock_unlock(agpu->lock);
}

static int calc_space_for_add(struct psx_gpu_async *agpu, uint32_t pos_added)
{
  int space = AGPU_BUF_LEN - (pos_added - RDPOS(agpu->pos_used));
  assert(space >= 0);
  assert(space <= AGPU_BUF_LEN);
  return space;
}

// adds everything or nothing, else we may get incomplete cmd
static int do_add_pos(struct psx_gpu_async *agpu, const void *list, int list_words,
    uint32_t *pos_added_)
{
  int pos, space, left, retval = 0;
  uint32_t pos_added = *pos_added_;

  assert(list_words < AGPU_BUF_LEN);
  space = calc_space_for_add(agpu, pos_added);
  if (space < list_words)
    return 0;

  pos = pos_added & AGPU_BUF_MASK;
  left = AGPU_BUF_LEN - pos;
  if (left < list_words) {
    memset(&agpu->cmd_buffer[pos], 0, left * 4);
    pos_added += left;
    pos = 0;
    space = calc_space_for_add(agpu, pos_added);
  }
  if (space >= list_words) {
    memcpy(&agpu->cmd_buffer[pos], list, list_words * 4);
    pos_added += list_words;
    retval = list_words;
  }
  *pos_added_ = pos_added;
  return retval;
}

static int do_add(struct psx_gpu_async *agpu, const void *list, int list_words)
{
  uint32_t pos_added = agpu->pos_added;
  int ret = do_add_pos(agpu, list, list_words, &pos_added);
  BARRIER();
  WRPOS(agpu->pos_added, pos_added);
  return ret;
}

// ReSys12: wait (lock held, wait_mode set by the caller) until the GPU thread clears wait_mode. On the Vita the
// waiting thread (the emulation thread, CPU0) meanwhile renders the split helper's band if it is pending: CPU1
// (sound, compiler, helper) runs out of time in heavy scenes (ehrgeiz), while CPU0 sat here for 8-10ms a frame
#ifdef __vita__
extern int rs12_split_worker_help(void);
extern void sceKernelDelayThread(unsigned int usec);
#endif
static void rs12_agpu_wait(struct psx_gpu_async *agpu)
{
#ifdef __vita__
  while (agpu->wait_mode != waitmode_none) {
    slock_unlock(agpu->lock);
    if (!rs12_split_worker_help())
      sceKernelDelayThread(30);
    slock_lock(agpu->lock);
  }
#else
  scond_wait(agpu->cond_add, agpu->lock);
#endif
}

static void do_add_with_wait(struct psx_gpu_async *agpu,
    const void *list, int list_words)
{
  for (;;)
  {
    if (do_add(agpu, list, list_words))
      break;
    slock_lock(agpu->lock);
    run_thread_nolock(agpu);
    while (list_words > AGPU_BUF_LEN - (agpu->pos_added - RDPOS(agpu->pos_used))) {
      assert(!agpu->idle);
      assert(agpu->wait_mode == waitmode_none);
      agpu->wait_mode = waitmode_progress;
      rs12_wreason = 0;
      agpu_wait_add(agpu);
      rs12_wreason = 1;
    }
    slock_unlock(agpu->lock);
  }
}

static void add_draw_area(struct psx_gpu_async *agpu, uint32_t pos, int force,
    uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
  uint32_t pos_area = agpu->pos_area;
  if (pos - agpu->draw_areas[pos_area].pos > 1u || force)
    pos_area = agpu->pos_area = (pos_area + 1) & AGPU_AREAS_MASK;
  agpu->draw_areas[pos_area].pos = pos;
  agpu->draw_areas[pos_area].x0 = x0;
  agpu->draw_areas[pos_area].y0 = y0;
  agpu->draw_areas[pos_area].x1 = x1;
  agpu->draw_areas[pos_area].y1 = y1;
  agpu->draw_areas[pos_area].dma = 0;
}

static void add_draw_area_e(struct psx_gpu_async *agpu, uint32_t pos, int force,
    const uint32_t *ex_regs)
{
  add_draw_area(agpu, pos, force,
       ex_regs[3] & 0x3ff,       (ex_regs[3] >> 10) & 0x3ff, // ReSys12: 10-bit Y
      (ex_regs[4] & 0x3ff) + 1, ((ex_regs[4] >> 10) & 0x3ff) + 1);
}

int gpu_async_do_cmd_list(struct psx_gpu *gpu, const uint32_t *list_data, int list_len,
 int *cpu_cycles_sum_out, int *cpu_cycles_last, int *last_cmd, int *vram_dirty)
{
  int cyc_sum = 0, cyc = *cpu_cycles_last;
  struct psx_gpu_async *agpu = gpu->async;
  int pos_handled = 0, dst_can_add = 1;
  int cmd = -1, pos = 0, len;
  int rendered_anything = 0;
  int insert_break = 0;
  uint32_t pos_added;

  assert(agpu);
  pos_added = agpu->pos_added;
  if (RDPOS(agpu->idle) && pos_added == RDPOS(agpu->pos_used)) {
    // do the cheap stuff directly to avoid thread wakeup/sync
    for (; pos < list_len; pos++) {
      uint32_t ecmd = LE32TOH(list_data[pos]);
      cmd = ecmd >> 24;
      if (cmd < 2)
        continue;
      if ((cmd & 0xf8) != 0xe0)
        break;
      if (gpu->ex_regs[cmd & 7] == ecmd)
        continue;
      gpu->ex_regs[cmd & 7] = ecmd;
      if (cmd == 0xe3 || cmd == 0xe4)
        add_draw_area_e(agpu, pos_added, 0, gpu->ex_regs);
    }
    pos_handled = pos;
    if (pos)
      renderer_do_cmd_list((uint32_t *)list_data, pos, agpu->ex_regs, &cyc_sum, &cyc, &cmd);
  }

  for (; pos < list_len; pos += len)
  {
    const uint32_t *list = list_data + pos;
    const int16_t *slist = (void *)list;
    const struct pos_drawarea *darea;
    int rendered = 1, skip = 0, cyc_tmp;
    int num_vertexes, x, y, w, h;

    cmd = LE32TOH(list[0]) >> 24;
    len = 1 + cmd_lengths[cmd];
    if (pos + len > list_len) {
      cmd = -1;
      break; // incomplete cmd
    }

    switch (cmd) {
      case 0x02:
        x =  (LE16TOH(slist[2]) & 0x3ff) & ~0xf;
        y =   LE16TOH(slist[3]) & 0x3ff; // ReSys12: 10-bit Y/height
        w = ((LE16TOH(slist[4]) & 0x3ff) + 0xf) & ~0xf;
        h =   LE16TOH(slist[5]) & 0x3ff;
        darea = &agpu->draw_areas[agpu->pos_area];
        if (x < darea->x0 || x + w > darea->x1 || y < darea->y0 || y + h > darea->y1) {
          // let sync_scanout() know about changes outside of drawing area
          agpu_log(gpu, "agpu: fill %d,%d %dx%d vs area %d,%d %dx%d\n", x, y, w, h,
            darea->x0, darea->y0, darea->x1 - darea->x0, darea->y1 - darea->y0);
          add_draw_area(agpu, pos_added, 1, x, y, x + w, y + h);
          add_draw_area_e(agpu, pos_added + 1, 1, gpu->ex_regs);
        }
        gput_sum(cyc_sum, cyc, gput_fill(w, h));
        break;
      case 0x1f: // irq?
        goto breakloop;
      case 0x20 ... 0x23: gput_sum(cyc_sum, cyc, gput_poly_base());    break;
      case 0x24 ... 0x27: gput_sum(cyc_sum, cyc, gput_poly_base_t());  goto do_texpage;
      case 0x28 ... 0x2b: gput_sum(cyc_sum, cyc, gput_quad_base());    break;
      case 0x2c ... 0x2f: gput_sum(cyc_sum, cyc, gput_quad_base_t());  goto do_texpage;
      case 0x30 ... 0x33: gput_sum(cyc_sum, cyc, gput_poly_base_g());  break;
      case 0x34 ... 0x37: gput_sum(cyc_sum, cyc, gput_poly_base_gt()); goto do_texpage;
      case 0x38 ... 0x3b: gput_sum(cyc_sum, cyc, gput_quad_base_g());  break;
      case 0x3c ... 0x3f: gput_sum(cyc_sum, cyc, gput_quad_base_gt());
      do_texpage:
        // ReSys12: tpage bit 11 = texture page Y + 512 on System 12
        gpu->ex_regs[1] &= ~0x9ff;
        gpu->ex_regs[1] |= (LE32TOH(list[4 + ((cmd >> 4) & 1)]) >> 16) & 0x9ff;
        break;
      case 0x40 ... 0x47:
        gput_sum(cyc_sum, cyc, gput_line(0));
        break;
      case 0x48 ... 0x4F:
        for (num_vertexes = 2; ; num_vertexes++)
        {
          gput_sum(cyc_sum, cyc, gput_line(0));
          if (pos + num_vertexes + 1 >= list_len) {
            cmd = -1;
            goto breakloop;
          }
          if ((list[num_vertexes + 1] & LE32TOH(0xf000f000)) == LE32TOH(0x50005000))
            break;
        }
        len += (num_vertexes - 2);
        break;
      case 0x50 ... 0x57:
        gput_sum(cyc_sum, cyc, gput_line(0));
        break;
      case 0x58 ... 0x5f:
        for (num_vertexes = 2; ; num_vertexes++)
        {
          gput_sum(cyc_sum, cyc, gput_line(0));
          if (pos + num_vertexes*2 >= list_len) {
            cmd = -1;
            goto breakloop;
          }
          if ((list[num_vertexes * 2] & LE32TOH(0xf000f000)) == LE32TOH(0x50005000))
            break;
        }
        len += (num_vertexes - 2) * 2;
        break;
      case 0x60 ... 0x63:
        w = LE16TOH(slist[4]) & 0x3FF;
        h = LE16TOH(slist[5]) & 0x1FF;
        gput_sum(cyc_sum, cyc, gput_sprite(w, h));
        break;
      case 0x64 ... 0x67:
        w = LE16TOH(slist[6]) & 0x3FF;
        h = LE16TOH(slist[7]) & 0x1FF;
        gput_sum(cyc_sum, cyc, gput_sprite(w, h));
        break;
      case 0x68 ... 0x6b: gput_sum(cyc_sum, cyc, gput_sprite(1, 1));   break;
      case 0x70 ... 0x73:
      case 0x74 ... 0x77: gput_sum(cyc_sum, cyc, gput_sprite(8, 8));   break;
      case 0x78 ... 0x7b:
      case 0x7C ... 0x7f: gput_sum(cyc_sum, cyc, gput_sprite(16, 16)); break;
      case 0x80 ... 0x9f: // vid -> vid
        rendered = do_vram_copy_pre(gpu, list, &cyc_tmp);
        gput_sum(cyc_sum, cyc, cyc_tmp);
        if (!rendered) {
          skip = 1;
          break;
        }
        x =   LE16TOH(slist[4]) & 0x3ff;
        y =   LE16TOH(slist[5]) & 0x3ff; // ReSys12: 10-bit Y/height
        w = ((LE16TOH(slist[6]) - 1) & 0x3ff) + 1;
        h = ((LE16TOH(slist[7]) - 1) & 0x3ff) + 1;
        darea = &agpu->draw_areas[agpu->pos_area];
        if ((w > 2 || h > 1) &&
            (x < darea->x0 || x + w > darea->x1 || y < darea->y0 || y + h > darea->y1))
        {
          add_draw_area(agpu, pos_added, 1, x, y, x + w, y + h);
          add_draw_area_e(agpu, pos_added + 1, 1, gpu->ex_regs);
        }
        break;
      case 0xa0 ... 0xbf: // sys -> vid
      case 0xc0 ... 0xdf: // vid -> sys
        goto breakloop;
      case 0xe0 ... 0xe2:
      case 0xe5 ... 0xe7:
        gpu->ex_regs[cmd & 7] = LE32TOH(list[0]);
        rendered = 0;
        break;
      case 0xe3:
      case 0xe4:
        rendered = 0;
        if (gpu->ex_regs[cmd & 7] == LE32TOH(list[0])) {
          skip = 1;
          break;
        }
        gpu->ex_regs[cmd & 7] = LE32TOH(list[0]);
        add_draw_area_e(agpu, pos_added, 0, gpu->ex_regs);
        insert_break = 1;
        break;
      default:
        rendered = 0;
        skip = 1;
        break;
    }
    rendered_anything |= rendered;
    if (dst_can_add) {
      if (!skip) {
        int added = dst_can_add = do_add_pos(agpu, list, len, &pos_added);
        pos_handled += added;
      }
      else
        pos_handled += len;
    }
  }
breakloop:
  *cpu_cycles_sum_out += cyc_sum;
  *cpu_cycles_last = cyc;
  *last_cmd = cmd;
  *vram_dirty |= rendered_anything;

  if (unlikely(rendered_anything && agpu->delayed_flip.src_x != -1))
    gpu_async_try_delayed_flip(gpu, 1);
  BARRIER();
  WRPOS(agpu->pos_added, pos_added);
  if (pos_handled && (rendered_anything || pos_handled < pos))
    run_thread(agpu);
  if (pos_handled < pos) {
    // note: this is poorly implemented (wrong pos_added for draw_areas)
    int left = pos - pos_handled;
    agpu_log(gpu, "agpu: full %d left %d\n", agpu->pos_added - agpu->pos_used, left);
    do_add_with_wait(agpu, list_data + pos_handled, left);
  }
  if (insert_break) {
    struct cmd_break cmdb = {{ HTOLE32(FAKECMD_BREAK << 24), }};
    do_add(agpu, cmdb.u32s, sizeof(cmdb.u32s) / sizeof(cmdb.u32s[0]));
  }
  return pos;
}

int gpu_async_try_dma(struct psx_gpu *gpu, const uint32_t *data, int words)
{
  struct psx_gpu_async *agpu = gpu->async;
  int used, w = gpu->dma.w, h = gpu->dma.h;
  uint32_t pos_added = agpu->pos_added;
  union cmd_dma_write cmd;
  int bad = 0;

  if (!agpu)
    return 0;
  // avoid double copying
  used = agpu->pos_added - RDPOS(agpu->pos_used);
  if (RDPOS(agpu->idle) && used == 0)
    return 0;
  // only proceed if there is space to avoid messy sync
  const uint32_t need = sizeof(cmd) / 4 + ((w + 1) / 2) * (h + 1);
  if (need > AGPU_BUF_LEN / 2) // ReSys12: too big to wait for space, the caller syncs fully
    return 0;
  if (AGPU_BUF_LEN - used < need) {
    // ReSys12: the queue is nearly full (the renderer is behind). Wait until this transfer fits instead of
    // returning 0, which made the caller wait for the whole queue to drain (3-5ms a frame in soulclbr fights)
    agpu_log(gpu, "agpu: dma: used %d\n", used);
    slock_lock(agpu->lock);
    run_thread_nolock(agpu);
    while (AGPU_BUF_LEN - (agpu->pos_added - RDPOS(agpu->pos_used)) < need) {
      assert(!agpu->idle);
      assert(agpu->wait_mode == waitmode_none);
      agpu->wait_mode = waitmode_progress;
      rs12_wreason = 0;
      agpu_wait_add(agpu);
      rs12_wreason = 1;
    }
    slock_unlock(agpu->lock);
    pos_added = agpu->pos_added;
  }

  // ReSys12: record the written area (outside drawing), so VRAM reads and scanout know about it
  add_draw_area(agpu, pos_added, 1, gpu->dma.x, gpu->dma.y, gpu->dma.x + w, gpu->dma.y + h);
  const uint32_t dma_area = agpu->pos_area;
  add_draw_area_e(agpu, pos_added + 1, 1, gpu->ex_regs);
  cmd.cmd = HTOLE32(FAKECMD_DMA_WRITE << 24);
  cmd.x = gpu->dma.x; cmd.y = gpu->dma.y;
  cmd.w = gpu->dma.w; cmd.h = gpu->dma.h;
  bad |= !do_add_pos(agpu, cmd.u32s, sizeof(cmd) / 4, &pos_added);
  agpu->draw_areas[dma_area].dma = pos_added - sizeof(cmd) / 4 + 1; // (after any wrap padding)
  agpu->draw_areas[dma_area].dma_r6 = gpu->ex_regs[6] & 3;
  if (w & 1) {
    // align lines to psx dma word units
    const uint16_t *sdata = (const uint16_t *)data;
    for (; h > 0; sdata += w, h--)
      bad |= !do_add_pos(agpu, sdata, w / 2 + 1, &pos_added);
  }
  else {
    for (; h > 0; data += w / 2, h--)
      bad |= !do_add_pos(agpu, data, w / 2, &pos_added);
  }
  assert(!bad); (void)bad;

  slock_lock(agpu->lock);
  agpu->pos_added = pos_added;
  run_thread_nolock(agpu);
  slock_unlock(agpu->lock);

  return 1;
}

static STRHEAD_RET_TYPE gpu_async_thread(void *unused)
{
  struct psx_gpu *gpup = &gpu;
  struct psx_gpu_async *agpu = gpup->async;
  int renderer_dirty = 0;

  assert(agpu);
  slock_lock(agpu->lock);
  while (!agpu->exit)
  {
    int len = RDPOS(agpu->pos_added) - agpu->pos_used;
    int pos = agpu->pos_used & AGPU_BUF_MASK;
    int done, cycles_dummy = 0, cmd = -1;
    assert(len >= 0);
    if (len == 0 && !renderer_dirty) {
      switch (agpu->wait_mode) {
        case waitmode_full:
        case waitmode_target:
          agpu->wait_mode = waitmode_none;
          scond_signal(agpu->cond_add);
          break;
        case waitmode_none:
          break;
        default:
          assert(0);
      }
      agpu->idle = 1;
      //agpu_log(&gpu, "%u/%u sleep\n", agpu->pos_used, RDPOS(agpu->pos_added));
      rs12_gpu_act = 0x1ff; // ReSys12: idle, for the sampling profiler
      scond_wait(agpu->cond_use, agpu->lock);
      continue;
    }
    slock_unlock(agpu->lock);

    if (len == 0 && renderer_dirty) {
      renderer_flush_queues();
      renderer_dirty = 0;
      slock_lock(agpu->lock);
      continue;
    }

    len = min(len, AGPU_BUF_LEN - pos);
    rs12_gpu_act = 0x1fe; // ReSys12: busy outside a parsed command (copies, DMA writes, flushes)
    const uint64_t busy_t0 = agpu_now_us(); // ReSys12: profiling
    done = renderer_do_cmd_list(agpu->cmd_buffer + pos, len, agpu->ex_regs,
             &cycles_dummy, &cycles_dummy, &cmd);
    renderer_dirty |= done;
    if (done != len) {
      const void *list = agpu->cmd_buffer + pos + done;
      switch (cmd) {
        case 0x80 ... 0x9f:
          done += do_vram_copy(gpup->vram, agpu->ex_regs, list);
          break;
        case FAKECMD_SCREEN_CHANGE:
          done += do_notify_screen_change(gpup, list);
          break;
        case FAKECMD_SET_INTERLACE:
          done += do_set_interlace(gpup, list);
          renderer_dirty = 0;
          break;
        case FAKECMD_DMA_WRITE:
          done += do_dma_write(gpup, list, pos + done);
          renderer_dirty = 0;
          break;
        case FAKECMD_BREAK:
          done += sizeof(struct cmd_break) / 4;
          renderer_flush_queues();
          renderer_dirty = 0;
          break;
        case FAKECMD_FLIP: // ReSys12: everything before it is drawn now; show it
          renderer_flush_queues();
          renderer_dirty = 0;
          vout_flip_do(gpup, &((const union cmd_flip *)list)->r);
          done += sizeof(union cmd_flip) / 4;
          break;
        default:
          assert(0);
          if (!done)
            done = 1;
          break;
      }
    }

    assert(done > 0);
    rs12_gpu_busy_us += agpu_now_us() - busy_t0;
    slock_lock(agpu->lock);
    agpu->pos_used += done;
    switch (agpu->wait_mode) {
      case waitmode_target:
        if ((int32_t)(agpu->pos_used - agpu->pos_target) < 0)
          break;
        // fallthrough
      case waitmode_progress:
        agpu->wait_mode = waitmode_none;
        scond_signal(agpu->cond_add);
        break;
      default:
        break;
    }
  }
  slock_unlock(agpu->lock);
  STRHEAD_RETURN();
}

// ReSys12: queue the display flip instead of waiting for the GPU thread to finish the displayed area. Interlaced
// games draw into the buffer being shown, so every vblank was a full sync (emulation and GPU threads in series;
// ehrgeiz 22ms a frame). Returns 0 if the queue is empty (the caller flips right away) or nothing is to be shown.
int rs12_async_flip = 1;
int gpu_async_queue_flip(struct psx_gpu *gpu, int src_x, int src_y)
{
  struct psx_gpu_async *agpu = gpu->async;
  union cmd_flip cmd;

  if (!agpu || !rs12_async_flip)
    return 0;
  if (RDPOS(agpu->idle) && agpu->pos_added == RDPOS(agpu->pos_used))
    return 0;
  memset(&cmd, 0, sizeof(cmd));
  if (!vout_flip_params(gpu, src_x, src_y, &cmd.r))
    return 0;
  cmd.cmd = HTOLE32(FAKECMD_FLIP << 24);
  do_add_with_wait(agpu, cmd.u32s, sizeof(cmd) / 4);
  slock_lock(agpu->lock);
  run_thread_nolock(agpu);
  slock_unlock(agpu->lock);
  return 1;
}

void gpu_async_notify_screen_change(struct psx_gpu *gpu)
{
  struct psx_gpu_async *agpu = gpu->async;
  union cmd_screen_change cmd;

  if (!agpu)
    return;
  if (RDPOS(agpu->idle) && agpu->pos_added == RDPOS(agpu->pos_used)) {
    renderer_notify_screen_change(&gpu->screen);
    return;
  }
  cmd.cmd = HTOLE32(FAKECMD_SCREEN_CHANGE << 24);
  cmd.x = gpu->screen.x;
  cmd.y = gpu->screen.y;
  cmd.hres = gpu->screen.hres;
  cmd.vres = gpu->screen.vres;
  cmd.src_x = gpu->screen.src_x;
  cmd.src_y = gpu->screen.src_y;
  do_add_with_wait(gpu->async, cmd.u32s, sizeof(cmd) / 4);
}

static int noinline do_notify_screen_change(struct psx_gpu *gpu,
    const union cmd_screen_change *cmd)
{
  struct psx_gpu_screen screen = gpu->screen;
  screen.x = cmd->x;
  screen.y = cmd->y;
  screen.hres = cmd->hres;
  screen.vres = cmd->vres;
  screen.src_x = cmd->src_x;
  screen.src_y = cmd->src_y;
  renderer_notify_screen_change(&screen);
  return sizeof(*cmd) / 4;
}

void gpu_async_set_interlace(struct psx_gpu *gpu, int enable, int is_odd)
{
  struct psx_gpu_async *agpu = gpu->async;
  union cmd_set_interlace cmd;

  if (!agpu)
    return;
  if (RDPOS(agpu->idle) && agpu->pos_added == RDPOS(agpu->pos_used)) {
    renderer_flush_queues();
    renderer_set_interlace(enable, is_odd);
    return;
  }
  cmd.cmd = HTOLE32(FAKECMD_SET_INTERLACE << 24);
  cmd.enable = enable;
  cmd.is_odd = is_odd;
  do_add_with_wait(gpu->async, cmd.u32s, sizeof(cmd) / 4);
}

static int do_set_interlace(struct psx_gpu *gpu,
    const union cmd_set_interlace *cmd)
{
  renderer_flush_queues();
  renderer_set_interlace(cmd->enable, cmd->is_odd);
  return sizeof(*cmd) / 4;
}

static int do_dma_write(struct psx_gpu *gpu,
    const union cmd_dma_write *cmd, uint32_t pos)
{
  int x = cmd->x, y = cmd->y, w = cmd->w, h = cmd->h;
  struct psx_gpu_async *agpu = gpu->async;
  uint32_t r6 = agpu->ex_regs[6] & 3;
  uint16_t *vram = gpu->vram;
  int stride = (w + 1) / 2;
  int done = 0;

  renderer_flush_queues();

  pos += sizeof(*cmd) / 4u;
  done += sizeof(*cmd) / 4u;
  assert(pos <= AGPU_BUF_LEN);
  for (; h > 0; h--, y++) {
    if (stride > AGPU_BUF_LEN - pos) {
      done += AGPU_BUF_LEN - pos;
      pos = 0;
    }

    y &= 1023; // ReSys12: 1024-line VRAM
    do_vram_line(vram, x, y, (uint16_t *)&agpu->cmd_buffer[pos], w, 0, r6);
    pos += stride;
    done += stride;
  }
  renderer_update_caches(x, cmd->y, w, cmd->h, 0);
  return done;
}

// ReSys12 (diagnostics): who asked for full syncs that had to wait (return addresses of gpu_async_sync callers)
volatile uintptr_t rs12_sync_ra[8];
volatile uint32_t rs12_sync_ra_us[8];
static uintptr_t rs12_sync_caller;

static void gpu_async_sync_nocheck(struct psx_gpu *gpu)
{
  struct psx_gpu_async *agpu = gpu->async;

  if (!agpu || (RDPOS(agpu->idle) && agpu->pos_added == RDPOS(agpu->pos_used)))
    return;
  agpu_log(gpu, "agpu: sync %d\n", agpu->pos_added - agpu->pos_used);
  slock_lock(agpu->lock);
  if (agpu->idle && agpu->pos_added != RDPOS(agpu->pos_used)) {
    agpu_log(gpu, "agpu: idle %d\n", agpu->pos_added - agpu->pos_used);
    run_thread_nolock(agpu);
  }
  if (!agpu->idle) {
    assert(agpu->wait_mode == waitmode_none);
    agpu->wait_mode = waitmode_full;
    const uint64_t rs12_t0 = agpu_now_us();
    agpu_wait_add(agpu);
    { // ReSys12: account the wait to the caller
      const uint32_t d = (uint32_t)(agpu_now_us() - rs12_t0);
      int k;
      for (k = 0; k < 8 && rs12_sync_ra[k] && rs12_sync_ra[k] != rs12_sync_caller; k++)
        ;
      if (k < 8) {
        rs12_sync_ra[k] = rs12_sync_caller;
        rs12_sync_ra_us[k] += d;
      }
    }
  }
  slock_unlock(agpu->lock);
  assert(agpu->pos_added == agpu->pos_used);
  assert(agpu->idle);
}

void gpu_async_sync(struct psx_gpu *gpu)
{
  struct psx_gpu_async *agpu = gpu->async;

  if (!agpu)
    return;
  if (!RDPOS(agpu->idle) || agpu->pos_added != RDPOS(agpu->pos_used)) {
    rs12_sync_caller = (uintptr_t)__builtin_return_address(0);
    gpu_async_sync_nocheck(gpu);
  }

  if (unlikely(agpu->delayed_flip.src_x != -1)) {
    int src_x = agpu->delayed_flip.src_x;
    agpu_log(gpu, "agpu: delayed_flip 2\n");
    agpu->delayed_flip.src_x = -1;
    vout_update(gpu, src_x, agpu->delayed_flip.src_y);
  }
}

// ReSys12: any rectangle (the display area, or a VRAM read): 0 = nothing pending can touch it, 1 = wait until
// *pos_to, -1 = full sync
static int calc_rect_wait(struct psx_gpu *gpu, int so_x0, int so_y0, int so_x1, int so_y1,
    uint32_t *pos_to);

static int calc_scanout_wait(struct psx_gpu *gpu, int so_x0, int so_y0,
    uint32_t *pos_to)
{
  return calc_rect_wait(gpu, so_x0, so_y0, so_x0 + gpu->screen.hres, so_y0 + gpu->screen.vres, pos_to);
}

static int calc_rect_wait(struct psx_gpu *gpu, int so_x0, int so_y0, int so_x1, int so_y1,
    uint32_t *pos_to)
{
  struct psx_gpu_async *agpu = gpu->async;
  uint32_t pos;
  int c, i;

  pos = RDPOS(agpu->pos_used);
  if (RDPOS(agpu->idle) && agpu->pos_added == pos)
    return 0;
  i = agpu->pos_area;
  if (agpu->idle)
    // unlikely but possible - do a full sync
    return -1;
  if (so_x1 > 1024 || so_y1 > 1024) { // ReSys12: 1024-line VRAM
    agpu_log(gpu, "agpu: wrap %d,%d %dx%d\n",
      so_x0, so_y0, so_x1 - so_x0, so_y1 - so_y0);
    return -1;
  }
  else if (agpu->draw_areas[(i+1) & AGPU_AREAS_MASK].pos > pos) {
    agpu_log(gpu, "agpu: oldest draw area %d > %d\n",
      agpu->draw_areas[(i+1) & AGPU_AREAS_MASK].pos, pos);
    return -1;
  }

  for (c = 0, i = agpu->pos_area; c < AGPU_AREAS_CNT;
       c++, i = (i - 1) & AGPU_AREAS_MASK)
  {
    int area_x0 = agpu->draw_areas[i].x0, area_y0 = agpu->draw_areas[i].y0;
    int area_x1 = agpu->draw_areas[i].x1, area_y1 = agpu->draw_areas[i].y1;
    if (so_x1 <= area_x0 || area_x1 <= so_x0)
      /* no x intersect */;
    else if (so_y1 <= area_y0 || area_y1 <= so_y0)
      /* no y intersect */;
    else {
      agpu_log(gpu, "agpu: scanout #%d %d,%d %dx%d hit %d,%d %dx%d\n",
        c, so_x0, so_y0, so_x1 - so_x0, so_y1 - so_y0,
        area_x0, area_y0, area_x1 - area_x0, area_y1 - area_y0);
      break;
    }
    pos = RDPOS(agpu->pos_used);
    if (pos >= agpu->draw_areas[i].pos)
      return 0;
  }
  if (c == 0)
    // newest drawing area hits the scanout - full sync
    return -1;

  {
    extern volatile int rs12_rw_hit[5];
    rs12_rw_hit[0] = agpu->draw_areas[i].x0; rs12_rw_hit[1] = agpu->draw_areas[i].y0;
    rs12_rw_hit[2] = agpu->draw_areas[i].x1; rs12_rw_hit[3] = agpu->draw_areas[i].y1;
    rs12_rw_hit[4] = c;
  }
  i = (i + 1) & AGPU_AREAS_MASK;
  *pos_to = agpu->draw_areas[i].pos + 1;
  return 1;
}

static void do_scanout_wait(struct psx_gpu *gpu, int check_ret, uint32_t target)
{
  struct psx_gpu_async *agpu = gpu->async;
  if (check_ret == 1) {
    agpu_log(gpu, "agpu: wait %d/%d @ %u/%u\n",
        target - RDPOS(agpu->pos_used), agpu->pos_added -
        RDPOS(agpu->pos_used), RDPOS(agpu->pos_used), agpu->pos_added);
    slock_lock(agpu->lock);
    if (!agpu->idle && (int32_t)(agpu->pos_used - target) < 0) {
      assert(agpu->wait_mode == waitmode_none);
      agpu->pos_target = target;
      agpu->wait_mode = waitmode_target;
      rs12_wreason = 2;
      agpu_wait_add(agpu);
      rs12_wreason = 1;
    }
    slock_unlock(agpu->lock);
  }
  else {
    rs12_wreason = 3;
    gpu_async_sync_nocheck(gpu);
    rs12_wreason = 1;
  }
}

// ReSys12: before a VRAM read: wait only for queued commands that can write the rectangle (games read small
// texture/CLUT areas every frame: ehrgeiz, fgtlayer, pacapp2; a full sync each time serialized the two threads)
void gpu_async_sync_rect(struct psx_gpu *gpu, int x, int y, int w, int h)
{
  struct psx_gpu_async *agpu = gpu->async;
  uint32_t target = 0;
  int ret;

  if (!agpu || (RDPOS(agpu->idle) && agpu->pos_added == RDPOS(agpu->pos_used)))
    return;
  ret = calc_rect_wait(gpu, x, y, x + w, y + h, &target);
  if (ret == 0)
    return;
  if (ret == 1) {
    const uint64_t t0 = agpu_now_us();
    const uint32_t behind = agpu->pos_added - RDPOS(agpu->pos_used), need = target - RDPOS(agpu->pos_used);
    slock_lock(agpu->lock);
    if (!agpu->idle && (int32_t)(agpu->pos_used - target) < 0) {
      agpu->pos_target = target;
      agpu->wait_mode = waitmode_target;
      agpu_wait_add(agpu); // (counted as reason 1, sync)
    }
    slock_unlock(agpu->lock);
    { // ReSys12 diagnostics: the longest rectangle wait of the log window
      extern volatile int rs12_rw_hit[5], rs12_rw_max[11];
      const int us = (int)(agpu_now_us() - t0);
      if (us > rs12_rw_max[0]) {
        rs12_rw_max[0] = us; rs12_rw_max[1] = x; rs12_rw_max[2] = y; rs12_rw_max[3] = w; rs12_rw_max[4] = h;
        for (int k = 0; k < 5; k++)
          rs12_rw_max[5 + k] = rs12_rw_hit[k];
        rs12_rw_max[10] = (int)((uint64_t)need * 1000 / (behind ? behind : 1));
      }
    }
    return;
  }
  rs12_sync_caller = (uintptr_t)__builtin_return_address(0);
  { // ReSys12 diagnostics: the last read that needed a full sync
    extern volatile int rs12_rw_full[6];
    rs12_rw_full[0]++; rs12_rw_full[1] = x; rs12_rw_full[2] = y; rs12_rw_full[3] = w; rs12_rw_full[4] = h;
    rs12_rw_full[5] = agpu->pos_added - RDPOS(agpu->pos_used);
  }
  gpu_async_sync_nocheck(gpu);
}

// ReSys12: a VRAM read whose rectangle only queued CPU->VRAM writes can touch is answered from what VRAM holds now
// with those writes laid over it in queue order, instead of waiting for the renderer to reach them (ehrgeiz reads
// back a 16x64 area after rewriting a row of it, 11-17ms behind the renderer). 1 = dst holds the w*h pixels
int gpu_async_read_forward(struct psx_gpu *gpu, int x, int y, int w, int h, uint16_t *dst)
{
  struct psx_gpu_async *agpu = gpu->async;
  int hits[AGPU_AREAS_CNT], n = 0, c, i, k, row;
  uint32_t pos;

  if (!agpu || RDPOS(agpu->idle) || x + w > 1024 || y + h > 1024)
    return 0;
  pos = RDPOS(agpu->pos_used);
  if (agpu->draw_areas[(agpu->pos_area + 1) & AGPU_AREAS_MASK].pos > pos)
    return 0; // pending areas no longer all known
  for (c = 0, i = agpu->pos_area; c < AGPU_AREAS_CNT; c++, i = (i - 1) & AGPU_AREAS_MASK) {
    const struct pos_drawarea *a = &agpu->draw_areas[i];
    if (x + w > a->x0 && a->x1 > x && y + h > a->y0 && a->y1 > y) {
      if (!a->dma || (a->dma_r6 & 2))
        return 0; // drawing (or a masked write) can touch it: wait
      hits[n++] = i;
    }
    if (RDPOS(agpu->pos_used) >= a->pos)
      break;
  }
  if (c == AGPU_AREAS_CNT)
    return 0;

  for (row = 0; row < h; row++)
    memcpy(dst + row * w, gpu->vram + (y + row) * 1024 + x, w * 2);
  for (k = n - 1; k >= 0; k--) {
    const struct pos_drawarea *a = &agpu->draw_areas[hits[k]];
    uint32_t p = (a->dma - 1) & AGPU_BUF_MASK;
    const union cmd_dma_write *cmd = (const void *)&agpu->cmd_buffer[p];
    const int cx = cmd->x, cw = cmd->w, stride = (cmd->w + 1) / 2;
    const uint16_t msb = (a->dma_r6 & 1) << 15;
    int cy = cmd->y, ch = cmd->h;
    if ((LE32TOH(cmd->cmd) >> 24) != FAKECMD_DMA_WRITE)
      return 0; // (cannot happen: the area points at its command)
    p += sizeof(*cmd) / 4u;
    for (; ch > 0; ch--, cy++) {
      if (stride > (int)(AGPU_BUF_LEN - p))
        p = 0;
      const int ry = (cy & 1023) - y;
      if (ry >= 0 && ry < h) {
        const uint16_t *src = (const uint16_t *)&agpu->cmd_buffer[p];
        int x0 = cx > x ? cx : x, x1 = cx + cw < x + w ? cx + cw : x + w, px;
        for (px = x0; px < x1; px++)
          dst[ry * w + px - x] = src[px - cx] | msb;
      }
      p += stride;
    }
  }
  return 1;
}

int gpu_async_sync_scanout(struct psx_gpu *gpu)
{
  struct psx_gpu_async *agpu = gpu->async;
  uint32_t target = 0;
  int ret;

  if (!agpu)
    return 0;
  ret = calc_scanout_wait(gpu, gpu->screen.src_x, gpu->screen.src_y, &target);
  if (ret == 0)
    return 0;
  if (gpu->frameskip.set) {
    // delay. Could do it without fskip also, but that would cause frame/input lag
    if (agpu->delayed_flip.src_x != -1)
      agpu_log(gpu, "agpu: missed delayed_flip?\n");
    agpu->delayed_flip.src_x = gpu->screen.src_x;
    agpu->delayed_flip.src_y = gpu->screen.src_y;
    return 1;
  }
  do_scanout_wait(gpu, ret, target);
  return 0;
}

void gpu_async_try_delayed_flip(struct psx_gpu *gpu, int force)
{
  struct psx_gpu_async *agpu = gpu->async;
  uint32_t target = 0;
  int check_ret = 0;

  if (!agpu || agpu->delayed_flip.src_x == -1)
    return;
  check_ret = calc_scanout_wait(gpu, gpu->screen.src_x, gpu->screen.src_y, &target);
  if (force) {
    do_scanout_wait(gpu, check_ret, target);
    check_ret = 0;
  }
  if (check_ret == 0) {
    int src_x = agpu->delayed_flip.src_x;
    agpu_log(gpu, "agpu: delayed_flip %d\n", force);
    agpu->delayed_flip.src_x = -1;
    vout_update(gpu, src_x, agpu->delayed_flip.src_y);
  }
}

void gpu_async_sync_ecmds(struct psx_gpu *gpu)
{
  struct psx_gpu_async *agpu = gpu->async;
  if (!agpu)
    return;
  if (RDPOS(agpu->idle) && agpu->pos_added == RDPOS(agpu->pos_used)) {
    memcpy(agpu->ex_regs + 1, gpu->ex_regs + 1, 6*4);
    add_draw_area_e(agpu, agpu->pos_added, 0, gpu->ex_regs);
    renderer_sync_ecmds(agpu->ex_regs);
    return;
  }
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  uint32_t i, ecmds[8];
  for (i = 1; i <= 6; i++)
    ecmds[i] = HTOLE32(ecmds_[i]);
#else
  const uint32_t *ecmds = gpu->ex_regs;
#endif
  do_add_with_wait(agpu, ecmds + 1, 6);
}

static void psx_gpu_async_free(struct psx_gpu_async *agpu)
{
  agpu->exit = 1;
  if (agpu->lock) {
    slock_lock(agpu->lock);
    if (agpu->cond_use)
      scond_signal(agpu->cond_use);
    slock_unlock(agpu->lock);
  }
  if (agpu->thread) {
    sthread_join(agpu->thread);
    agpu->thread = NULL;
  }
  if (agpu->cond_add) { scond_free(agpu->cond_add); agpu->cond_add = NULL; }
  if (agpu->cond_use) { scond_free(agpu->cond_use); agpu->cond_use = NULL; }
  if (agpu->lock)     { slock_free(agpu->lock); agpu->lock = NULL; }
  free(agpu);
}

void gpu_async_stop(struct psx_gpu *gpu)
{
  if (gpu->async) {
    gpu_async_sync(gpu);
    psx_gpu_async_free(gpu->async);
    gpu->async = NULL;
  }
}

void gpu_async_enable(struct psx_gpu *gpu, int enable)
{
  struct psx_gpu_async *agpu;
  if (enable < 0)
    enable = pcsxr_sthread_core_count > 1;
  if (!gpu->async == !enable)
    return;
  if (!enable) {
    gpu_async_stop(gpu);
    return;
  }

  assert(AGPU_DMA_MAX <= AGPU_BUF_LEN / 2);

  agpu = calloc(1, sizeof(*agpu));
  if (agpu) {
    agpu->delayed_flip.src_x = -1;
    agpu->delayed_flip.src_y = -1;
    agpu->lock = slock_new();
    agpu->cond_add = scond_new();
    agpu->cond_use = scond_new();
    if (agpu->lock && agpu->cond_add && agpu->cond_use) {
      gpu->async = agpu;
      agpu->thread = pcsxr_sthread_create(gpu_async_thread, PCSXRT_GPU);
    }
    if (agpu->thread) {
      gpu_async_sync_ecmds(gpu);
      return;
    }
  }

  SysPrintf("gpu thread init failed\n");
  gpu->async = NULL;
  if (agpu)
    psx_gpu_async_free(agpu);
}

// vim:shiftwidth=2:expandtab

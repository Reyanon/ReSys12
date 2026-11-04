// ReSys12 for Vita: Namco System 12 emulator. Emulation on CPU0, sound thread CPU1, GPU thread CPU2, vitaGL output.
// - clock 444MHz, UDP log, JIT region for the R3000A recompiler
// - start screen game list (frontend/menu.c, ReST-V UI): cursor on the last game (game.txt); nothing starts on its own.
//   The one-shot ux0:data/resys12/autostart (measurement tools) skips the list and does not touch game.txt
// - ROM load through the cache ux0:data/resys12/cache/<set>.bin with a loading screen
// - SELECT+START or PS = in-game menu, SELECT+L / SELECT+R = quick save / load (current slot); a double PS press goes home
//   (kernel helper module/rs12_kctrl.skprx, which also installs updates found on GitHub)
// - keeps the screen on (power tick) and exits on START+SELECT+L+R, the menu, or the ux0:data/resys12/exit request
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <malloc.h>
#include <psp2/power.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/clock.h"
#include "platform/jit.h"
#include "platform/log.h"
#include "platform/timer.h"
#include "rs12_jit.h"
#include "common/re_pad.h"
#include "frontend/audio.h"
#include "frontend/menu.h"
#include "frontend/resample.h"
#include "frontend/touch.h"
#include "frontend/update.h"
#include "rs12_kctrl.h"
#include "video/video.h"

#include "psxcommon.h"
#include "r3000a.h"
#include "psxevents.h"
#include "psxinterpreter.h"
#include "new_dynarec/new_dynarec.h"
#include "plugins.h"
#include "../../../src/psx/frontend/plugin_lib.h"
#include "s12.h"
#include "s12_rom.h"
#include "s12_state.h"
#include "h8.h"

#define DATA_DIR "ux0:data/resys12"
#define ROM_DIR DATA_DIR "/roms"
#define CACHE_DIR DATA_DIR "/cache"

int _newlib_heap_size_user = 192 * 1024 * 1024; // ROM sets up to ~80MB + core + caches
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;

// vita-elf-create appends SCE data (a few KB) to the end of the text segment. If text ends just below a 64KB/1MB boundary it
// overlaps the data segment and fails, so push text past the boundary with read-only padding
// (adjust the size if build error "segment 1 overlaps" shows up again; same fix as ReST-V)
__attribute__((used)) static const unsigned char s_text_pad[32 * 1024] = { 1 };

extern void rs12_psx_bind(void);
extern uint64_t s12_sub_profile_ns(void);
extern volatile uint64_t rs12_gpu_busy_us, rs12_gpu_wait_us; // gpulib gpu_async.c (ReSys12 profiling)
extern volatile uint32_t rs12_gpu_act, rs12_gpu_tex8_updates;   // gpu_neon (ReSys12 profiling)
// ReSys12: split rendering (gpu_neon psx_gpu_if.c): second renderer instance on a CPU1 helper thread
extern void rs12_gpu_split_enable(int enable);
extern int rs12_gpu_split_active(void);
extern volatile uint32_t rs12_gpu_act_b;
extern volatile uint64_t rs12_gpu_split_busy_us, rs12_gpu_split_a_us, rs12_gpu_split_join_us;
extern volatile uint32_t rs12_gpu_split_chunks, rs12_gpu_split_barriers, rs12_gpu_split_stolen;
extern volatile uint32_t rs12_gpu_split_mismatch, rs12_gpu_split_ratio;

// ---- sampling profiler of the GPU thread (ux0:data/resys12/sampler): what gpu_neon is doing, every ~250us ----
// rs12_gpu_act: command byte 0x00-0xff, 0x1xx = rasterizing blocks queued by command xx, 0x1fe = other work, 0x1ff = idle
#ifdef RS12_NDRC_PROFILE
// recompiler functions (new_dynarec.c -finstrument-functions): innermost (self) and anywhere on the stack (incl),
// logged as offsets from new_dyna_clear_cache so addr2line can resolve them
extern volatile uintptr_t rs12_ndrc_ct_sp;
extern volatile void *rs12_ndrc_fn[32];
extern volatile int rs12_ndrc_fn_depth;
void new_dyna_clear_cache(void *start, void *end);
#define NS_SLOTS 512
static uintptr_t s_ns_fn[NS_SLOTS];
static uint32_t s_ns_self[NS_SLOTS], s_ns_incl[NS_SLOTS], s_ns_total;
static int ns_slot(uintptr_t fn)
{
	uint32_t h = (uint32_t)(fn >> 2) * 2654435761u >> 23;
	for (int k = 0; k < NS_SLOTS; k++, h = (h + 1) & (NS_SLOTS - 1)) {
		if (s_ns_fn[h] == fn)
			return h;
		if (!s_ns_fn[h]) {
			s_ns_fn[h] = fn;
			return h;
		}
	}
	return -1;
}
static void ndrc_sample(void)
{
	int d = rs12_ndrc_fn_depth;
	if (d <= 0)
		return;
	if (d > 32)
		d = 32;
	uintptr_t st[32];
	for (int k = 0; k < d; k++)
		st[k] = (uintptr_t)rs12_ndrc_fn[k];
	s_ns_total++;
	int h = ns_slot(st[d - 1]);
	if (h >= 0)
		s_ns_self[h]++;
	for (int k = 0; k < d; k++) {
		int seen = 0;
		for (int m = 0; m < k; m++)
			seen |= st[m] == st[k];
		if (!seen && (h = ns_slot(st[k])) >= 0)
			s_ns_incl[h]++;
	}
}
static void ndrc_sample_log_one(const char *what, uint32_t *v)
{
	char line[600];
	int n = snprintf(line, sizeof(line), "ndrcsample %s %u:", what, s_ns_total);
	for (int k = 0; k < 24 && n < (int)sizeof(line) - 24; k++) {
		uint32_t best = 0, bi = 0;
		for (uint32_t i = 0; i < NS_SLOTS; i++)
			if (v[i] > best) {
				best = v[i];
				bi = i;
			}
		if (!best)
			break;
		n += snprintf(line + n, sizeof(line) - n, " %x:%.1f", (unsigned)(s_ns_fn[bi] - (uintptr_t)new_dyna_clear_cache),
			100.0 * best / s_ns_total);
		v[bi] = 0;
	}
	RS12_LOG("%s", line);
}
static void ndrc_sample_log(void)
{
	if (s_ns_total < 20)
		return;
	ndrc_sample_log_one("self", s_ns_self);
	ndrc_sample_log_one("incl", s_ns_incl);
	memset(s_ns_fn, 0, sizeof(s_ns_fn));
	memset(s_ns_self, 0, sizeof(s_ns_self));
	memset(s_ns_incl, 0, sizeof(s_ns_incl));
	s_ns_total = 0;
}
#endif

#ifdef RS12_SUB_PROFILE
static int gpu_sampler_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	// ReSys12: 0x2xx entries = the split rendering helper (rs12_gpu_act_b), 0x1fd = renderer waits for it
	static uint32_t hist[0x400];
	// per instance: other, gradients, spans, block setup, split check, triangle prepare, -, rasterizing (0x1xx)
	uint32_t stage[2][8] = { { 0 } };
	uint32_t total = 0, last_tex8 = rs12_gpu_tex8_updates;
	uint32_t h8ph[H8PH_N] = { 0 };
	uint64_t t_last = rs12_time_us();
	for (;;) {
		sceKernelDelayThread(250);
		const uint32_t act = rs12_gpu_act, act_b = rs12_gpu_act_b;
		hist[act & 0x1ff]++;
		h8ph[h8_phase < H8PH_N ? h8_phase : 0]++;
		if ((act & 0x1ff) < 0x1fd)
			stage[0][(act & 0x100) ? 7 : (act >> 12) & 7]++;
		if (rs12_gpu_split_active()) {
			hist[0x200 | (act_b & 0x1ff)]++;
			if ((act_b & 0x1ff) < 0x1fd)
				stage[1][(act_b & 0x100) ? 7 : (act_b >> 12) & 7]++;
		}
		total++;
#ifdef RS12_NDRC_PROFILE
		ndrc_sample();
#endif
		const uint64_t now = rs12_time_us();
		if (now - t_last < 5000000)
			continue;
#ifdef RS12_NDRC_PROFILE
		ndrc_sample_log();
#endif
		char line[512];
		const uint32_t tex8 = rs12_gpu_tex8_updates;
		extern volatile uint32_t rs12_gpu_tex4_updates;
		static uint32_t last_tex4;
		const uint32_t tex4 = rs12_gpu_tex4_updates;
		int n = snprintf(line, sizeof(line), "gpusample %u tex8upd/s %.1f tex4upd/s %.1f:", total, (tex8 - last_tex8) / 5.0, (tex4 - last_tex4) / 5.0);
		last_tex4 = tex4;
		for (int k = 0; k < 20 && n < (int)sizeof(line) - 24; k++) {
			uint32_t best = 0, bi = 0;
			for (uint32_t i = 0; i < 0x400; i++)
				if (hist[i] > best) {
					best = hist[i];
					bi = i;
				}
			if (!best)
				break;
			n += snprintf(line + n, sizeof(line) - n, " %03x %.1f%%", bi, 100.0 * best / total);
			hist[bi] = 0;
		}
		RS12_LOG("%s", line);
		for (int k = 0; k < 2; k++)
			RS12_LOG("gpustage %c %% (other/grad/spans/blocks/splitchk/prep/tex8/raster): %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f",
				'A' + k, 100.0 * stage[k][0] / total, 100.0 * stage[k][1] / total, 100.0 * stage[k][2] / total,
				100.0 * stage[k][3] / total, 100.0 * stage[k][4] / total, 100.0 * stage[k][5] / total, 100.0 * stage[k][6] / total,
				100.0 * stage[k][7] / total);
		RS12_LOG("h8phase %% of time (run/code/blk/interp/update/idle/ext/chip/call/help): %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f",
			100.0 * h8ph[1] / total, 100.0 * h8ph[2] / total, 100.0 * h8ph[3] / total, 100.0 * h8ph[4] / total,
			100.0 * h8ph[5] / total, 100.0 * h8ph[6] / total, 100.0 * h8ph[7] / total, 100.0 * h8ph[8] / total,
			100.0 * h8ph[9] / total, 100.0 * h8ph[10] / total);
		memset(h8ph, 0, sizeof(h8ph));
		memset(stage, 0, sizeof(stage));
		memset(hist, 0, sizeof(hist));
		total = 0;
		last_tex8 = tex8;
		t_last = now;
	}
	return 0;
}
#endif

// ---- profiling: time spent in the GPU rasterizer (DMA2 command lists) per frame ----
static GPUdmaChain s_gpu_dma_chain;
static GPUwriteDataMem s_gpu_write_mem;
static uint64_t s_gpu_us;

static long gpu_dma_chain_timed(uint32_t *ram, uint32_t addr, uint32_t *progress, int32_t *cycles_last)
{
	const uint64_t t0 = rs12_time_us();
	const long r = s_gpu_dma_chain(ram, addr, progress, cycles_last);
	s_gpu_us += rs12_time_us() - t0;
	return r;
}

static void gpu_write_mem_timed(uint32_t *mem, int count)
{
	const uint64_t t0 = rs12_time_us();
	s_gpu_write_mem(mem, count);
	s_gpu_us += rs12_time_us() - t0;
}
extern void (*rs12_GPUrearmedCallbacks)(const struct rearmed_cbs *);

static volatile int s_quit;
// The exit file says "n" (measurement tools, as ReST-V): the app relaunches itself after a clean exit, into the game of a
// restart file or else the game list. The app never goes back to the home screen between games, so the screen stays on
// and a launch can never meet an app still closing (error C1-2569-2)
static volatile int s_exit_next;
static volatile int s_app_quit; // the process exits (s_quit only ends the running game or list)
static volatile int s_emu_up; // psxInit done: the emulation threads have to be stopped at the end of the game
static volatile uint64_t s_hb_t; // last progress of the running game (frame end), for the hang check
static int s_hangdump;          // ux0:data/resys12/hangdump (dev builds): a game stuck for 4 s crashes on purpose,
                                // so the core dump shows every thread's stack
// test recording (ux0:data/resys12/audiorec = seconds, one-shot): the rendered C352 output (before resampling) is
// collected here and the watch thread writes it to audio_rec.raw (s16 stereo at the render rate)
static int16_t *s_rec;
static uint32_t s_rec_len, s_rec_cap; // stereo frames
static volatile int s_rec_ready;

static int s_il_mode = 2; // allow_interlace: 2 = auto, 1 = field rendering (renderer overloaded)
volatile uint64_t rs12_ndrc_compile_us; // new_dynarec.c: recompiler time / blocks compiled
volatile uint32_t rs12_ndrc_blocks;
volatile uint64_t rs12_ndrc_cpu_us; // new_dynarec.c: the compiling thread's run time in the compiler

// run time of the calling thread (µs)
uint64_t rs12_thread_run_us(void)
{
	SceKernelThreadInfo ti;
	ti.size = sizeof(ti);
	if (sceKernelGetThreadInfo(0, &ti) < 0)
		return 0;
	return (uint64_t)ti.runClocks;
}
volatile int rs12_rw_hit[5], rs12_rw_max[11]; // gpu_async.c: VRAM read rectangle waits
volatile int rs12_rw_full[6]; // gpu_async.c: VRAM reads that needed a full sync (count, last rect, queued words)
volatile uint32_t rs12_fwd_n, rs12_fwd_bad;    // gpu.c: VRAM reads answered from the queue / that differed (check)
volatile uint64_t rs12_ndrc_sync_us; // new_dynarec.c: sceKernelSyncVMDomain time / calls
volatile uint32_t rs12_ndrc_syncs;
extern volatile uint64_t rs12_ndrc_pass_us[10]; // new_dynarec.c: time per compile pass
extern volatile unsigned int rs12_emu_frame;     // vout_pl.c: emulated frame number (tags flips)
extern int rs12_video_lowlag;                    // video_gl.c: show the frame just emulated when it is ready
extern volatile uint32_t rs12_video_qdepth;       // video_gl.c: frames waiting at the presents
extern volatile uint32_t rs12_ndrc_insns;        // new_dynarec.c: instructions compiled
extern uint32_t rs12_ndrc_hot[256][3];         // new_dynarec.c: compiles per block start

// ---- EEPROM (AT28C16: test mode settings, coin counters, records) kept in ux0:data/resys12/nvram/<set>.eep ----
// The emulation thread only copies a changed EEPROM into s_eep_buf; the watch thread writes the file (no memory card
// I/O on the main thread). At exit the main thread writes what is left.
#define NVRAM_DIR DATA_DIR "/nvram"
static char s_eep_path[96];
static uint8_t s_eep_buf[sizeof(s12.eeprom)];
static volatile int s_eep_pending, s_eep_writing;

static void eep_load(const char *game)
{
	sceIoMkdir(NVRAM_DIR, 0777);
	snprintf(s_eep_path, sizeof(s_eep_path), NVRAM_DIR "/%s.eep", game);
	const SceUID fd = sceIoOpen(s_eep_path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	uint8_t buf[sizeof(s12.eeprom)];
	if (sceIoRead(fd, buf, sizeof(buf)) == (int)sizeof(buf)) {
		memcpy(s12.eeprom, buf, sizeof(buf));
		RS12_LOG("eeprom: loaded %s", s_eep_path);
	}
	sceIoClose(fd);
}

static void eep_write(const uint8_t *data)
{
	const SceUID fd = sceIoOpen(s_eep_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return;
	sceIoWrite(fd, data, sizeof(s12.eeprom));
	sceIoClose(fd);
}

// ---- recompiler warm start: the block entry points compiled in earlier sessions, ux0:data/resys12/cache/<set>.blk
// (address, register flags pairs), compiled ahead of time between frames (emu_if.c). The emulation thread collects
// the list every 30 s after new code was compiled; the watch thread writes it (and the main thread at exit).
#define WARM_MAX 32768
extern void rs12_ndrc_warm_set(const uint32_t *pairs, int n);
extern int rs12_ndrc_warm_collect(uint32_t *out, int max);
static char s_warm_path[96];
static uint32_t s_warm_buf[WARM_MAX * 2];
static int s_warm_n;
static volatile int s_warm_pending, s_warm_writing;

static void warm_load(const char *game)
{
	rs12_ndrc_warm_set(NULL, 0); // (no list from the previous game of this process)
	s_warm_path[0] = 0;
	s_warm_n = 0;
	SceIoStat nst;
	if (sceIoGetstat(DATA_DIR "/nowarm", &nst) >= 0) { // measurements: a cold start (the list is not read or written)
		RS12_LOG("dynarec warm start: off (nowarm)");
		return;
	}
	sceIoMkdir(DATA_DIR "/cache", 0777);
	snprintf(s_warm_path, sizeof(s_warm_path), DATA_DIR "/cache/%s.blk", game);
	const SceUID fd = sceIoOpen(s_warm_path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	const int n = sceIoRead(fd, s_warm_buf, sizeof(s_warm_buf)) / 8;
	sceIoClose(fd);
	if (n > 0) {
		rs12_ndrc_warm_set(s_warm_buf, n);
		RS12_LOG("dynarec warm start: %d blocks from %s", n, s_warm_path);
	}
}

static void warm_write(void)
{
	const SceUID fd = sceIoOpen(s_warm_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return;
	sceIoWrite(fd, s_warm_buf, s_warm_n * 8);
	sceIoClose(fd);
}

// end of each frame (emulation thread)
static void warm_frame(uint32_t frames)
{
	static int last_compiles = -1;
	if (frames % 1800 != 1799 || !s_warm_path[0] || ndrc_g.did_compile == last_compiles ||
	    __atomic_load_n(&s_warm_pending, __ATOMIC_ACQUIRE))
		return;
	last_compiles = ndrc_g.did_compile;
	s_warm_n = rs12_ndrc_warm_collect(s_warm_buf, WARM_MAX);
	__atomic_store_n(&s_warm_pending, 1, __ATOMIC_RELEASE);
}

// ---- measurements: GPU input recording and replay (gpulib rs12_gpu_rec_*), a fixed piece of rendering timed on the
// device. ux0:data/resys12/gpurec "F N": record frames F..F+N to gpurec.bin; gpuplay "F R": at frame F replay
// gpurec.bin R times (no flips: the rendering alone), each timed, with its VRAM hash (the game goes on afterwards
// with that VRAM)
extern int rs12_gpu_rec_start(size_t max_words, uint32_t frames);
extern const uint32_t *rs12_gpu_rec_result(size_t *words);
extern int rs12_gpu_replay(const uint32_t *b, size_t words, int with_vblank);
extern uint64_t rs12_gpu_vram_hash(void);
static int s_grec_f = -1, s_grec_n, s_grec_state, s_gplay_f = -1, s_gplay_r;
static int s_split_prio = -1; // measurements (splitprio, hex): the first split render helper's priority, at frame 60
static void gpu_rec_init(void)
{
	s_grec_f = s_gplay_f = -1;
	s_grec_state = 0;
	s_split_prio = 0x4A; // (the default again after a measurement changed it)
	FILE *sp = fopen(DATA_DIR "/splitprio", "r");
	if (sp) {
		if (fscanf(sp, "%i", &s_split_prio) != 1)
			s_split_prio = 0x4A;
		fclose(sp);
	}
	FILE *f = fopen(DATA_DIR "/gpurec", "r");
	if (f) {
		if (fscanf(f, "%d %d", &s_grec_f, &s_grec_n) != 2)
			s_grec_f = -1;
		fclose(f);
	}
	f = fopen(DATA_DIR "/gpuplay", "r");
	if (f) {
		if (fscanf(f, "%d %d", &s_gplay_f, &s_gplay_r) != 2)
			s_gplay_f = -1;
		fclose(f);
	}
}
static void gpu_rec_frame(uint32_t frames)
{
	if (s_split_prio > 0 && frames == 60) {
		extern void rs12_split_helper_prio(int prio);
		rs12_split_helper_prio(s_split_prio);
		RS12_LOG("split helper priority %x", s_split_prio);
	}
	if (s_grec_f >= 0 && s_grec_state == 0 && frames == (uint32_t)s_grec_f) {
		s_grec_state = rs12_gpu_rec_start(6u << 20, (uint32_t)s_grec_n) ? 1 : 3;
		RS12_LOG("gpurec: start at f%u for %d frames: %s", frames, s_grec_n, s_grec_state == 1 ? "ok" : "no memory");
	}
	size_t words = 0;
	const uint32_t *rb = s_grec_state == 1 ? rs12_gpu_rec_result(&words) : NULL;
	if (rb) {
		const SceUID fd = sceIoOpen(DATA_DIR "/gpurec.bin", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		if (fd >= 0) {
			sceIoWrite(fd, rb, (SceSize)(words * 4));
			sceIoClose(fd);
		}
		RS12_LOG("gpurec: %u words written", (unsigned)words);
		s_grec_state = 2;
	}
	if (s_gplay_f < 0 || frames != (uint32_t)s_gplay_f)
		return;
	const SceUID fd = sceIoOpen(DATA_DIR "/gpurec.bin", SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	const SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	uint32_t *pb = (uint32_t *)malloc((size_t)sz);
	if (pb && sceIoRead(fd, pb, (SceSize)sz) == (int)sz) {
		extern volatile uint64_t rs12_gpu_busy_us, rs12_gpu_split_busy_us, rs12_gpu_split_busy_c_us;
		for (int r = 0; r < s_gplay_r; r++) {
			const uint64_t g0 = rs12_gpu_busy_us, b0 = rs12_gpu_split_busy_us, c0 = rs12_gpu_split_busy_c_us;
			const uint64_t t0 = rs12_time_us();
			const int n = rs12_gpu_replay(pb, (size_t)sz / 4, 0);
			const uint64_t dt = rs12_time_us() - t0;
			RS12_LOG("gpuplay %d: %d frames in %.1f ms (%.2f ms/frame; renderer %.2f B %.2f C %.2f ms/frame) vram %016llx",
				r, n, dt / 1000.0, n > 0 ? dt / 1000.0 / n : 0.0, n > 0 ? (rs12_gpu_busy_us - g0) / 1000.0 / n : 0.0,
				n > 0 ? (rs12_gpu_split_busy_us - b0) / 1000.0 / n : 0.0, n > 0 ? (rs12_gpu_split_busy_c_us - c0) / 1000.0 / n : 0.0,
				(unsigned long long)rs12_gpu_vram_hash());
		}
	}
	free(pb);
	sceIoClose(fd);
}

// end of each frame (emulation thread): hand a changed EEPROM to the watch thread unless a write is still waiting
static void eep_frame(void)
{
	if (!s12.eeprom_dirty || __atomic_load_n(&s_eep_pending, __ATOMIC_ACQUIRE))
		return;
	memcpy(s_eep_buf, s12.eeprom, sizeof(s_eep_buf));
	s12.eeprom_dirty = 0;
	__atomic_store_n(&s_eep_pending, 1, __ATOMIC_RELEASE);
}

// ---- save states: ux0:data/resys12/state/<set>_<slot>.st. The emulation thread serializes (s12_state_*), the watch
// thread writes / reads the file (no memory card I/O on the main thread), the emulation waits for it ----
#include <zstd.h>
#define STATE_DIR DATA_DIR "/state"
static SceUID s_watch_wake = -1;  // wakes the watch thread early (state requests)
static SceUID s_watch_thread = -1;
static volatile int s_st_req;     // 1 write s_st_buf, 2 read into s_st_buf; back to 0 when done
static volatile int s_st_ok;
static uint8_t *s_st_buf;
static size_t s_st_size;
static volatile size_t s_st_got;  // read: bytes of the file in s_st_buf so far (s_st_buf / s_st_size set once reading)
static volatile int s_st_reading; // read: s_st_buf and s_st_size are set
#define STATE_READ_PIECE (256 * 1024)
static char s_st_path[128];

// State files are zstd frames (level 1, written as a stream through a small buffer): 6.4MB of state is 1-3MB, and
// the card reads ~13MB/s, so loading waits about a third as long. Files that do not start with the zstd magic are
// raw states (earlier builds, the PC harness's measurement scenes) and load as before
#define STATE_ZSTD_LEVEL 1
static int state_write_zstd(SceUID fd, const uint8_t *src, size_t size)
{
	ZSTD_CCtx *cc = ZSTD_createCCtx();
	const size_t cap = ZSTD_CStreamOutSize();
	uint8_t *out = (uint8_t *)malloc(cap);
	int ok = cc && out && !ZSTD_isError(ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, STATE_ZSTD_LEVEL));
	ZSTD_inBuffer in = { src, size, 0 };
	for (int done = 0; ok && !done;) {
		ZSTD_outBuffer ob = { out, cap, 0 };
		const size_t left = ZSTD_compressStream2(cc, &ob, &in, ZSTD_e_end);
		if (ZSTD_isError(left) || (ob.pos && sceIoWrite(fd, out, ob.pos) != (int)ob.pos))
			ok = 0;
		done = left == 0;
	}
	free(out);
	ZSTD_freeCCtx(cc);
	return ok;
}

static void state_io(void) // watch thread
{
	const int req = s_st_req;
	int ok = 0;
	if (req == 1) {
		sceIoMkdir(STATE_DIR, 0777);
		const SceUID fd = sceIoOpen(s_st_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		if (fd >= 0) {
			ok = state_write_zstd(fd, s_st_buf, s_st_size);
			sceIoClose(fd);
			if (!ok)
				sceIoRemove(s_st_path); // (no half-written state left behind)
		}
	} else if (req == 2) {
		const uint64_t r0 = rs12_time_us();
		const SceUID fd = sceIoOpen(s_st_path, SCE_O_RDONLY, 0);
		if (fd >= 0) {
			const SceOff n = sceIoLseek(fd, 0, SCE_SEEK_END);
			sceIoLseek(fd, 0, SCE_SEEK_SET);
			if (n > 0 && n < 64 * 1024 * 1024 && (s_st_buf = malloc((size_t)n))) {
				s_st_size = (size_t)n;
				// in pieces, each published as it lands: the emulation thread unpacks behind the read (state_read)
				__atomic_store_n(&s_st_reading, 1, __ATOMIC_RELEASE);
				size_t got = 0;
				while (got < s_st_size) {
					const size_t want = s_st_size - got < STATE_READ_PIECE ? s_st_size - got : STATE_READ_PIECE;
					const int r = sceIoRead(fd, s_st_buf + got, want);
					if (r <= 0)
						break;
					got += (size_t)r;
					__atomic_store_n(&s_st_got, got, __ATOMIC_RELEASE);
				}
				ok = got == s_st_size;
			}
			sceIoClose(fd);
		}
		RS12_LOG("state: read %u ms (%u bytes)", (unsigned)((rs12_time_us() - r0) / 1000), (unsigned)s_st_size);
	}
	if (req == 1) { // the emulation went on: report here and free the buffer
		RS12_LOG("state: wrote %s (%u bytes) %s", s_st_path, (unsigned)s_st_size, ok ? "ok" : "failed");
		if (!ok)
			rs12_osd_show("Save state: write failed");
		free(s_st_buf);
		s_st_buf = NULL;
	}
	s_st_ok = ok;
	__atomic_store_n(&s_st_req, 0, __ATOMIC_RELEASE);
}

static void state_wait(void);
static void state_request(int req);

// emulation thread: read the state file at s_st_path into s_st_buf / s_st_size (raw state); 1 = ok. A zstd state
// (~2MB) is unpacked here while the watch thread is still reading it: reading takes ~155ms and unpacking ~195ms on
// the device (~33MB/s), one after the other they took ~350ms
static int state_read(void)
{
	s_st_buf = NULL;
	s_st_size = 0;
	__atomic_store_n(&s_st_got, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&s_st_reading, 0, __ATOMIC_RELEASE);
	state_request(2);
	// the first bytes tell zstd from a raw state
	while (__atomic_load_n(&s_st_req, __ATOMIC_ACQUIRE) &&
		!(__atomic_load_n(&s_st_reading, __ATOMIC_ACQUIRE) && __atomic_load_n(&s_st_got, __ATOMIC_ACQUIRE) >= 32))
		sceKernelDelayThread(500);
	const size_t got0 = __atomic_load_n(&s_st_got, __ATOMIC_ACQUIRE);
	const uint8_t *src = s_st_buf;
	if (!src || got0 < 4 || src[0] != 0x28 || src[1] != 0xb5 || src[2] != 0x2f || src[3] != 0xfd) { // ZSTD magic
		state_wait();
		return s_st_ok && s_st_buf;
	}
	const uint64_t t0 = rs12_time_us();
	const unsigned long long raw = ZSTD_getFrameContentSize(src, got0);
	uint8_t *dst = raw < 64 * 1024 * 1024 ? (uint8_t *)malloc((size_t)raw) : NULL;
	ZSTD_DStream *ds = dst ? ZSTD_createDStream() : NULL;
	int ok = ds && !ZSTD_isError(ZSTD_initDStream(ds));
	ZSTD_inBuffer in = { src, 0, 0 };
	ZSTD_outBuffer out = { dst, (size_t)raw, 0 };
	while (ok) {
		const int reading = __atomic_load_n(&s_st_req, __ATOMIC_ACQUIRE) != 0;
		in.size = __atomic_load_n(&s_st_got, __ATOMIC_ACQUIRE);
		if (in.pos < in.size) {
			const size_t left = ZSTD_decompressStream(ds, &out, &in);
			if (ZSTD_isError(left))
				ok = 0;
			else if (left == 0)
				break; // the frame is complete
		} else if (!reading)
			ok = 0; // the file ended (or failed) before the frame did
		else
			sceKernelDelayThread(300);
	}
	ZSTD_freeDStream(ds);
	state_wait();
	ok = ok && s_st_ok && out.pos == raw;
	free(s_st_buf);
	s_st_buf = ok ? dst : NULL;
	s_st_size = ok ? (size_t)raw : 0;
	if (!ok)
		free(dst);
	RS12_LOG("state: unpacked in %u ms from the first piece", (unsigned)((rs12_time_us() - t0) / 1000));
	return ok;
}

static void state_wait(void) // emulation thread: the last request is done
{
	while (__atomic_load_n(&s_st_req, __ATOMIC_ACQUIRE))
		sceKernelDelayThread(2000);
}

static void state_request(int req) // emulation thread: hand over (a write goes on in the background)
{
	__atomic_store_n(&s_st_req, req, __ATOMIC_RELEASE);
	if (s_watch_wake >= 0)
		sceKernelSignalSema(s_watch_wake, 1);
}

static void sound_idle(void);

static void state_save(const char *game, int slot)
{
	state_wait();
	sound_idle();
	size_t size = 0;
	uint8_t *buf = s12_state_save(&size);
	if (buf) {
		snprintf(s_st_path, sizeof(s_st_path), STATE_DIR "/%s_%d.st", game, slot);
		s_st_buf = buf;
		s_st_size = size;
		state_request(1);
	}
	char msg[48];
	snprintf(msg, sizeof(msg), buf ? "State %d saved" : "State %d: save failed", slot);

	rs12_osd_show(msg);
}

// read and load a state file (emulation thread); 1 = loaded
static int state_load_file(const char *path)
{
	state_wait();
	sound_idle();
	snprintf(s_st_path, sizeof(s_st_path), "%s", path);
	int ok = state_read() && s12_state_load(s_st_buf, s_st_size) == 0;
	if (ok) {
		extern void rs12_ndrc_warm_boot_end(void);
		rs12_ndrc_warm_boot_end();
	}
	free(s_st_buf);
	s_st_buf = NULL;
	return ok;
}

static void state_load(const char *game, int slot)
{
	const uint64_t t0 = rs12_time_us();
	state_wait();
	sound_idle();
	const uint64_t t1 = rs12_time_us();
	snprintf(s_st_path, sizeof(s_st_path), STATE_DIR "/%s_%d.st", game, slot);
	const int read_ok = state_read();
	const uint64_t t2 = rs12_time_us();
	int ok = read_ok && s12_state_load(s_st_buf, s_st_size) == 0;
	RS12_LOG("state: load parts (ms) idle %u read %u apply %u", (unsigned)((t1 - t0) / 1000), (unsigned)((t2 - t1) / 1000),
		(unsigned)((rs12_time_us() - t2) / 1000));
	if (ok) {
		extern void rs12_ndrc_warm_boot_end(void);
		rs12_ndrc_warm_boot_end();
	}
	free(s_st_buf);
	s_st_buf = NULL;
	RS12_LOG("state: load slot %d %s in %u ms", slot, ok ? "ok" : "failed", (unsigned)((rs12_time_us() - t0) / 1000));
	char msg[48];
	snprintf(msg, sizeof(msg), ok ? "State %d loaded" : "State %d: nothing to load", slot);
	rs12_osd_show(msg);
}

// screen-on keeper, exit-request watcher and EEPROM writer (CPU1, low priority: no memory card I/O on the main thread)
static int watch_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	int n = 0;
	while (!s_app_quit) {
		if (__atomic_load_n(&s_eep_pending, __ATOMIC_ACQUIRE)) {
			s_eep_writing = 1;
			if (!s_quit)
				eep_write(s_eep_buf);
			__atomic_store_n(&s_eep_pending, 0, __ATOMIC_RELEASE);
			s_eep_writing = 0;
		}
		if (__atomic_load_n(&s_warm_pending, __ATOMIC_ACQUIRE)) {
			s_warm_writing = 1;
			if (!s_quit)
				warm_write();
			__atomic_store_n(&s_warm_pending, 0, __ATOMIC_RELEASE);
			s_warm_writing = 0;
		}
		if (__atomic_load_n(&s_st_req, __ATOMIC_ACQUIRE))
			state_io();
		if (s_rec_ready == 1) {
			const SceUID fd = sceIoOpen(DATA_DIR "/audio_rec.raw", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
			if (fd >= 0) {
				sceIoWrite(fd, s_rec, s_rec_len * 4);
				sceIoClose(fd);
			}
			RS12_LOG("audiorec: wrote %u frames", (unsigned)s_rec_len);
			s_rec_ready = 2;
		}
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
#ifdef RS12_DEBUG_TOOLS
		if (s_hangdump && s_emu_up && !s_quit && s_hb_t && rs12_time_us() - s_hb_t > 4000000) {
			RS12_LOG("hang: no frame for %llu ms, pc %08x: crashing for a core dump",
				(unsigned long long)((rs12_time_us() - s_hb_t) / 1000), psxRegs.pc);
			sceKernelDelayThread(300000);
			*(volatile int *)0 = 0;
		}
#endif
		if (++n % 2 == 0) {
			SceIoStat st;
			static int empty_seen;
			if (sceIoGetstat(DATA_DIR "/exit", &st) >= 0 && (st.st_size > 0 || ++empty_seen > 2)) {
				// (an empty file is read again on the next polls: the FTP upload creates it before writing the "n", and a
				// plain exit there left the measurement tools without a relaunch)
				char c = 0;
				const SceUID xf = sceIoOpen(DATA_DIR "/exit", SCE_O_RDONLY, 0);
				if (xf >= 0) {
					sceIoRead(xf, &c, 1);
					sceIoClose(xf);
				}
				s_exit_next = c == 'n';
				sceIoRemove(DATA_DIR "/exit");
				RS12_LOG("exit requested%s", s_exit_next ? " (next)" : "");
				s_quit = 1;
			}
		}
		SceUInt timeout = 1000 * 1000;
		if (s_watch_wake < 0 || sceKernelWaitSema(s_watch_wake, 1, &timeout) < 0)
			if (s_watch_wake < 0)
				sceKernelDelayThread(1000 * 1000);
	}
	return 0;
}

// ---- sound thread (CPU1): renders the C352 frame batches one frame behind the emulation ----
static SceUID s_snd_go, s_snd_done, s_snd_thread = -1;
static S12SoundBatch *volatile s_snd_batch;
static volatile uint32_t s_snd_us; // render time of the last batch

static int sound_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	static int16_t buf[S12_SOUND_MAX_FRAMES * 2];
	for (;;) {
		if (sceKernelWaitSema(s_snd_go, 1, NULL) < 0 || s_quit)
			break;
		S12SoundBatch *b = s_snd_batch;
		const uint64_t t0 = rs12_time_us();
		const int n = s12_sound_render(b, buf);
		if (n > 0 && s_rec && !s_rec_ready) {
			const uint32_t k = s_rec_len + (uint32_t)n <= s_rec_cap ? (uint32_t)n : s_rec_cap - s_rec_len;
			memcpy(s_rec + (size_t)s_rec_len * 2, buf, (size_t)k * 4);
			s_rec_len += k;
			if (s_rec_len == s_rec_cap)
				s_rec_ready = 1;
		}
		if (n > 0)
			rs12_audio_push(buf, n < RS12_RESAMPLE_MAX_IN ? n : RS12_RESAMPLE_MAX_IN);
		s_snd_us = (uint32_t)(rs12_time_us() - t0);
		sceKernelSignalSema(s_snd_done, 1);
	}
	return 0;
}

// end of an emulated frame: wait for the previous batch (normally long done), hand over this frame's batch
static S12SoundBatch *s_snd_prev;
static void sound_frame_end(void)
{
	S12SoundBatch *prev = s_snd_prev;
	if (prev)
		sceKernelWaitSema(s_snd_done, 1, NULL);
	S12SoundBatch *b = s12_sub_sound_frame(prev);
	s_snd_batch = b;
	s_snd_prev = b;
	sceKernelSignalSema(s_snd_go, 1);
}

// the batch in flight is rendered (save states touch the C352); the semaphore is left as it was
static void sound_idle(void)
{
	if (!s_snd_prev)
		return;
	sceKernelWaitSema(s_snd_done, 1, NULL);
	sceKernelSignalSema(s_snd_done, 1);
}

// ---- gpulib video output ----
static int s_vout_w = 640, s_vout_h = 480;

static int vout_open(void) { return 0; }
static void vout_close(void) {}

static void vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp)
{
	(void)raw_w; (void)raw_h;
	s_vout_w = w;
	s_vout_h = h;
	RS12_LOG("vout mode %dx%d bpp %d", w, h, bpp);
}

// new pictures the game handed to the display (gpulib flips only when the displayed frame changed), for the real
// frame rate: the loop rate stays at 59.94 even when the game drops frames
static uint32_t s_flips;

static volatile uint32_t s_flip_us; // time copying frames into the video texture (renderer thread)
static volatile uint32_t s_flip_late, s_flip_max;
extern volatile uint32_t rs12_video_repeats, rs12_video_skips; // video_gl.c // game frames shown more than 25ms after the last one, longest gap

static void vout_flip(const void *vram, int vram_offset, int bgr24, int x, int y, int w, int h, int dims_changed)
{
	(void)x; (void)y; (void)dims_changed;
	if (!vram || w <= 0 || h <= 0)
		return;
	s_flips++;
	const uint64_t t0 = rs12_time_us();
	{
		static uint64_t last;
		const uint32_t d = last ? (uint32_t)(t0 - last) : 0;
		last = t0;
		if (d > 25000 && d < 200000)
			s_flip_late++;
		if (d < 200000 && d > s_flip_max)
			s_flip_max = d;
	}
	if (w > RS12_VIDEO_TEX_W)
		w = RS12_VIDEO_TEX_W;
	if (h > RS12_VIDEO_TEX_H)
		h = RS12_VIDEO_TEX_H;
	const uint8_t *base = (const uint8_t *)vram + vram_offset;
	extern int rs12_vout_field;
	if (!bgr24 && rs12_vout_field && h > 256) {
		// field rendering (480i while the renderer is overloaded): only one field's lines are new, so show that field
		// alone (bob, no combing) and copy half the lines (the copy took ~2ms of the renderer's frame)
		const int f = rs12_vout_field - 1;
		h /= 2;
		uint16_t *dst = rs12_video_frame_begin16(w, h);
		rs12_video_frame_field(f);
		for (int j = 0; j < h; j++)
			memcpy(dst + j * RS12_VIDEO_TEX_W, base + (size_t)(2 * j + f) * 2048, (size_t)w * 2);
	} else if (!bgr24) {
		uint16_t *dst = rs12_video_frame_begin16(w, h);
		for (int j = 0; j < h; j++)
			memcpy(dst + j * RS12_VIDEO_TEX_W, base + (size_t)j * 2048, (size_t)w * 2);
	} else {
		uint32_t *dst = rs12_video_frame_begin(w, h);
		for (int j = 0; j < h; j++) {
			const uint8_t *p = base + (size_t)j * 2048;
			uint32_t *d = dst + j * RS12_VIDEO_TEX_W;
			for (int i = 0; i < w; i++, p += 3)
				d[i] = 0xFF000000u | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
		}
	}
	rs12_video_frame_end();
	s_flip_us += (uint32_t)(rs12_time_us() - t0);
}

// ---- analog controls of gun / wheel / lever games (s12_analog_kind): the aim stick (setting) gives the gun position
// (stick position = screen position, or the stick moves the aim), the wheel or the lever. The gun trigger, golgo13's scope
// sensor and the wheels' gas / brake are settings (default R / L / R / L). Returns extra game buttons ----
static float stick_axis(int v, int centre, float dead)
{
	float a = (float)(v - centre) / (float)(v < centre ? centre : 255 - centre);
	if (a > 1.0f) a = 1.0f;
	if (a < -1.0f) a = -1.0f;
	if (a > -dead && a < dead)
		return 0.0f;
	return a > 0 ? (a - dead) / (1.0f - dead) : (a + dead) / (1.0f - dead);
}

// the stick a game's analog control uses (gun, wheel or lever setting), or -1: it is not a lever then (map_input)
static int analog_stick(void)
{
	switch (s12_analog_kind(s12.game)) {
	case S12_ANALOG_GUN: return g_set.aim_stick;
	case S12_ANALOG_WHEEL: return g_set.wheel_stick;
	case S12_ANALOG_LEVER: return g_set.lever_stick;
	default: return -1;
	}
}

static uint32_t analog_input(const SceCtrlData *pad)
{
	const int kind = s12_analog_kind(s12.game);
	const int st = analog_stick();
	const uint32_t b = pad->buttons;
	uint32_t p = 0;
	// sticks rest off centre (this Vita: left 111,119): the first half second of a game gives each stick's centre (if
	// it is near 128)
	static int cal_n, cal[4], cen[4] = { 128, 128, 128, 128 };
	const int raw[4] = { pad->lx, pad->ly, pad->rx, pad->ry };
	if (cal_n < 30) {
		for (int k = 0; k < 4; k++)
			cal[k] += raw[k];
		if (++cal_n == 30) {
			for (int k = 0; k < 4; k++) {
				const int m = cal[k] / 30;
				if (m > 96 && m < 160)
					cen[k] = m;
			}
			RS12_LOG("stick centres L %d,%d R %d,%d", cen[0], cen[1], cen[2], cen[3]);
		}
	}
	const int ix = st == RS12_AIM_RIGHT ? 2 : 0, iy = ix + 1;
	if (kind == S12_ANALOG_GUN) {
		// sensitivity 10: the aim is where the stick points (or moves at full speed). Lower: the centre of the stick
		// range is slower (power curve) and the aim follows it smoothly; the edges stay reachable
		static float gx = 0.5f, gy = 0.5f;
		const float sens = (float)g_set.gun_sens / 10.0f;
		const int golgo = s12.game->state == S12_S_GOLGO13;
		if (g_set.touch_gun) {
			// Touch gun: the touch point aims (where it is on the game picture, turned back with the screen rotation)
			// and the trigger is held while the screen is touched; the aim stays where it was when let go
			int tx[6], ty[6];
			const int tn = rs12_touch_read(tx, ty);
			int ti = 0; // the first touch that is not on a touch button aims and fires
			while (ti < tn && g_set.touch_keys && rs12_touch_on_area(tx[ti], ty[ti]))
				ti++;
			if (ti < tn) {
				float fx, fy;
				rs12_video_screen_to_game((float)tx[ti], (float)ty[ti], &fx, &fy);
				gx = fx < 0.0f ? 0.0f : fx > 1.0f ? 1.0f : fx;
				gy = fy < 0.0f ? 0.0f : fy > 1.0f ? 1.0f : fy;
				p |= golgo ? S12_IN_B2 : S12_IN_B1;
			}
			s12_set_gun(0, gx, gy);
			rs12_video_crosshair(0, g_set.crosshair, gx, gy);
			if (b & rs12_key_code(g_set.gun_fire)) // (the trigger key works too)
				p |= golgo ? S12_IN_B2 : S12_IN_B1;
			if (golgo && (b & rs12_key_code(g_set.gun_sensor)))
				p |= S12_IN_B7; // scope sensor
			return p;
		}
		float ax = stick_axis(raw[ix], cen[ix], g_set.aim_mode == RS12_AIM_MOVE ? 0.12f : 0.04f);
		float ay = stick_axis(raw[iy], cen[iy], g_set.aim_mode == RS12_AIM_MOVE ? 0.12f : 0.04f);
		if (g_set.aim_mode == RS12_AIM_MOVE) { // full tilt at 10 crosses the screen in about 2/3 s
			gx += ax * 0.025f * sens;
			gy += ay * 0.025f * sens;
		} else {
			const float e = 1.0f + (1.0f - sens) * 1.2f, follow = 0.15f + 0.85f * sens;
			ax = ax < 0 ? -powf(-ax, e) : powf(ax, e);
			ay = ay < 0 ? -powf(-ay, e) : powf(ay, e);
			// the aim chases the stick target with a speed cap: 10 = no cap, 5 = about 3/4 s across the screen
			float dx = (0.5f + ax * 0.55f - gx) * follow, dy = (0.5f + ay * 0.55f - gy) * follow;
			const float cap = 0.004f + 0.06f * sens * sens, d = sqrtf(dx * dx + dy * dy);
			const int released = ax == 0.0f && ay == 0.0f; // stick let go: back to the centre at once
			if (released) {
				dx = 0.5f - gx;
				dy = 0.5f - gy;
			} else if (sens < 1.0f && d > cap) {
				dx *= cap / d;
				dy *= cap / d;
			}
			gx += dx;
			gy += dy;
		}
		gx = gx < 0.0f ? 0.0f : gx > 1.0f ? 1.0f : gx;
		gy = gy < 0.0f ? 0.0f : gy > 1.0f ? 1.0f : gy;
		s12_set_gun(0, gx, gy);
		rs12_video_crosshair(0, g_set.crosshair, gx, gy);
		if (b & rs12_key_code(g_set.gun_fire))
			p |= golgo ? S12_IN_B2 : S12_IN_B1; // trigger
		if (golgo && (b & rs12_key_code(g_set.gun_sensor)))
			p |= S12_IN_B7; // scope sensor
	} else if (kind == S12_ANALOG_WHEEL) {
		// d-pad left/right turn the wheel gradually (full lock in 1/4 s), it comes back when released; the stick adds
		static float dsteer;
		if (b & SCE_CTRL_LEFT)
			dsteer = dsteer > -1.0f ? dsteer - 0.07f : -1.0f;
		else if (b & SCE_CTRL_RIGHT)
			dsteer = dsteer < 1.0f ? dsteer + 0.07f : 1.0f;
		else
			dsteer = dsteer > 0.1f ? dsteer - 0.1f : dsteer < -0.1f ? dsteer + 0.1f : 0.0f;
		float steer = stick_axis(raw[ix], cen[ix], 0.08f) + dsteer;
		steer = steer < -1.0f ? -1.0f : steer > 1.0f ? 1.0f : steer;
		s12_set_wheel(steer, (b & rs12_key_code(g_set.gas_key)) ? 1.0f : 0.0f, (b & rs12_key_code(g_set.brake_key)) ? 1.0f : 0.0f);
	} else if (kind == S12_ANALOG_LEVER) {
		// the stick moves the lever and it springs back; d-pad up/down move it and it stays (like a throttle) while
		// the stick is at rest
		static float dlever;
		if (b & SCE_CTRL_UP)
			dlever = dlever < 1.0f ? dlever + 0.03f : 1.0f;
		else if (b & SCE_CTRL_DOWN)
			dlever = dlever > -1.0f ? dlever - 0.03f : -1.0f;
		const float v = -stick_axis(raw[iy], cen[iy], 0.08f);
		s12_set_lever(v != 0.0f ? v : dlever);
	}
	if (kind != S12_ANALOG_GUN)
		rs12_video_crosshair(0, 0, 0, 0);
	return p;
}

// ---- input (ReST-V map_input): d-pad and analog stick (setting) = lever, game buttons 1-6 / coin / start = configured Vita buttons
// (default Square/Triangle/Cross/Circle/R/L, SELECT, START), one turbo key. Buttons held with a system combo (SELECT+START menu,
// SELECT+L / SELECT+R quick save / load) are not passed to the game. 1P and 2P each read their own controller (re_pad) with
// their own port settings (keys, stick, turbo, combo keys: g_set.port). The gun / wheel / lever is 1P's ----
static int sys_combo(uint32_t b)
{
	return (b & SCE_CTRL_SELECT) && (b & (SCE_CTRL_START | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER));
}
static uint32_t map_player(int pl, const SceCtrlData *pad, unsigned frame, uint32_t *sys)
{
	const uint32_t b = pad->buttons;
	// this player's buttons, stick, turbo and combo keys: those of the device it reads (they go with the device)
	const rs12_port *pr = &g_set.port[re_pad_player_profile(pl)];
	const int *km = pr->keymap;
	uint32_t p = 0, s = 0;
	if (b & SCE_CTRL_UP) p |= S12_IN_UP;
	if (b & SCE_CTRL_DOWN) p |= S12_IN_DOWN;
	if (b & SCE_CTRL_LEFT) p |= S12_IN_LEFT;
	if (b & SCE_CTRL_RIGHT) p |= S12_IN_RIGHT;
	// analog stick: tilting more than 48 from center 128 gives that direction (diagonals included)
	// gun / wheel / lever games: the aim stick is not a lever (analog_input below)
	const int aim_left = !pl && analog_stick() == RS12_AIM_LEFT;
	const int aim_right = !pl && analog_stick() == RS12_AIM_RIGHT;
	if ((pr->stick == RS12_STICK_LEFT || pr->stick == RS12_STICK_BOTH) && !aim_left) {
		if (pad->ly < 80) p |= S12_IN_UP;
		if (pad->ly > 176) p |= S12_IN_DOWN;
		if (pad->lx < 80) p |= S12_IN_LEFT;
		if (pad->lx > 176) p |= S12_IN_RIGHT;
	}
	if ((pr->stick == RS12_STICK_RIGHT || pr->stick == RS12_STICK_BOTH) && !aim_right) {
		if (pad->ry < 80) p |= S12_IN_UP;
		if (pad->ry > 176) p |= S12_IN_DOWN;
		if (pad->rx < 80) p |= S12_IN_LEFT;
		if (pad->rx > 176) p |= S12_IN_RIGHT;
	}
	if (rs12_input_rot && re_pad_port(pl) == 0 && !re_pad_is_tv()) {
		// the Vita is held turned (its own controls only, not a controller) (Rotate Vertical L / R): turn the screen directions back into the game's
		// (90: screen right = game up; 270: screen left = game up)
		const int u = (p & S12_IN_UP) != 0, d = (p & S12_IN_DOWN) != 0, l = (p & S12_IN_LEFT) != 0, r = (p & S12_IN_RIGHT) != 0;
		p &= ~(uint32_t)(S12_IN_UP | S12_IN_DOWN | S12_IN_LEFT | S12_IN_RIGHT);
		if (rs12_input_rot == 90)
			p |= (r ? S12_IN_UP : 0) | (l ? S12_IN_DOWN : 0) | (u ? S12_IN_LEFT : 0) | (d ? S12_IN_RIGHT : 0);
		else
			p |= (l ? S12_IN_UP : 0) | (r ? S12_IN_DOWN : 0) | (d ? S12_IN_LEFT : 0) | (u ? S12_IN_RIGHT : 0);
	}
	static const uint32_t s12_btn[RS12_GAME_BUTTONS] = { S12_IN_B1, S12_IN_B2, S12_IN_B3, S12_IN_B4, S12_IN_B5, S12_IN_B6 };
	// turbo: while the turbo key (None = off) is held, the turbo button is pressed for the first half of each period. The turbo key's
	// normal mapping is not used
	const uint32_t tm = rs12_key_code(pr->turbo_key);
	// combo keys: a key with a combo acts as its two keys pressed together (instead of its own mapping)
	uint32_t vb = b, add = 0;
	for (int k = 0; k < RS12_KEY_COUNT; k++)
		if (pr->combo[k] && (b & rs12_key_code(k))) {
			vb &= ~rs12_key_code(k);
			for (int j = 0; j < RS12_KEY_COUNT; j++)
				if (pr->combo[k] & (1 << j))
					add |= rs12_key_code(j);
		}
	const uint32_t nb = (vb | add) & ~tm;
	if (tm && (b & tm) && pr->turbo_btn >= 1 && pr->turbo_btn <= RS12_GAME_BUTTONS) {
		const unsigned per = (unsigned)rs12_turbo_period(pr->turbo_speed);
		if (frame % per < per / 2)
			p |= s12_btn[pr->turbo_btn - 1];
	}
	for (int k = 0; k < rs12_menu_buttons(); k++)
		if (nb & rs12_key_code(km[RS12_ACT_B1 + k]))
			p |= s12_btn[k];
	if (nb & rs12_key_code(km[RS12_ACT_COIN]))
		s |= pl ? S12_SYS_COIN2 : S12_SYS_COIN1;
	if (nb & rs12_key_code(km[RS12_ACT_START]))
		p |= S12_IN_START;
	*sys |= s;
	return p;
}
static void map_input(const SceCtrlData *pad, const SceCtrlData *pad2, unsigned frame)
{
	uint32_t s = 0;
	s12.in.player[0] = sys_combo(pad->buttons) ? 0 : map_player(0, pad, frame, &s) | analog_input(pad);
	s12.in.player[1] = sys_combo(pad2->buttons) ? 0 : map_player(1, pad2, frame, &s);
	if (rs12_dip_service) // test mode from the menu
		s |= S12_SYS_DSW_SERVICE;
	s12.in.system = s;
}

static int file_exists(const char *path)
{
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
}

static void write_text_file(const char *path, const char *text)
{
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0)
		return;
	sceIoWrite(fd, text, strlen(text));
	sceIoClose(fd);
}

static int read_line_file(const char *path, char *buf, int len, int remove_after)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	int ok = fgets(buf, len, f) != NULL;
	fclose(f);
	if (remove_after)
		sceIoRemove(path);
	if (!ok)
		return 0;
	buf[strcspn(buf, "\r\n ")] = 0;
	return buf[0] != 0;
}

// ROM loading fills the loading screen up to 95%; the rest is the board start
static void load_progress(int permille) { rs12_loading_set(permille * 95 / 100); }

// The ROM load (zip inflate, cache build) runs on this thread (CPU1, idle until the game starts); the main thread only
// draws the loading screen, one frame per vblank, until it is done
static struct {
	S12Rom *rom;
	const char *game, *cache_dir;
	char *msg;
	int msg_len;
	int rc;
	volatile int done;
} s_ld;
static int load_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	s_ld.rc = s12_rom_load_cached(s_ld.rom, ROM_DIR, s_ld.cache_dir, s_ld.game, s_ld.msg, s_ld.msg_len, load_progress);
	__atomic_store_n(&s_ld.done, 1, __ATOMIC_RELEASE);
	return 0;
}
static int rom_load_shown(S12Rom *rom, const char *game, const char *cache_dir, char *msg, int msg_len)
{
	s_ld.rom = rom;
	s_ld.game = game;
	s_ld.cache_dir = cache_dir;
	s_ld.msg = msg;
	s_ld.msg_len = msg_len;
	s_ld.done = 0;
	const SceUID th = sceKernelCreateThread("rs12_load", load_thread, 0x60, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) { // (no thread: load here, the screen waits)
		if (th >= 0)
			sceKernelDeleteThread(th);
		load_thread(0, NULL);
		return s_ld.rc;
	}
	while (!__atomic_load_n(&s_ld.done, __ATOMIC_ACQUIRE))
		rs12_loading_frame();
	sceKernelWaitThreadEnd(th, NULL, NULL);
	sceKernelDeleteThread(th);
	return s_ld.rc;
}

// per perf window: CPU time, sound wait (sum, max, us), frames over 16 / 20 ms
static uint64_t s_win_ex, s_win_sw;
static uint32_t s_win_ex_max, s_win_sw_max, s_win_over[2];
extern volatile uint32_t rs12_ndrc_invalidated;

#if RS12_TEXT_PAD
// read-only data at the end of the code segment, only to move its end (vita/CMakeLists.txt RS12_TEXT_PAD)
const volatile unsigned char rs12_text_pad[RS12_TEXT_PAD] = { 1 };
#endif

int main(int argc, char *argv[])
{
#if RS12_TEXT_PAD
	(void)rs12_text_pad[0];
#endif
	int quit_mode = 0; // 1 = exit, 2 = back to the game list (relaunch); the menu Reset resets in place
	char game[64] = "", last[64] = "";
	sceIoMkdir(DATA_DIR, 0777);
	sceIoMkdir(DATA_DIR "/roms", 0777);
	rs12_clock_boost();
	rs12_log_init();
	RS12_LOG("ReSys12 start");
	{
		int restarted = 0, back = 0; // started again by rs12_kctrl_init ("kctrl"); back from a game ("list")
		for (int i = 0; i < argc; i++) {
			restarted |= argv[i] && !strcmp(argv[i], "kctrl");
			back |= argv[i] && !strcmp(argv[i], "list");
		}
		rs12_kctrl_init(restarted);
		// GitHub releases, on its own thread (the game list offers a newer one): only when the app is started, not
		// when it comes back to the list from a game
		if (!back)
			rs12_update_check_start();
	}

	// main thread = emulation: CPU0, high priority
	sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
	sceKernelChangeThreadPriority(0, 0x40);
	s_watch_wake = sceKernelCreateSema("rs12_watch_wake", 0, 0, 1, NULL);
	s_watch_thread = sceKernelCreateThread("rs12_watch", watch_thread, 0xA0, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
	sceKernelStartThread(s_watch_thread, 0, NULL);

	char jit_msg[200];
	rs12_jit_selftest(jit_msg, sizeof(jit_msg));
	RS12_LOG("%s", jit_msg);

	rs12_video_init();
	if (rs12_audio_init() != 0)
		RS12_LOG("audio: init failed");
	re_pad_init();
	rs12_touch_init();
	rs12_settings_load();
	rs12_settings_apply_video();
	// input engine (setting): RePad = the Bluetooth controller plugin from the app, unless it already runs (ur0:tai, or
	// another Re app); Native = whatever the system has
	g_set.input_engine = re_pad_engine_init(g_set.input_engine, "ux0:app/RSYS00012/module/repad.skprx"); // (the kernel cannot open app0:)
	RS12_LOG("input engine: %s, RePad %s", re_pad_engine_name(g_set.input_engine), re_pad_plugin() ? "running" : "not running");

next_game:
	// game: one-shot autostart (measurement tools) or restart (menu Reset) skip the list; else the game list with the cursor on
	// the last game (game.txt), which is only rewritten when the pick changes
	game[0] = 0;
	if (!read_line_file(DATA_DIR "/autostart", game, sizeof(game), 1) &&
		!read_line_file(DATA_DIR "/restart", game, sizeof(game), 1)) {
		read_line_file(DATA_DIR "/game.txt", last, sizeof(last), 0);
		const char *pick = rs12_game_select(last, ROM_DIR, &s_quit);
		if (!pick)
			goto out;
		snprintf(game, sizeof(game), "%s", pick);
		if (strcmp(game, last) != 0)
			write_text_file(DATA_DIR "/game.txt", game);
	}
	RS12_LOG("game: %s", game);
	const int gi = s12_find_game(game);
	const S12Game *ginfo = gi >= 0 ? s12_game(gi) : NULL;
	const char *gtitle = ginfo && ginfo->title ? ginfo->title : game;
	rs12_settings_load_game(game);
	rs12_settings_apply_video(); // (screen rotation: the game's monitor and the Rotate setting)
	rs12_video_set_art(game, ginfo ? ginfo->parent : NULL);
	rs12_menu_init(game, gtitle, rs12_game_buttons(game));

	static S12Rom rom;
	char msg[160];
	uint64_t t0 = rs12_time_us();
	// ROM cache (ux0:data/resys12/cache/<set>.bin, built on first load) unless ux0:data/resys12/nocache exists
	const char *cache_dir = file_exists(DATA_DIR "/nocache") ? NULL : CACHE_DIR;
	if (cache_dir)
		sceIoMkdir(CACHE_DIR, 0777);
	{ // measurements: threads reading the ROM cache from ux0:data/resys12/cachereaders (default 2)
		extern int s12_rom_cache_readers;
		char cr[8];
		s12_rom_cache_readers = read_line_file(DATA_DIR "/cachereaders", cr, sizeof(cr), 0) ? atoi(cr) : 2;
		extern int s12_rom_bg_load; // measurements: ux0:data/resys12/nobgload reads all of the ROM before the start
		s12_rom_bg_load = !file_exists(DATA_DIR "/nobgload");
	}
	rs12_loading_title(gtitle);
	rs12_loading_progress(0);
	const int load_rc = rom_load_shown(&rom, game, cache_dir, msg, sizeof(msg));
	rs12_loading_progress(1000);
	rs12_loading_done();
	if (load_rc != 0) {
		RS12_LOG("rom %s: %s", game, msg);
		rs12_osd_show("ROM load failed: %s - %s  (START: quit)", game, msg); // (stays: no rs12_osd_tick here)
		while (!s_quit) {
			rs12_video_present();
			SceCtrlData pad;
			re_pad_poll();
			re_pad_read_all(&pad);
			if (pad.buttons & SCE_CTRL_START)
				break;
		}
		goto out;
	}
	{
		extern const char *s12_rom_cache_why;
		extern uint32_t s12_rom_stage_ms[3];
		RS12_LOG("rom %s loaded in %llums (%s%s%s) header %u alloc %u start %u ms", game,
			(unsigned long long)((rs12_time_us() - t0) / 1000), msg,
			strcmp(msg, "ok (cache)") && s12_rom_cache_why[0] ? "; cache: " : "", strcmp(msg, "ok (cache)") ? s12_rom_cache_why : "",
			(unsigned)s12_rom_stage_ms[0], (unsigned)s12_rom_stage_ms[1], (unsigned)s12_rom_stage_ms[2]);
	}
	s12_attach(&rom);
	eep_load(game);
	rs12_menu_set_game(game);
	warm_load(game);
	// C352 output at 88.2kHz through the 2:1 decimation filter (ADR-061). ux0:data/resys12/halfrate (measurements):
	// every other sample at 44.1kHz with no filter, as before (the 22-44kHz part of the output folds back)
	{
		extern int rs12_audio_in_half;
		SceIoStat st;
		s12_sound_half_rate = rs12_audio_in_half = sceIoGetstat(DATA_DIR "/halfrate", &st) >= 0 ? 1 : 0;
	}

	memset(&Config, 0, sizeof(Config));
	// ari64 recompiler unless ux0:data/resys12/nodynarec exists (debug comparison)
	{
		SceIoStat st;
		Config.Cpu = sceIoGetstat(DATA_DIR "/nodynarec", &st) >= 0 ? CPU_INTERPRETER : CPU_DYNAREC;
	}
	Config.cycle_multiplier = Config.Cpu == CPU_DYNAREC ? S12_CYCLE_MULTIPLIER_DYNAREC : S12_CYCLE_MULTIPLIER;
	{ // measurements: recompiler options from ux0:data/resys12/ndhacks (NDHACK_* bits, e.g. 0x6)
		FILE *hf = fopen(DATA_DIR "/ndhacks", "r");
		if (hf) {
			unsigned h = 0;
			if (fscanf(hf, "%i", (int *)&h) == 1)
				ndrc_g.hacks = h;
			fclose(hf);
		}
	}
	{ // measurements: the recompiler thread's CPU mask from ux0:data/resys12/drccpu
		extern int rs12_drc_cpu_mask;
		FILE *cf = fopen(DATA_DIR "/drccpu", "r");
		if (cf) {
			if (fscanf(cf, "%i", &rs12_drc_cpu_mask) != 1)
				rs12_drc_cpu_mask = 0;
			fclose(cf);
		}
		extern int rs12_cpu3_usable(void);
		if (rs12_drc_cpu_mask == 0x80000 && !rs12_cpu3_usable())
			rs12_drc_cpu_mask = SCE_KERNEL_CPU_MASK_USER_1; // (stock system: no CPU3 for user threads)
		RS12_LOG("cpu3 %s, dynarec thread cpu mask %x", rs12_cpu3_usable() ? "open" : "closed", rs12_drc_cpu_mask);
	}
	{ // measurements: main CPU cycles between sub CPU slices from ux0:data/resys12/subq
		FILE *qf = fopen(DATA_DIR "/subq", "r");
		if (qf) {
			int q = 0;
			if (fscanf(qf, "%d", &q) == 1 && q >= 500 && q <= 100000)
				s12_sub_quantum = q;
			fclose(qf);
		}
	}
	rs12_video_lowlag = file_exists(DATA_DIR "/lowlag"); // measurements for now: wait for the newest picture
	{
		extern int rs12_video_vsync_pace, rs12_video_pace_forced; // measurements: vblank pacing (Display > Latency)
		rs12_video_pace_forced = file_exists(DATA_DIR "/vsyncpace");
		if (rs12_video_pace_forced) { // "1": always wait for the vblank, "2": adaptive (the menu's Low)
			FILE *vf = fopen(DATA_DIR "/vsyncpace", "r");
			int m = 1;
			if (vf) {
				if (fscanf(vf, "%d", &m) != 1)
					m = 1;
				fclose(vf);
			}
			rs12_video_vsync_pace = m == 2 ? 2 : 1;
		} else
			rs12_video_vsync_pace = g_set.latency == RS12_LATENCY_LOW ? 2 : 0;
	}
	extern int rs12_ndrc_align, rs12_ndrc_scan_off;
	if (file_exists(DATA_DIR "/ndrcalign")) // measurements: blocks do not end at 1KB boundaries
		rs12_ndrc_align = 0;
	rs12_ndrc_scan_off = file_exists(DATA_DIR "/noscan"); // measurements: no function entry scans
	{
		extern int h8_jit_enable, h8_blk_enable; // measurements: H8 blocks without their ARM code / no blocks
		h8_jit_enable = !file_exists(DATA_DIR "/noh8jit");
		gpu_rec_init();
		h8_blk_enable = !file_exists(DATA_DIR "/noh8blk");
	}
	{
		extern int rs12_ndrc_inv_defer; // measurements: invalidations wait for the compiler thread as upstream
		rs12_ndrc_inv_defer = !file_exists(DATA_DIR "/noinvdefer");
		extern int rs12_ndrc_keep_same; // measurements: overwritten blocks are invalidated even when unchanged
		rs12_ndrc_keep_same = !file_exists(DATA_DIR "/nokeepsame");
	}
	RS12_LOG("cpu: %s hacks %x subq %d align %d", Config.Cpu == CPU_DYNAREC ? "ari64 dynarec" : "interpreter",
		ndrc_g.hacks, s12_sub_quantum, rs12_ndrc_align);

	pl_rearmed_cbs.pl_vout_open = vout_open;
	pl_rearmed_cbs.pl_vout_close = vout_close;
	pl_rearmed_cbs.pl_vout_set_mode = vout_set_mode;
	pl_rearmed_cbs.pl_vout_flip = vout_flip;
	pl_rearmed_cbs.gpu_neon.allow_interlace = 2;
	s_il_mode = 2;
	// GPU commands rendered on CPU2 (gpulib gpu_async) unless ux0:data/resys12/noasyncgpu exists
	{
		SceIoStat st;
		pl_rearmed_cbs.thread_rendering = sceIoGetstat(DATA_DIR "/noasyncgpu", &st) >= 0 ? 0 : 1;
		RS12_LOG("gpu: %s", pl_rearmed_cbs.thread_rendering ? "threaded (CPU2)" : "on the emulation thread");
		// ReSys12: split rendering (helper renderers on CPU1 and CPU3) unless ux0:data/resys12/nogpusplit exists
		// (ux0:data/resys12/gpusplitseq: both halves on the GPU thread, for measurements)
		int split = sceIoGetstat(DATA_DIR "/nogpusplit", &st) >= 0 ? 0 : 1;
		if (split && sceIoGetstat(DATA_DIR "/gpusplitseq", &st) >= 0)
			split = 2;
		// three bands (helpers on CPU1 and CPU3) unless CPU3 is closed or ux0:data/resys12/gpusplit2 exists
		extern void rs12_gpu_split_set_count(int n);
		extern int rs12_cpu3_usable(void);
		const int bands = file_exists(DATA_DIR "/gpusplit2") || !rs12_cpu3_usable() ? 2 : 3;
		rs12_gpu_split_set_count(bands);
		rs12_gpu_split_enable(split);
		RS12_LOG("gpu split: %s", split == 1 ? (bands == 2 ? "2 bands (helper on CPU1)" :
			"3 bands (helpers on CPU1, CPU3)") : split ? "on (sequential)" : "off");
	}

	rs12_psx_bind();
	s_gpu_dma_chain = GPU_dmaChain;
	s_gpu_write_mem = GPU_writeDataMem;
	GPU_dmaChain = gpu_dma_chain_timed;
	GPU_writeDataMem = gpu_write_mem_timed;
	if (psxInit() != 0) {
		RS12_LOG("psxInit failed");
		goto out;
	}
	s_emu_up = 1;
	s_hb_t = rs12_time_us();
	s_hangdump = file_exists(DATA_DIR "/hangdump");
	rs12_GPUrearmedCallbacks(&pl_rearmed_cbs);
	GPU_init();
	// ReSys12: GPU_init clears the GPU state (allow_interlace, frame counter...): hand the settings over again
	rs12_GPUrearmedCallbacks(&pl_rearmed_cbs);
	GPU_open(NULL, NULL, NULL);
	psxReset();

	{ // test recording of the sound output (see sound_thread)
		FILE *rf = fopen(DATA_DIR "/audiorec", "r");
		int secs = 0;
		if (rf) {
			if (fscanf(rf, "%d", &secs) != 1)
				secs = 0;
			fclose(rf);
			sceIoRemove(DATA_DIR "/audiorec");
		}
		if (secs > 0 && secs <= 120) {
			s_rec_cap = (uint32_t)secs * (s12_sound_half_rate ? 44100u : 88200u);
			s_rec = (int16_t *)malloc((size_t)s_rec_cap * 4);
			RS12_LOG("audiorec: %d s %s", secs, s_rec ? "on" : "no memory");
		}
	}
	s_snd_go = sceKernelCreateSema("rs12_snd_go", 0, 0, 1, NULL);
	s_snd_done = sceKernelCreateSema("rs12_snd_done", 0, 0, 1, NULL);
	// free to run on CPU0 while the emulation (0x40) waits for the renderer, so the split helper on CPU1 is held back
	// less by the C352 work
	s_snd_thread = sceKernelCreateThread("rs12_sound", sound_thread, 0x48, 0x10000, 0,
		SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1, NULL);
	sceKernelStartThread(s_snd_thread, 0, NULL);
#ifdef RS12_SUB_PROFILE
	{ // measurement builds only: the sampler wakes 4000 times a second on CPU1, above the split render helper
		SceIoStat st;
		if (sceIoGetstat(DATA_DIR "/sampler", &st) >= 0) {
			SceUID smp = sceKernelCreateThread("rs12_sampler", gpu_sampler_thread, 0x40, 0x4000, 0,
				SCE_KERNEL_CPU_MASK_USER_1, NULL);
			sceKernelStartThread(smp, 0, NULL);
			h8_jit_prof = 1;
			RS12_LOG("sampler: on");
		}
	}
#endif
	uint64_t win_t0 = rs12_time_us();
	uint32_t frames = 0, win_n = 0, max_emu = 0;
	uint64_t sum_emu = 0, sum_present = 0;
	float osd_fps = 0;
	uint32_t prev_buttons = ~0u;
	while (!s_quit) {
		// 1P and 2P controllers (re_pad); system combos (menu, quick save / load, quit) from any controller
		SceCtrlData pad, pad2, all;
		re_pad_poll();
		re_pad_read(0, &pad);
		re_pad_read(1, &pad2);
		re_pad_read_all(&all);
		if (g_set.touch_keys) { // touch buttons press their Vita keys (in gun games too: those touches do not aim)
			const uint32_t tb = rs12_touch_buttons();
			pad.buttons |= tb;
			all.buttons |= tb;
		}
		if (re_pad_changed()) {
			char t1[48], t2[48];
			rs12_osd_show("1P: %s   2P: %s", re_pad_player_text(0, t1, sizeof(t1)), re_pad_player_text(1, t2, sizeof(t2)));
		}
		const uint32_t quit_keys = SCE_CTRL_START | SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
		if ((all.buttons & quit_keys) == quit_keys)
			break;
		// test (ux0:data/resys12/menutest): menu pages, notice, FPS box to PPM files at frame 600
		if (frames == 600 && file_exists(DATA_DIR "/menutest")) {
			sceIoRemove(DATA_DIR "/menutest");
			rs12_menu_selftest();
			RS12_LOG("menu: selftest images written");
		}
		// screenshots (ux0:data/resys12/shot.txt = frame numbers): screen as presented -> shot_f<N>.ppm.
		// The list is read once at the game's start (no card I/O per frame); the file is deleted after the last shot.
		// These one-shot test files are read at frame 0 of each game (games switch inside the app)
		{
			static int shot_n, shot_i;
			static unsigned shot_frames[16];
			if (frames == 0) {
				shot_n = shot_i = 0;
				FILE *sf = fopen(DATA_DIR "/shot.txt", "r");
				if (sf) {
					while (shot_n < 16 && fscanf(sf, "%u", &shot_frames[shot_n]) == 1)
						shot_n++;
					fclose(sf);
				}
			}
			if (shot_i < shot_n && frames == shot_frames[shot_i]) {
				char sp[96];
				snprintf(sp, sizeof(sp), DATA_DIR "/shot_f%u.ppm", frames);
				RS12_LOG("shot %s: %d", sp, rs12_video_capture_ppm(sp));
				if (++shot_i == shot_n)
					sceIoRemove(DATA_DIR "/shot.txt");
			}
		}
		// save state test (ux0:data/resys12/statetest = "S L": save slot 9 after frame S, load it after frame L; read once
		// at startup and deleted, like shot.txt)
		{
			static unsigned st_save, st_load;
			if (frames == 0) {
				st_save = st_load = 0;
				FILE *tf = fopen(DATA_DIR "/statetest", "r");
				if (tf) {
					if (fscanf(tf, "%u %u", &st_save, &st_load) != 2)
						st_save = st_load = 0;
					fclose(tf);
					sceIoRemove(DATA_DIR "/statetest");
				}
			}
			// reset test (ux0:data/resys12/resettest = frame, read once with statetest's start): the menu Reset
			static unsigned rt_frame;
			if (frames == 0) {
				rt_frame = 0;
				FILE *rf = fopen(DATA_DIR "/resettest", "r");
				if (rf) {
					if (fscanf(rf, "%u", &rt_frame) != 1)
						rt_frame = 0;
					fclose(rf);
					sceIoRemove(DATA_DIR "/resettest");
				}
			}
			if (rt_frame && frames == rt_frame) {
				sound_idle();
				GPU_writeStatus(0);
				psxReset();
				RS12_LOG("reset f%u (test)", frames);
			}
			if (st_save && frames == st_save)
				state_save(game, 9);
			if (st_load && frames == st_load)
				state_load(game, 9);
		}
		// measurements: ux0:data/resys12/kprof = "F S" (one-shot): from frame F, S seconds of PC / LR samples of this
		// thread taken by the kernel helper -> kprof.bin (header: count, main(), JIT region base and size, then pairs)
		{
			static unsigned kp_from, kp_to;
			static uint32_t *kp;
			static int kp_n;
			enum { KP_CAP = 256 << 10 }; // words: 20 s of 500 us samples is ~80k
			if (frames == 0) {
				kp_from = kp_to = 0;
				FILE *kf = fopen(DATA_DIR "/kprof", "r");
				unsigned s = 0;
				if (kf) {
					if (fscanf(kf, "%u %u", &kp_from, &s) != 2)
						kp_from = s = 0;
					fclose(kf);
					sceIoRemove(DATA_DIR "/kprof");
				}
				kp_to = s ? kp_from + s * 60 : 0;
				if (kp_to && !(kp = (uint32_t *)malloc(KP_CAP * 4)))
					RS12_LOG("kprof: no memory");
			}
			if (kp && !rs12_kctrl_ready()) { // (no kernel helper this run)
				RS12_LOG("kprof: kernel helper not loaded");
				free(kp);
				kp = NULL;
			}
			if (kp && frames == kp_from)
				RS12_LOG("kprof: start %d", rs12KCtrlProfStart(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0));
			if (kp && frames > kp_from && frames <= kp_to && (frames % 8 == 0 || frames == kp_to)) {
				int err = 0;
				const int got = rs12KCtrlProfRead(kp + 5 + kp_n, KP_CAP - 5 - kp_n, &err);
				kp_n += got > 0 ? got : 0;
				if (frames == kp_to) {
					rs12KCtrlProfStart(-1, 0);
					size_t jsz = 0;
					kp[0] = (uint32_t)kp_n;
					kp[1] = (uint32_t)(uintptr_t)main;
					kp[2] = (uint32_t)(uintptr_t)rs12_jit_code_region(&jsz);
					kp[3] = (uint32_t)jsz;
					kp[4] = 0;
					FILE *of = fopen(DATA_DIR "/kprof.bin", "wb");
					if (of) {
						fwrite(kp, 4, (size_t)kp_n + 5, of);
						fclose(of);
					}
					RS12_LOG("kprof: %d samples, last error %08x, main %08x jit %08x+%x", kp_n / 2, (unsigned)err, kp[1], kp[2], kp[3]);
					free(kp);
					kp = NULL;
				}
			}
		}
		// measurement scenes: the one-shot ux0:data/resys12/benchload loads state/bench/<set>.st after the first frame
		// (scenes saved by the PC harness, vita/tools/run_game.ps1 -Bench), so a run measures gameplay, not the demo.
		// A slot number in the file loads that save slot instead (scenes saved while playing, -Slot N)
		if (frames == 1) {
			SceIoStat bst;
			if (sceIoGetstat(DATA_DIR "/benchload", &bst) >= 0) {
				int slot = -1;
				FILE *bf = fopen(DATA_DIR "/benchload", "r");
				if (bf) {
					if (fscanf(bf, "%d", &slot) != 1)
						slot = -1;
					fclose(bf);
				}
				sceIoRemove(DATA_DIR "/benchload");
				char bp[128];
				if (slot >= 0)
					snprintf(bp, sizeof(bp), STATE_DIR "/%s_%d.st", game, slot);
				else
					snprintf(bp, sizeof(bp), STATE_DIR "/bench/%s.st", game);
				RS12_LOG("bench: load %s %s", bp, state_load_file(bp) ? "ok" : "failed");
			}
		}
		if (rs12_menu_should_open(all.buttons)) {
			// in-game menu (SELECT+START): emulation stops while it is open; drawn over the last frame
			RS12_LOG("menu: open f%u", frames);
			rs12_fps_hide(); // (it covered the menu, e.g. Done of the touch layout editor)
			rs12_touch_overlay_update(1);
			for (int done = 0; !done;) {
				re_pad_poll();
				re_pad_read_all(&all);
				switch (rs12_menu_update(all.buttons)) {
				case RS12_MENU_CLOSE: done = 1; break;
				case RS12_MENU_RESET:
					// the board is reset in place (CPU, RAM, hardware, System 12 board, H8, C352, GPU): the ROMs stay
					// loaded and the EEPROM keeps its settings, as on the cabinet
					sound_idle();
					GPU_writeStatus(0); // GP1(00h): GPU reset
					psxReset();
					RS12_LOG("reset f%u", frames);
					done = 1;
					break;
				case RS12_MENU_SAVE: // the slot shows as used once the file is written
					state_save(game, rs12_menu_slot());
					state_wait();
					rs12_menu_states_changed();
					break;
				case RS12_MENU_LOAD: state_load(game, rs12_menu_slot()); done = 1; break;
				case RS12_MENU_SELECT_GAME: quit_mode = 2; done = 1; break;
				case RS12_MENU_EXIT: quit_mode = 1; done = 1; break;
				default: break;
				}
				if (s_quit) {
					quit_mode = 1;
					done = 1;
				}
				if (!done)
					rs12_menu_draw();
				rs12_osd_tick();
				rs12_video_present();
				s_hb_t = rs12_time_us(); // (the menu is not a hang)
			}
			rs12_menu_hide();
			rs12_video_present();
			RS12_LOG("menu: close (quit %d)", quit_mode);
			prev_buttons = all.buttons;
			win_t0 = rs12_time_us();
			win_n = 0;
			sum_emu = sum_present = 0;
			max_emu = 0;
			if (quit_mode)
				break;
			continue;
		}
		// quick save / load combos (SELECT+L save / SELECT+R load, current slot)
		{
			const uint32_t edge = all.buttons & ~prev_buttons;
			if ((all.buttons & SCE_CTRL_SELECT) && (edge & SCE_CTRL_LTRIGGER))
				state_save(game, rs12_menu_slot());
			else if ((all.buttons & SCE_CTRL_SELECT) && (edge & SCE_CTRL_RTRIGGER))
				state_load(game, rs12_menu_slot());
			prev_buttons = all.buttons;
		}
		map_input(&pad, &pad2, frames);
		// test input script (ux0:data/resys12/input.txt, "F:P1:SYS,..." hex masks as in the PC harness RS12_INPUT, each
		// held from frame F on, added to the pad). Read once at startup and deleted (one-shot, like shot.txt)
		{
			static int in_n, in_i;
			static unsigned in_f[64], in_p1[64], in_sys[64], cur_p1, cur_sys;
			if (frames == 0) {
				in_n = in_i = 0;
				cur_p1 = cur_sys = 0;
				FILE *inf = fopen(DATA_DIR "/input.txt", "r");
				if (inf) {
					while (in_n < 64 && fscanf(inf, " %u:%x:%x ,", &in_f[in_n], &in_p1[in_n], &in_sys[in_n]) == 3)
						in_n++;
					fclose(inf);
					sceIoRemove(DATA_DIR "/input.txt");
					RS12_LOG("input script: %d entries", in_n);
				}
			}
			while (in_i < in_n && frames >= in_f[in_i]) {
				cur_p1 = in_p1[in_i];
				cur_sys = in_sys[in_i];
				in_i++;
			}
			s12.in.player[0] |= cur_p1;
			s12.in.system |= cur_sys;
			// an entry with P1 mask 0xfffff mashes buttons from then on, as the PC harness RS12_INPUT_MASH (long
			// gameplay runs on a measurement scene)
			static uint32_t mash_rng = 12345, mash_p1;
			if (cur_p1 == 0xfffff) {
				s12.in.player[0] &= ~0xfffffu;
				if (frames % 6 == 0) {
					mash_rng = mash_rng * 1103515245u + 12345u;
					mash_p1 = (mash_rng >> 8) & 0x3ff & ~(uint32_t)0x3; // buttons 1-6, left/right; no up/down
				}
				s12.in.player[0] |= (frames % 6) < 4 ? mash_p1 : 0;
			}
		}

		const uint64_t a = rs12_time_us();
		rs12_emu_frame = frames; // (tags the flips of this frame: display latency log)
		psxRegs.stop = 0;
		psxCpu->Execute(&psxRegs);
		const uint64_t a2 = rs12_time_us();
		sound_frame_end();
		const uint64_t a3 = rs12_time_us();
		eep_frame();
		warm_frame(frames);
		gpu_rec_frame(frames);
		const uint64_t b = rs12_time_us();
		{ // where long frames come from: the CPU part and the wait for the sound thread, frames over the budget
			const uint32_t ex = (uint32_t)(a2 - a), sw = (uint32_t)(a3 - a2), tot = (uint32_t)(b - a);
			s_win_ex += ex;
			s_win_sw += sw;
			if (ex > s_win_ex_max) s_win_ex_max = ex;
			if (sw > s_win_sw_max) s_win_sw_max = sw;
			s_win_over[0] += tot > 16000;
			s_win_over[1] += tot > 20000;
		}
		if (rs12_video_lowlag) {
			// low latency: wait for the picture this frame asked for (its flip is at the frame's end, the renderer
			// finishes it a few ms later), until 2.5 ms before the vblank; meanwhile this thread renders a pending
			// split band. Late: shown in order as before
			extern int rs12_video_newest_tag(void);
			extern int rs12_split_worker_help(void);
			static uint64_t last_present;
			const uint64_t deadline = last_present + 16683 - 2500;
			while (rs12_video_newest_tag() != (int)(uint8_t)frames && rs12_time_us() < deadline)
				if (!rs12_split_worker_help())
					sceKernelDelayThread(50);
			rs12_video_present();
			last_present = rs12_time_us();
		} else
			rs12_video_present();
		const uint64_t c = rs12_time_us();

		const uint32_t emu = (uint32_t)(b - a);
		// stall log: a frame over 40ms, with the recompiler's and the GPU front end's share of it
		{
			static uint64_t last_cus, last_gus, last_sus;
			static uint32_t last_blk, last_syn;
			const uint64_t sus = rs12_ndrc_sync_us;
			const uint32_t syn = rs12_ndrc_syncs;
			const uint64_t cus = rs12_ndrc_compile_us, gus = s_gpu_us < last_gus ? 0 : last_gus;
			const uint32_t blk = rs12_ndrc_blocks;
			static uint64_t last_pass[10], last_cpu;
			static uint32_t last_ins;
			if (emu > 40000) {
				char pb[96];
				int o = 0;
				for (int k = 1; k < 10; k++)
					o += snprintf(pb + o, sizeof(pb) - o, " %.1f", (rs12_ndrc_pass_us[k] - last_pass[k]) / 1000.0);
				extern volatile uint32_t rs12_ndrc_invalidated, rs12_ndrc_warm_done;
				RS12_LOG("stall f%u passes(ms)%s insns %u cpu %.1fms inv %u warm %u", frames, pb,
					rs12_ndrc_insns - last_ins, (rs12_ndrc_cpu_us - last_cpu) / 1000.0, rs12_ndrc_invalidated,
					rs12_ndrc_warm_done);
			}
#ifdef RS12_SUB_PROFILE
			if (frames % 1200 == 0 && rs12_ndrc_insns)
				RS12_LOG("ndrc total f%u: %u insns, %u blocks, cpu %.0fms (%.2fus/insn), wall %.0fms", frames,
					rs12_ndrc_insns, blk, rs12_ndrc_cpu_us / 1000.0, (double)rs12_ndrc_cpu_us / rs12_ndrc_insns,
					cus / 1000.0);
#endif
			last_ins = rs12_ndrc_insns;
			last_cpu = rs12_ndrc_cpu_us;
			for (int k = 1; k < 10; k++)
				last_pass[k] = rs12_ndrc_pass_us[k];
			extern volatile uint64_t rs12_ndrc_tsync_us, rs12_ndrc_inv_us;
			static uint64_t last_tsync, last_inv;
			if (emu > 40000)
				RS12_LOG("stall f%u emu %.1fms: compile %.1fms (%u blocks; cache sync %.1fms in %u calls) gpu %.1fms "
					"thread wait %.1fms invalidate %.1fms", frames,
					emu / 1000.0, (cus - last_cus) / 1000.0, blk - last_blk, (sus - last_sus) / 1000.0, syn - last_syn,
					(s_gpu_us - gus) / 1000.0, (rs12_ndrc_tsync_us - last_tsync) / 1000.0,
					(rs12_ndrc_inv_us - last_inv) / 1000.0);
			last_tsync = rs12_ndrc_tsync_us;
			last_inv = rs12_ndrc_inv_us;
			last_cus = cus;
			last_blk = blk;
			last_sus = sus;
			last_syn = syn;
			last_gus = s_gpu_us;
		}
		sum_emu += emu;
		sum_present += c - b;
		if (emu > max_emu)
			max_emu = emu;
		frames++;
		s_hb_t = rs12_time_us();
		if ((frames & 15) == 0) { // field rendering on within a quarter second (a fight start), off by the 1 s windows below
			extern int rs12_gpu_interlaced(void);
			extern void rs12_gpu_set_allow_interlace(int mode);
			static uint64_t q_busy;
			const uint64_t gb = rs12_gpu_busy_us;
			const double gthr_ms = (gb - q_busy) / 16.0 / 1000.0;
			if (q_busy && s_il_mode == 2 && rs12_gpu_interlaced() && gthr_ms > 14.0) {
				s_il_mode = 1;
				rs12_gpu_set_allow_interlace(1);
				RS12_LOG("gpu: field rendering on (renderer %.1fms over 16 frames)", gthr_ms);
			}
			q_busy = gb;
		}
		if (++win_n == 60) {
#ifdef RS12_SUB_PROFILE
			{ // H8 events per frame
				static uint32_t last_h8p[H8P_N];
				extern uint64_t h8_prof_ns;
				static uint64_t last_h8ns;
				extern uint32_t h8_jit_blocks, h8_jit_bytes, h8_jit_native_n, h8_jit_call_n;
				RS12_LOG("h8 periph update %.3f ms/frame, jit %u blocks %u bytes, made %u called %u", (h8_prof_ns - last_h8ns) / 60e6,
					h8_jit_blocks, h8_jit_bytes, h8_jit_native_n, h8_jit_call_n);
				last_h8ns = h8_prof_ns;
				RS12_LOG("h8 per frame: insn %.0f update %.1f idle %.1f extR %.1f extW %.1f chipR %.1f chipW %.1f run %.1f",
					(h8_prof[0] - last_h8p[0]) / 60.0, (h8_prof[1] - last_h8p[1]) / 60.0, (h8_prof[2] - last_h8p[2]) / 60.0,
					(h8_prof[3] - last_h8p[3]) / 60.0, (h8_prof[4] - last_h8p[4]) / 60.0, (h8_prof[5] - last_h8p[5]) / 60.0,
					(h8_prof[6] - last_h8p[6]) / 60.0, (h8_prof[7] - last_h8p[7]) / 60.0);
				memcpy(last_h8p, h8_prof, sizeof(last_h8p));
				static uint64_t last_states, last_idle;
				const uint64_t states = h8_total_cycles(), idle = h8.idle_cycles;
				RS12_LOG("h8 states per frame %.0f, skipped idle %.0f", (states - last_states) / 60.0, (idle - last_idle) / 60.0);
				last_states = states;
				last_idle = idle;
				extern uint32_t h8_prof_chip[0x300][2];
				if (frames % 600 == 0) {
					char cb[240];
					int o = 0;
					for (int k = 0; k < 10; k++) {
						int bi = -1;
						uint32_t best = 0;
						for (int j = 0; j < 0x600; j++)
							if (h8_prof_chip[j >> 1][j & 1] > best)
								best = h8_prof_chip[j >> 1][j & 1], bi = j;
						if (bi < 0)
							break;
						o += snprintf(cb + o, sizeof(cb) - o, " %c%06x:%u", (bi & 1) ? 'w' : 'r', 0xfffd10 + (bi >> 1), best);
						h8_prof_chip[bi >> 1][bi & 1] = 0;
					}
					memset(h8_prof_chip, 0, sizeof(h8_prof_chip));
					RS12_LOG("h8 on-chip top (10 s):%s", cb);
				}
			}
#endif
			const double fps = 60.0 * 1e6 / (double)(c - win_t0);
			static uint32_t last_flips;
			const double gfps = (s_flips - last_flips) * 1e6 / (double)(c - win_t0);
			last_flips = s_flips;
			static uint64_t last_sub_ns;
			const uint64_t sub_ns = s12_sub_profile_ns();
			rs12_audio_stats as;
			rs12_audio_get_stats(&as);
			static uint64_t last_gbusy, last_gwait, last_gbusy_b, last_gbusy_c;
			extern volatile uint64_t rs12_gpu_split_busy_c_us;
			const uint64_t gbusy_c = rs12_gpu_split_busy_c_us;
			static uint32_t last_chunks, last_stolen, last_barriers;
			const uint64_t gbusy = rs12_gpu_busy_us, gwait = rs12_gpu_wait_us;
			// ReSys12: split rendering helper (CPU1): busy ms/frame, chunks/stolen/barriers per frame, band ratio
			static uint64_t last_a, last_join;
			const uint64_t gbusy_b = rs12_gpu_split_busy_us, sa = rs12_gpu_split_a_us, sj = rs12_gpu_split_join_us;
			const uint32_t chunks = rs12_gpu_split_chunks, stolen = rs12_gpu_split_stolen;
			const uint32_t barriers = rs12_gpu_split_barriers;
			{
				extern volatile uint64_t rs12_gpu_wait_r[4];
				static uint64_t last_r[4];
				if (rs12_gpu_wait_r[0] + rs12_gpu_wait_r[1] + rs12_gpu_wait_r[2] + rs12_gpu_wait_r[3] !=
					last_r[0] + last_r[1] + last_r[2] + last_r[3])
				RS12_LOG("gwait by reason (ms/frame): queue %.2f sync %.2f scanout %.2f scanout_full %.2f",
					(rs12_gpu_wait_r[0] - last_r[0]) / 60e3, (rs12_gpu_wait_r[1] - last_r[1]) / 60e3,
					(rs12_gpu_wait_r[2] - last_r[2]) / 60e3, (rs12_gpu_wait_r[3] - last_r[3]) / 60e3);
				for (int k = 0; k < 4; k++)
					last_r[k] = rs12_gpu_wait_r[k];
			}
			{
				extern volatile uintptr_t rs12_sync_ra[8];
				extern volatile uint32_t rs12_sync_ra_us[8];
				char sb[200];
				int o = 0, any = 0;
				for (int k = 0; k < 8 && rs12_sync_ra[k]; k++) {
					o += snprintf(sb + o, sizeof(sb) - o, " %08x:%.1f", (unsigned)rs12_sync_ra[k], rs12_sync_ra_us[k] / 60e3);
					any |= rs12_sync_ra_us[k] >= 600; // 0.01ms a frame
					rs12_sync_ra_us[k] = 0;
				}
				if (any) // full renderer syncs that waited, by caller (addr2line after subtracting the vout_flip offset)
					RS12_LOG("gsync callers (ms/frame):%s (vout_flip at %08x)", sb, (unsigned)(uintptr_t)vout_flip);
				extern volatile int rs12_rw_max[11];
				if (rs12_rw_max[0] >= 1000) // the longest wait for a VRAM read rectangle: what it hit, how far into the queue
					RS12_LOG("rwait max %.1fms read %d,%d %dx%d hit area #%d %d,%d-%d,%d at %d/1000 of the queue",
						rs12_rw_max[0] / 1000.0, rs12_rw_max[1], rs12_rw_max[2], rs12_rw_max[3], rs12_rw_max[4], rs12_rw_max[9],
						rs12_rw_max[5], rs12_rw_max[6], rs12_rw_max[7], rs12_rw_max[8], rs12_rw_max[10]);
				rs12_rw_max[0] = 0;
				extern volatile uint32_t rs12_fwd_n, rs12_fwd_bad;
				static uint32_t last_fwd;
				if (rs12_fwd_n != last_fwd)
					RS12_LOG("vram read forward %u (+%u), differed %u", rs12_fwd_n, rs12_fwd_n - last_fwd, rs12_fwd_bad);
				last_fwd = rs12_fwd_n;
				extern volatile int rs12_rw_full[6];
				static int last_full;
				if (rs12_rw_full[0] != last_full)
					RS12_LOG("vram read full syncs +%d, last %d,%d %dx%d with %d words queued", rs12_rw_full[0] - last_full,
						rs12_rw_full[1], rs12_rw_full[2], rs12_rw_full[3], rs12_rw_full[4], rs12_rw_full[5]);
				last_full = rs12_rw_full[0];
			}
			{ // display latency: emulated frames from the one that asked for a picture to the one run before it is shown
				extern volatile uint32_t rs12_video_lag_sum, rs12_video_lag_n, rs12_video_lag_max;
				if (rs12_video_lag_n)
					RS12_LOG("lag f%u: picture shown %.2f frames after its frame (max %u)", frames,
						(double)rs12_video_lag_sum / rs12_video_lag_n, (unsigned)rs12_video_lag_max);
				rs12_video_lag_sum = rs12_video_lag_n = rs12_video_lag_max = 0;
				extern volatile uint32_t rs12_video_disp_sum, rs12_video_disp_n, rs12_video_disp_max;
				extern volatile uint32_t rs12_video_pace_falls, rs12_video_pace_resyncs, rs12_video_pace_low_frames;
				if (rs12_video_disp_n)
					RS12_LOG("disp f%u: swap to screen %.1f ms (max %.1f) pace low %u falls %u resyncs %u", frames,
						rs12_video_disp_sum / 1000.0 / rs12_video_disp_n, rs12_video_disp_max / 1000.0,
						rs12_video_pace_low_frames, rs12_video_pace_falls, rs12_video_pace_resyncs);
				rs12_video_disp_sum = rs12_video_disp_n = rs12_video_disp_max = 0;
			}
			{ // H8 block code found overwritten (h8.c): how often, to find what overwrites it
				extern uint32_t h8_jit_bad_n;
				static uint32_t last_bad;
				if (h8_jit_bad_n != last_bad) {
					RS12_LOG("h8 jit: %u blocks found with overwritten code (f%u)", h8_jit_bad_n, frames);
					last_bad = h8_jit_bad_n;
				}
			}
			RS12_LOG("f%u fps %.2f gfps %.2f emu %.2f/%.2f (h8 %.2f gpu %.2f gwait %.2f) gthr %.2f gthrB %.2f gthrC %.2f snd %.2f present %.2f flipcp %.2f late %u fmax %.1f rep %u skip %u qd %.2f aud fill %d ppm %d und %u ovf %u"
				" split %s A %.2f join %.2f ch %.0f st %.0f bar %.0f r %u mm %u epc %08x h8 %06x",
				frames, fps, gfps, sum_emu / 60.0 / 1000.0, max_emu / 1000.0, (sub_ns - last_sub_ns) / 60.0 / 1e6,
				s_gpu_us / 60.0 / 1000.0, (gwait - last_gwait) / 60.0 / 1000.0, (gbusy - last_gbusy) / 60.0 / 1000.0,
				(gbusy_b - last_gbusy_b) / 60.0 / 1000.0, (gbusy_c - last_gbusy_c) / 60.0 / 1000.0,
				s_snd_us / 1000.0, sum_present / 60.0 / 1000.0, s_flip_us / 60.0 / 1000.0, s_flip_late, s_flip_max / 1000.0, rs12_video_repeats, rs12_video_skips, rs12_video_qdepth / 60.0, as.fill, as.drc_ppm, as.underruns, as.overflows,
				rs12_gpu_split_active() ? "on" : "off", (sa - last_a) / 60.0 / 1000.0, (sj - last_join) / 60.0 / 1000.0,
				(chunks - last_chunks) / 60.0, (stolen - last_stolen) / 60.0,
				(barriers - last_barriers) / 60.0, (unsigned)rs12_gpu_split_ratio, (unsigned)rs12_gpu_split_mismatch,
				psxRegs.CP0.n.EPC, (unsigned)h8.PPC); // EPC: where the game was when the frame interrupt hit
			{ // field rendering for interlaced games while the renderer is overloaded
				extern int rs12_gpu_interlaced(void);
				extern void rs12_gpu_set_allow_interlace(int mode);
				int il_mode = s_il_mode;
				const double gthr_ms = (gbusy - last_gbusy) / 60.0 / 1000.0;
				if (il_mode == 2 && rs12_gpu_interlaced() && gthr_ms > 14.0) {
					il_mode = 1;
					rs12_gpu_set_allow_interlace(1);
					RS12_LOG("gpu: field rendering on (renderer %.1fms)", gthr_ms);
				} else if (il_mode == 1 && (gthr_ms < 7.0 || !rs12_gpu_interlaced())) {
					il_mode = 2;
					rs12_gpu_set_allow_interlace(2);
					RS12_LOG("gpu: field rendering auto (renderer %.1fms)", gthr_ms);
				}
				s_il_mode = il_mode;
			}
			last_gbusy = gbusy;
			last_gwait = gwait;
			s_flip_us = 0;
			s_flip_late = s_flip_max = 0;
			rs12_video_repeats = rs12_video_skips = 0;
			rs12_video_qdepth = 0;
			last_gbusy_b = gbusy_b;
			last_gbusy_c = gbusy_c;
			last_a = sa;
			last_join = sj;
			last_chunks = chunks;
			last_stolen = stolen;
			last_barriers = barriers;
			last_sub_ns = sub_ns;
			s_gpu_us = 0;
			osd_fps = (float)gfps; // what the player sees
			RS12_LOG("split f%u exec %.2f/%.1f sndwait %.2f/%.1f over16 %u over20 %u inv %u", frames, s_win_ex / 60e3, s_win_ex_max / 1e3,
				s_win_sw / 60e3, s_win_sw_max / 1e3, s_win_over[0], s_win_over[1], rs12_ndrc_invalidated);
			{ // events per frame by type (each one leaves the recompiled code)
				extern volatile uint32_t rs12_irq_count[];
				static uint32_t last[32];
				char eb[200];
				int o = 0;
				for (int k = 0; k <= PSXINT_COUNT && o < (int)sizeof(eb) - 16; k++) {
					const uint32_t d = rs12_irq_count[k] - last[k];
					last[k] = rs12_irq_count[k];
					if (d)
						o += snprintf(eb + o, sizeof(eb) - o, " %d:%.1f", k, d / 60.0);
				}
				extern volatile uint32_t rs12_mtc0_count[16];
				static uint32_t last_m[16];
				o += snprintf(eb + o, sizeof(eb) - o, "  mtc0");
				for (int k = 0; k < 16 && o < (int)sizeof(eb) - 16; k++) {
					const uint32_t d = rs12_mtc0_count[k] - last_m[k];
					last_m[k] = rs12_mtc0_count[k];
					if (d)
						o += snprintf(eb + o, sizeof(eb) - o, " r%d:%.1f", k, d / 60.0);
				}
				RS12_LOG("events/frame%s (%d = checks)", eb, PSXINT_COUNT);
				extern volatile uint32_t rs12_empty_pc[64][3];
				o = 0;
				for (int n = 0; n < 4; n++) { // the 4 pcs with the most checks that ran nothing (counts reset each window)
					int bi = -1;
					uint32_t bc = 0;
					for (int k = 0; k < 64; k++)
						if (rs12_empty_pc[k][1] > bc) {
							bc = rs12_empty_pc[k][1];
							bi = k;
						}
					if (bi < 0)
						break;
					o += snprintf(eb + o, sizeof(eb) - o, " %08x:%u(%d)", rs12_empty_pc[bi][0], bc, (int)rs12_empty_pc[bi][2]);
					rs12_empty_pc[bi][1] = 0;
				}
				for (int k = 0; k < 64; k++)
					rs12_empty_pc[k][1] = 0;
				RS12_LOG("empty checks:%s", eb);
			}
			s_win_ex = s_win_sw = 0;
			s_win_ex_max = s_win_sw_max = 0;
			s_win_over[0] = s_win_over[1] = 0;
			{ // recompiler time and blocks in this window (a share of the emulation time)
				static uint64_t last_cus;
				static uint32_t last_blk;
				const uint64_t cus = rs12_ndrc_compile_us;
				const uint32_t blk = rs12_ndrc_blocks;
				extern volatile uint32_t rs12_ndrc_wraps;
				extern uint32_t rs12_ndrc_tc_used(void);
				if (blk != last_blk)
				{
					extern volatile uint32_t rs12_ndrc_warm_done, rs12_ndrc_scan_found;
					extern volatile uint32_t rs12_ndrc_kept, rs12_ndrc_invalidated;
					RS12_LOG("ndrc f%u compile %.2f ms/frame, %u blocks, cache at %.2f MB, wraps %u, warm %u/%d, scan %u, "
						"inv %u kept %u",
						frames, (cus - last_cus) / 60.0 / 1000.0, blk - last_blk, rs12_ndrc_tc_used() / 1048576.0,
						rs12_ndrc_wraps, rs12_ndrc_warm_done, s_warm_n, rs12_ndrc_scan_found, rs12_ndrc_invalidated,
						rs12_ndrc_kept);
					// blocks compiled more than once in this window
					char hb[160];
					int ho = 0;
					for (int k = 0; k < 4; k++) {
						int bi = -1;
						for (int j = 0; j < 256; j++)
							if (rs12_ndrc_hot[j][1] > 1 && (bi < 0 || rs12_ndrc_hot[j][1] > rs12_ndrc_hot[bi][1]))
								bi = j;
						if (bi < 0)
							break;
						ho += snprintf(hb + ho, sizeof(hb) - ho, " %08x:%ux%u", rs12_ndrc_hot[bi][0], rs12_ndrc_hot[bi][1],
							rs12_ndrc_hot[bi][2] / rs12_ndrc_hot[bi][1]);
						rs12_ndrc_hot[bi][1] = 0;
					}
					if (ho)
						RS12_LOG("ndrc again:%s", hb);
					extern uint32_t rs12_ndrc_big[8][2], rs12_ndrc_big_n;
					static uint32_t last_big;
					if (rs12_ndrc_big_n != last_big) { // the last big blocks (400+ instructions)
						ho = 0;
						const uint32_t nb = rs12_ndrc_big_n - last_big < 8 ? rs12_ndrc_big_n - last_big : 8;
						for (uint32_t k = 0; k < nb; k++) {
							const uint32_t *b = rs12_ndrc_big[(rs12_ndrc_big_n - 1 - k) & 7];
							ho += snprintf(hb + ho, sizeof(hb) - ho, " %08x+%u", b[0], b[1]);
						}
						RS12_LOG("ndrc big %u:%s", rs12_ndrc_big_n - last_big, hb);
						last_big = rs12_ndrc_big_n;
					}
				}
				memset(rs12_ndrc_hot, 0, sizeof(rs12_ndrc_hot));
				last_cus = cus;
				last_blk = blk;
			}
			{ // the ROM's graphics bank read in the background after the start (s12_rom.c)
				extern volatile uint32_t s12_rom_bg_ms;
				static uint32_t bg_seen;
				if (s12_rom_bg_ms != bg_seen) {
					bg_seen = s12_rom_bg_ms;
					RS12_LOG("rom: background read done in %u ms", (unsigned)bg_seen);
				}
			}
			{ // boards that test their ROMs at power-on show a still screen for up to a minute (golgo13, kartduel:
			  // the same in MAME): say so, so it does not look like a hang
				static int still;
				still = (gfps < 0.5 && frames < 7200) ? still + 1 : 0;
				if (still == 5 || (still > 5 && still % 10 == 0))
					rs12_osd_show("Starting up: the board checks its ROMs (up to a minute)");
			}
			win_n = 0;
			win_t0 = c;
			sum_emu = sum_present = 0;
			max_emu = 0;
		}
		// FPS box (top right, refreshed every 0.1 s) and notice expiry
		rs12_fps_update(osd_fps, frames);
		rs12_touch_overlay_update(0);
		rs12_osd_tick();
	}

out:
	RS12_LOG("game end (mode %d%s)", quit_mode, s_exit_next ? ", next" : "");
	s_quit = 1;
	// EEPROM changes not written yet (the watch thread skips them at s_quit; wait if it is in the middle of a write)
	while (s_eep_writing)
		sceKernelDelayThread(1000);
	if (s12.eeprom_dirty || s_eep_pending)
		eep_write(s12.eeprom);
	// a save state still being written (the watch thread finishes one it has started; one it never took is done here)
	for (int i = 0; i < 200 && s_st_req; i++)
		sceKernelDelayThread(10000);
	if (s_st_req == 1)
		state_io();
	// recompiler blocks for the next warm start
	while (s_warm_writing)
		sceKernelDelayThread(1000);
	if (s_warm_path[0] && psxCpu != &psxInt) {
		s_warm_n = rs12_ndrc_warm_collect(s_warm_buf, WARM_MAX);
		warm_write();
	}
	// stop the game's threads: the sound thread, the GPU threads (async renderer; the split helper waits for the next
	// game), the recompiler thread on CPU3
	if (s_snd_thread >= 0) {
		sceKernelSignalSema(s_snd_go, 1);
		sceKernelWaitThreadEnd(s_snd_thread, NULL, NULL);
		sceKernelDeleteThread(s_snd_thread);
		sceKernelDeleteSema(s_snd_go);
		sceKernelDeleteSema(s_snd_done);
		s_snd_thread = -1;
	}
	if (s_emu_up) {
		GPU_close();
		GPU_shutdown();
		psxShutdown();
		s_emu_up = 0;
	}
	if (quit_mode == 2 || s_exit_next) {
		// next game in this process: the ROM is swapped, the app keeps running (a relaunch with sceAppMgrLoadExec, as
		// ReST-V does, failed now and then: the new process never started, error C1-2569-2). The emulator was shut down
		// above and starts again from psxInit (the harness checks a game run after another one hashes the same,
		// RS12_PRERUN); video, audio output, log and the translation cache stay
		s12_rom_free(&rom);
		// the game's hand-over state: no sound batch in flight (the next game's first frame waited for one forever),
		// no EEPROM or recompiler list still to write (the watch thread would write it under the next game's name)
		s_snd_prev = NULL;
		s_snd_batch = NULL;
		s_eep_pending = 0;
		s_warm_pending = 0;
		free(s_rec);
		s_rec = NULL;
		s_rec_len = s_rec_cap = 0;
		s_rec_ready = 0;
		{
			SceKernelFreeMemorySizeInfo fm;
			fm.size = sizeof(fm);
			struct mallinfo mi = mallinfo(); // (newlib heap in use: a leak per game would grow it)
			if (sceKernelGetFreeMemorySize(&fm) < 0)
				fm.size_user = fm.size_cdram = 0;
			RS12_LOG("next game (%s): free user %u KB cdram %u KB, heap in use %u KB", quit_mode == 2 ? "list" :
				"restart file or list", fm.size_user >> 10, fm.size_cdram >> 10, (unsigned)mi.uordblks >> 10);
		}
		quit_mode = 0;
		s_exit_next = 0;
		s_quit = 0;
		goto next_game;
	}
	s_app_quit = 1;
	if (s_watch_thread >= 0) { // (it ends at s_app_quit; woken so it does not sit out its 1 s wait)
		if (s_watch_wake >= 0)
			sceKernelSignalSema(s_watch_wake, 1);
		SceUInt wt = 3000 * 1000;
		sceKernelWaitThreadEnd(s_watch_thread, NULL, &wt);
	}
	rs12_audio_fini();
	rs12_video_shutdown();
	rs12_ps_release();
	RS12_LOG("exit: threads stopped");
	rs12_jit_shutdown();
	rs12_log_shutdown();
	sceKernelExitProcess(0);
	return 0;
}

// ReSys12 PC harness: headless run of a System 12 set for N frames.
//   harness --roms DIR --game SET [--frames N] [--checkpoint N] [--dump F1,F2,...] [--out DIR] [--rtc UNIXTIME|now]
// Prints FNV-1a hashes of every presented frame (accumulated) and writes PPM dumps.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "psxcommon.h"
#include "r3000a.h"
#include "plugins.h"
#include "../../src/psx/frontend/plugin_lib.h"
#include "s12.h"
#include "s12_rom.h"
#include "s12_state.h"
#include "h8.h"
#ifdef RS12_HARNESS_DYNAREC
#include "new_dynarec/new_dynarec.h"
#endif

static const char *savestate_file, *loadstate_file;
static int savestate_frame, loadstate_frame;

extern void rs12_psx_bind(void);
extern void (*rs12_GPUrearmedCallbacks)(const struct rearmed_cbs *);
extern unsigned int frame_counter;
extern uint64_t s12_sub_profile_ns(void);
// ReSys12: split GPU rendering (src/psx/plugins/gpu_neon/psx_gpu_if.c)
extern void rs12_gpu_split_enable(int enable);
extern void rs12_gpu_split_set_ratio(int ratio);
extern const void *rs12_gpu_vram_ptr(void);
extern volatile uint32_t rs12_gpu_split_chunks, rs12_gpu_split_barriers, rs12_gpu_split_solos;
extern volatile uint32_t rs12_gpu_split_mismatch;

static uint64_t s_hash = 1469598103934665603ull;
static uint64_t s_ahash = 1469598103934665603ull;
static int s_apeak;
static FILE *s_wavf;
static int s_w, s_h, s_bpp = 16;
static uint32_t *s_frame; // last presented frame, XRGB8888
static int s_fw, s_fh;
static unsigned s_flips;

static void fnv(const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	for (size_t i = 0; i < n; i++) {
		s_hash ^= b[i];
		s_hash *= 1099511628211ull;
	}
}

static int vout_open(void) { return 0; }
static void vout_close(void) {}
static void vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp)
{
	(void)raw_w; (void)raw_h;
	s_w = w;
	s_h = h;
	s_bpp = bpp;
	fprintf(stderr, "[f%u] vout mode %dx%d bpp %d\n", frame_counter, w, h, bpp);
}

static void vout_flip(const void *vram, int vram_offset, int bgr24, int x, int y, int w, int h, int dims_changed)
{
	(void)x; (void)y; (void)dims_changed;
	s_flips++;
	if (!vram || w <= 0 || h <= 0)
		return;
	if (w != s_fw || h != s_fh) {
		free(s_frame);
		s_frame = (uint32_t *)calloc((size_t)w * h, 4);
		s_fw = w;
		s_fh = h;
	}
	const uint8_t *base = (const uint8_t *)vram + vram_offset;
	for (int j = 0; j < h; j++) {
		const uint8_t *row = base + (size_t)j * 2048; // VRAM stride 1024 pixels
		uint32_t *dst = s_frame + (size_t)j * w;
		for (int i = 0; i < w; i++) {
			uint32_t c;
			if (bgr24) {
				const uint8_t *p = row + i * 3;
				c = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
			} else {
				const uint16_t v = (uint16_t)(row[i * 2] | (row[i * 2 + 1] << 8));
				const uint32_t r = (v & 31) << 3, g = ((v >> 5) & 31) << 3, b = ((v >> 10) & 31) << 3;
				c = r << 16 | g << 8 | b;
			}
			dst[i] = c;
		}
		fnv(dst, (size_t)w * 4);
	}
}

static void write_ppm(const char *path)
{
	if (!s_frame)
		return;
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", s_fw, s_fh);
	for (int i = 0; i < s_fw * s_fh; i++) {
		const uint32_t c = s_frame[i];
		const uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

static int in_list(const char *list, int f)
{
	if (!list)
		return 0;
	const char *p = list;
	while (*p) {
		if (atoi(p) == f)
			return 1;
		p = strchr(p, ',');
		if (!p)
			break;
		p++;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *roms = "roms", *game = "tektagt", *dump = NULL, *out = ".", *wav = NULL;
	int frames = 600, checkpoint = 0;
	int64_t rtc = 946684800; // fixed RTC (2000-01-01 00:00:00 UTC) keeps runs deterministic; --rtc now = host clock
	int dynarec = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dynarec")) dynarec = 1; // ARM builds only (RS12_DYNAREC)
		else if (!strcmp(argv[i], "--roms") && i + 1 < argc) roms = argv[++i];
		else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--checkpoint") && i + 1 < argc) checkpoint = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i]; // raw 88.2kHz stereo s16le
		else if (!strcmp(argv[i], "--savestate") && i + 2 < argc) {
			savestate_frame = atoi(argv[++i]);
			savestate_file = argv[++i];
		} else if (!strcmp(argv[i], "--loadstate") && i + 2 < argc) {
			loadstate_frame = atoi(argv[++i]);
			loadstate_file = argv[++i];
		}
		else if (!strcmp(argv[i], "--rtc") && i + 1 < argc) {
			++i;
			rtc = strcmp(argv[i], "now") ? atoll(argv[i]) : 0;
		}
	}

	static S12Rom rom;
	char msg[256];
	if (s12_rom_load_cached(&rom, roms, getenv("RS12_ROMCACHE"), game, msg, sizeof(msg), NULL) != 0) {
		fprintf(stderr, "rom: %s\n", msg);
		return 1;
	}
	fprintf(stderr, "rom: %s %s (%s)\n", game, rom.game->title, msg);
	s12_attach(&rom);
	if (getenv("RS12_EEPROM")) { // ReSys12: start from a saved EEPROM image (Vita nvram/<set>.eep)
		FILE *ef = fopen(getenv("RS12_EEPROM"), "rb");
		if (ef) {
			fprintf(stderr, "eeprom %s: %zu\n", getenv("RS12_EEPROM"), fread(s12.eeprom, 1, sizeof(s12.eeprom), ef));
			fclose(ef);
		}
	}
	s12.rtc_base = rtc;

	memset(&Config, 0, sizeof(Config));
	Config.Cpu = CPU_INTERPRETER;
#ifdef RS12_HARNESS_DYNAREC
	if (dynarec)
		Config.Cpu = CPU_DYNAREC;
#else
	(void)dynarec;
#endif
	Config.cycle_multiplier = Config.Cpu == CPU_DYNAREC ? S12_CYCLE_MULTIPLIER_DYNAREC : S12_CYCLE_MULTIPLIER;
	// ReSys12: timing experiments. RS12_CMULT = cycle multiplier, RS12_NDHACKS = new_dynarec hack bits (new_dynarec.h)
	if (getenv("RS12_CMULT"))
		Config.cycle_multiplier = atoi(getenv("RS12_CMULT"));
#ifdef RS12_HARNESS_DYNAREC
	if (getenv("RS12_NDHACKS"))
		ndrc_g.hacks = (unsigned)strtoul(getenv("RS12_NDHACKS"), NULL, 0);
#endif

	pl_rearmed_cbs.pl_vout_open = vout_open;
	pl_rearmed_cbs.pl_vout_close = vout_close;
	pl_rearmed_cbs.pl_vout_set_mode = vout_set_mode;
	pl_rearmed_cbs.pl_vout_flip = vout_flip;
	{
		extern int rs12_log_vram_reads;
		rs12_log_vram_reads = getenv("RS12_LOGREAD") != NULL;
		extern int rs12_gp0dump_frame;
		s12_sound_half_rate = getenv("RS12_HALFRATE") != NULL;
		extern unsigned int rs12_watch_lo, rs12_watch_hi, rs12_watch_from;
		if (getenv("RS12_WATCH_FROM"))
			rs12_watch_from = (unsigned)atoi(getenv("RS12_WATCH_FROM"));
		if (getenv("RS12_WATCH"))
			sscanf(getenv("RS12_WATCH"), "%x-%x", &rs12_watch_lo, &rs12_watch_hi);
		if (getenv("RS12_GP0DUMP"))
			rs12_gp0dump_frame = atoi(getenv("RS12_GP0DUMP"));
	}
	pl_rearmed_cbs.gpu_neon.allow_interlace = getenv("RS12_INTERLACE") ? atoi(getenv("RS12_INTERLACE")) : 2; // ReSys12: 0 off, 1 on, 2 auto

	rs12_psx_bind();
	if (psxInit() != 0) {
		fprintf(stderr, "psxInit failed\n");
		return 1;
	}
	rs12_GPUrearmedCallbacks(&pl_rearmed_cbs);
	GPU_init();
	// ReSys12: GPU_init clears the GPU state (allow_interlace, frame counter...): hand the settings over again
	rs12_GPUrearmedCallbacks(&pl_rearmed_cbs);
	GPU_open(NULL, NULL, NULL);
	psxReset();
	if (getenv("RS12_SOUND_SYNC")) // ReSys12: exact (MAME-like) C352 timing
		s12_sound_sync = atoi(getenv("RS12_SOUND_SYNC"));
	// ReSys12: split GPU rendering (two renderer instances, run one after the other here):
	// RS12_GPUSPLIT=1 [RS12_GPUSPLIT_RATIO=1..255 = top band share /256]. Hashes must match the default.
	if (getenv("RS12_GPUSPLIT") && atoi(getenv("RS12_GPUSPLIT"))) {
		rs12_gpu_split_enable(1);
		if (getenv("RS12_GPUSPLIT_RATIO"))
			rs12_gpu_split_set_ratio(atoi(getenv("RS12_GPUSPLIT_RATIO")));
		fprintf(stderr, "gpu split: on\n");
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	// --savestate N FILE: save after frame N. --loadstate N FILE: load FILE and continue as frame N+1
	int first = 1;
	if (loadstate_file) {
		FILE *sf = fopen(loadstate_file, "rb");
		if (sf) {
			fseek(sf, 0, SEEK_END);
			const long sz = ftell(sf);
			fseek(sf, 0, SEEK_SET);
			uint8_t *sb = (uint8_t *)malloc((size_t)sz);
			if (sb && fread(sb, 1, (size_t)sz, sf) == (size_t)sz)
				fprintf(stderr, "loadstate %s: %d\n", loadstate_file, s12_state_load(sb, (size_t)sz));
			free(sb);
			fclose(sf);
			first = loadstate_frame + 1;
		}
	}
	// ReSys12: scripted input for gameplay scenes. RS12_INPUT = "F:P1:SYS,..." (hex masks, S12_IN_* / S12_SYS_*,
	// each held from frame F on); RS12_INPUT_MASH = F: from frame F, player 1 mashes buttons and directions (fixed LCG)
	const char *in_script = getenv("RS12_INPUT");
	const int mash_from = getenv("RS12_INPUT_MASH") ? atoi(getenv("RS12_INPUT_MASH")) : -1;
	uint32_t mash_rng = 12345, mash_p1 = 0;
	for (int f = first; f <= frames; f++) {
		for (const char *q = in_script; q && *q;) {
			unsigned ff, p1, sys;
			if (sscanf(q, "%u:%x:%x", &ff, &p1, &sys) == 3 && (int)ff == f) {
				s12.in.player[0] = p1;
				s12.in.system = sys;
			}
			q = strchr(q, ',');
			if (q)
				q++;
		}
		if (mash_from >= 0 && f >= mash_from) {
			if (f % 6 == 0) {
				mash_rng = mash_rng * 1103515245u + 12345u;
				mash_p1 = (mash_rng >> 8) & 0x3ff & ~(uint32_t)0x3; // buttons 1-6, left/right; no up/down
			}
			s12.in.player[0] = (f % 6) < 4 ? mash_p1 : 0;
		}
		if (getenv("RS12_FLUSHTEX")) { // ReSys12 (diagnostics): drop every texture cache each frame
			extern void renderer_update_caches(int x, int y, int w, int h, int state_changed);
			renderer_update_caches(0, 0, 1024, 1024, 0);
		}
		psxRegs.stop = 0;
		psxCpu->Execute(&psxRegs);
		{
			// C352 output of this frame: audio hash, peak, optional raw dump (88.2kHz stereo s16le)
			// same frame-batch flow as the Vita sound thread, run synchronously
			static int16_t abuf[S12_SOUND_MAX_FRAMES * 2];
			static S12SoundBatch *prev;
			S12SoundBatch *batch = s12_sub_sound_frame(prev);
			const int n = s12_sound_render(batch, abuf); // RS12_HALFRATE=1: 44.1kHz output as on the Vita
			prev = batch;
			for (int k = 0; k < n * 2; k++) {
				s_ahash = (s_ahash ^ (uint16_t)abuf[k]) * 1099511628211ull;
				const int v = abuf[k] < 0 ? -abuf[k] : abuf[k];
				if (v > s_apeak)
					s_apeak = v;
			}
			if (wav) {
				if (!s_wavf)
					s_wavf = fopen(wav, "wb");
				if (s_wavf)
					fwrite(abuf, 4, (size_t)n, s_wavf);
			}
		}
		if (getenv("RS12_RAMHASH")) {
			// per-frame hash of main RAM and the H8 shared RAM: first divergence between interpreter and recompiler
			uint64_t h = 1469598103934665603ull;
			const uint8_t *m = (const uint8_t *)psxRegs.ptrs.psxM;
			for (uint32_t k = 0; k < 0x400000; k++)
				h = (h ^ m[k]) * 1099511628211ull;
			uint64_t hs = 1469598103934665603ull;
			const uint8_t *sh = (const uint8_t *)s12.shared;
			for (uint32_t k = 0; k < sizeof(s12.shared); k++)
				hs = (hs ^ sh[k]) * 1099511628211ull;
			fprintf(stderr, "ram f%d %016llx shared %016llx pc %08x\n", f, (unsigned long long)h, (unsigned long long)hs,
				psxRegs.pc);
		}
		if (getenv("RS12_CP0"))
			fprintf(stderr, "f%d pc %08x cause %08x epc %08x sr %08x ra %08x\n", f, psxRegs.pc, psxRegs.CP0.n.Cause,
				psxRegs.CP0.n.EPC, psxRegs.CP0.n.SR, psxRegs.GPR.n.ra);
		if (savestate_file && f == savestate_frame) {
			size_t ss = 0;
			uint8_t *sb = s12_state_save(&ss);
			FILE *sf = sb ? fopen(savestate_file, "wb") : NULL;
			if (sf) {
				fwrite(sb, 1, ss, sf);
				fclose(sf);
			}
			fprintf(stderr, "savestate f%d: %zu bytes\n", f, ss);
			free(sb);
		}
		if (checkpoint && f % checkpoint == 0) {
			// h8busy: H8 states run (not idle-skipped or sleeping) per frame since the last checkpoint
			static uint64_t last_h8_total, last_h8_idle;
			const uint64_t h8t = h8_total_cycles(), h8i = h8.idle_cycles;
			fprintf(stderr, "f%d pc %08x hash %016llx flips %u unmapped %u h8busy %llu\n", f, psxRegs.pc,
				(unsigned long long)s_hash, s_flips, s12.log_unmapped,
				(unsigned long long)((h8t - last_h8_total) - (h8i - last_h8_idle)) / (unsigned)checkpoint);
			{ // ReSys12: VRAM transfers that forced a renderer sync since the last checkpoint (read/big/split/other)
				extern volatile uint32_t rs12_vram_sync_n[4], rs12_vram_sync_words[4];
				static uint32_t ln[4], lw[4];
				fprintf(stderr, "  vramsync r %u/%u big %u/%u split %u/%u other %u/%u\n",
					rs12_vram_sync_n[0] - ln[0], rs12_vram_sync_words[0] - lw[0], rs12_vram_sync_n[1] - ln[1],
					rs12_vram_sync_words[1] - lw[1], rs12_vram_sync_n[2] - ln[2], rs12_vram_sync_words[2] - lw[2],
					rs12_vram_sync_n[3] - ln[3], rs12_vram_sync_words[3] - lw[3]);
				for (int k = 0; k < 4; k++) {
					ln[k] = rs12_vram_sync_n[k];
					lw[k] = rs12_vram_sync_words[k];
				}
			}
			if (getenv("RS12_GPUSTAT")) { // ReSys12: GPU status (interlace bit 22, 480 lines bit 19)
				extern uint32_t GPUreadStatus(void);
				fprintf(stderr, "  gpustat %08x\n", GPUreadStatus());
			}
			last_h8_total = h8t;
			last_h8_idle = h8i;
			if (getenv("RS12_VRAMHASH")) { // ReSys12: whole 1024x1024 VRAM, incl. off-screen
				uint64_t vh = 1469598103934665603ull;
				const uint8_t *vb = (const uint8_t *)rs12_gpu_vram_ptr();
				for (size_t i = 0; vb && i < 1024 * 1024 * 2; i++)
					vh = (vh ^ vb[i]) * 1099511628211ull;
				fprintf(stderr, "f%d vram %016llx\n", f, (unsigned long long)vh);
			}
		}
		if (in_list(dump, f)) {
			char p[512];
			snprintf(p, sizeof(p), "%s/%s_f%d.ppm", out, game, f);
			write_ppm(p);
			if (getenv("RS12_TEX8DUMP")) { // ReSys12 (diagnostics): 8bpp cache entry of page RS12_TEX8DUMP
				extern void rs12_dump_tex8(int page, void *out);
				static unsigned char t8[65536];
				rs12_dump_tex8(atoi(getenv("RS12_TEX8DUMP")), t8);
				snprintf(p, sizeof(p), "%s/%s_f%d_tex8.bin", out, game, f);
				FILE *tf = fopen(p, "wb");
				if (tf) {
					fwrite(t8, 1, sizeof(t8), tf);
					fclose(tf);
				}
			}
			if (getenv("RS12_VRAMDUMP_ALL")) { // ReSys12: whole VRAM as 1024x1024 15-bit colour (texture pages show as noise)
				const uint16_t *vw = (const uint16_t *)rs12_gpu_vram_ptr();
				snprintf(p, sizeof(p), "%s/%s_f%d_vram.ppm", out, game, f);
				FILE *vf = vw ? fopen(p, "wb") : NULL;
				if (vf) {
					fprintf(vf, "P6\n1024 1024\n255\n");
					for (int i = 0; i < 1024 * 1024; i++) {
						const uint16_t c = vw[i];
						const uint8_t rgb[3] = { (uint8_t)((c & 31) << 3), (uint8_t)(((c >> 5) & 31) << 3),
							(uint8_t)(((c >> 10) & 31) << 3) };
						fwrite(rgb, 1, 3, vf);
					}
					fclose(vf);
				}
				// and the exact 16-bit words (bit 15 is a texel bit in 8bpp/4bpp textures)
				snprintf(p, sizeof(p), "%s/%s_f%d_vram.bin", out, game, f);
				vf = vw ? fopen(p, "wb") : NULL;
				if (vf) {
					fwrite(vw, 2, 1024 * 1024, vf);
					fclose(vf);
				}
			}
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	const double total_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
	printf("%s frames %d video %016llx audio %016llx peak %d pc %08x\n", game, frames, (unsigned long long)s_hash,
		(unsigned long long)s_ahash, s_apeak, psxRegs.pc);
	if (s_wavf)
		fclose(s_wavf);
	if (frames > 0)
		fprintf(stderr, "time: %.3f ms/frame total, %.3f ms/frame sub CPU (H8)\n", total_ms / frames,
			(double)s12_sub_profile_ns() / 1e6 / frames);
	if (getenv("RS12_GPUSPLIT")) // ReSys12
		fprintf(stderr, "gpu split: chunks %u barriers %u solo %u mismatch %u\n", rs12_gpu_split_chunks,
			rs12_gpu_split_barriers, rs12_gpu_split_solos, rs12_gpu_split_mismatch);
	if (getenv("RS12_RAMDUMP")) {
		// raw 4MB main RAM at exit (disassemble with mipsel-linux-gnu-objdump -b binary -m mips --adjust-vma=0x80000000)
		FILE *rf = fopen(getenv("RS12_RAMDUMP"), "wb");
		if (rf) {
			fwrite(psxRegs.ptrs.psxM, 1, 0x400000, rf);
			fclose(rf);
		}
	}
	s12_dump_unmapped();
	s12_sub_dump();
	if (getenv("RS12_PCHIST"))
		s12_pchist_dump();
#ifdef RS12_H8_PCHIST
	if (getenv("RS12_H8HIST"))
		h8_pchist_dump(40);
#endif
	return 0;
}

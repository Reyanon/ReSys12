// ReSys12: System 12 ROM set loader. Load rules follow MAME ROM_LOAD* semantics (little-endian regions).
#include "s12_rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs12_zip.h"
#include "s12_games.h"
#include "s12_games_0260.h"

static void (*s_progress)(int permille);

int s12_game_count(void)
{
	return S12_NUM_GAMES;
}

const S12Game *s12_game(int index)
{
	return (index >= 0 && index < S12_NUM_GAMES) ? &s12_games[index] : NULL;
}

int s12_find_game(const char *name)
{
	for (int i = 0; i < S12_NUM_GAMES; i++)
		if (strcmp(s12_games[i].name, name) == 0)
			return i;
	return -1;
}

static uint32_t region_size(const S12Game *g, const S12RomEntry *roms, int rgn)
{
	switch (rgn) {
	case S12_RGN_MAIN: return g->main_size;
	case S12_RGN_BANK: return g->bank_size;
	case S12_RGN_SUB: return g->sub_size;
	case S12_RGN_C352: return g->c352_size;
	default: break;
	}
	// small regions: size from the entries
	uint32_t s = 0;
	for (int i = 0; i < g->rom_count; i++) {
		const S12RomEntry *e = &roms[g->rom_start + i];
		if (e->region == rgn && e->offset + e->size > s)
			s = e->offset + e->size;
	}
	return s;
}

// MAME ROM_LOAD* placement of `src` (size bytes) into region `dst` at e->offset
static void place(uint8_t *dst, uint32_t dst_size, const S12RomEntry *e, const uint8_t *src, uint32_t size)
{
	uint32_t o = e->offset;
	switch (e->load) {
	case S12_LD_PLAIN:
		if (o + size <= dst_size)
			memcpy(dst + o, src, size);
		break;
	case S12_LD_16_BYTE: // every other byte
		for (uint32_t i = 0; i < size && o + i * 2 < dst_size; i++)
			dst[o + i * 2] = src[i];
		break;
	case S12_LD_16_WORD_SWAP: // 16-bit words, byte swapped (big-endian CPU data)
		for (uint32_t i = 0; i + 1 < size && o + i + 1 < dst_size; i += 2) {
			dst[o + i] = src[i + 1];
			dst[o + i + 1] = src[i];
		}
		break;
	case S12_LD_32_BYTE: // one byte of every 32-bit word
		for (uint32_t i = 0; i < size && o + i * 4 < dst_size; i++)
			dst[o + i * 4] = src[i];
		break;
	case S12_LD_32_WORD: // one 16-bit half of every 32-bit word
		for (uint32_t i = 0; i + 1 < size && o + i * 2 + 1 < dst_size; i += 2) {
			dst[o + i * 2] = src[i];
			dst[o + i * 2 + 1] = src[i + 1];
		}
		break;
	default:
		break;
	}
}

static const ZipEntry *find_in(Zip *zips, int nz, const S12RomEntry *e, Zip **owner, int *by_name)
{
	// CRC first in every zip, then name
	for (int z = 0; z < nz; z++) {
		if (!zips[z].fp)
			continue;
		for (int i = 0; i < zips[z].count; i++)
			if (zips[z].e[i].header.crc32 == e->crc) {
				*owner = &zips[z];
				*by_name = 0;
				return &zips[z].e[i];
			}
	}
	for (int z = 0; z < nz; z++) {
		if (!zips[z].fp)
			continue;
		for (int i = 0; i < zips[z].count; i++)
			if (strcmp(zips[z].e[i].name, e->name) == 0 && zips[z].e[i].header.uncompressedSize == e->size) {
				*owner = &zips[z];
				*by_name = 1;
				return &zips[z].e[i];
			}
	}
	return NULL;
}

// load the set described by g / roms (one of the ROM tables); r->game = meta (always the main table's entry)
static int load_with(S12Rom *r, const S12Game *meta, const S12Game *g, const S12RomEntry *roms, const char *roms_dir,
	char *msg, int msg_len)
{
	memset(r, 0, sizeof(*r));
	r->game = meta;

	Zip zips[2];
	memset(zips, 0, sizeof(zips));
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.zip", roms_dir, meta->name); // the file has the requested name (renamed sets)
	if (zip_open(&zips[0], path) != 0) {
		snprintf(msg, msg_len, "cannot open %s", path);
		return -2;
	}
	if (g->parent[0]) {
		snprintf(path, sizeof(path), "%s/%s.zip", roms_dir, g->parent);
		zip_open(&zips[1], path); // optional: non-merged sets are complete on their own
	}

	for (int rg = 0; rg < S12_RGN_COUNT; rg++) {
		r->rgn_size[rg] = region_size(g, roms, rg);
		if (r->rgn_size[rg]) {
			r->rgn[rg] = (uint8_t *)calloc(1, r->rgn_size[rg]);
			if (!r->rgn[rg]) {
				snprintf(msg, msg_len, "out of memory (region %d, %u bytes)", rg, (unsigned)r->rgn_size[rg]);
				zip_close(&zips[0]);
				zip_close(&zips[1]);
				s12_rom_free(r);
				return -3;
			}
		}
	}

	const S12RomEntry *last = NULL;
	uint8_t *last_data = NULL;
	uint32_t last_size = 0;
	for (int i = 0; i < g->rom_count; i++) {
		const S12RomEntry *e = &roms[g->rom_start + i];
		uint8_t *dst = r->rgn[e->region];
		const uint32_t dsz = r->rgn_size[e->region];
		if (s_progress)
			s_progress(i * 900 / g->rom_count); // the last 10% is for writing the cache
		if (e->load == S12_LD_FILL) {
			if (dst && e->offset + e->size <= dsz)
				memset(dst + e->offset, (int)e->crc, e->size);
			continue;
		}
		if (e->load == S12_LD_RELOAD) {
			if (last && last_data) {
				S12RomEntry re = *last;
				re.offset = e->offset;
				place(dst, dsz, &re, last_data, e->size < last_size ? e->size : last_size);
			}
			continue;
		}
		free(last_data);
		last_data = NULL;
		last = e;
		if (e->flags & S12_F_NODUMP)
			continue;
		Zip *owner = NULL;
		int by_name = 0;
		const ZipEntry *ze = find_in(zips, 2, e, &owner, &by_name);
		if (!ze) {
			r->missing++;
			if (r->missing == 1)
				snprintf(msg, msg_len, "missing %s (crc %08x)", e->name, (unsigned)e->crc);
			continue;
		}
		r->by_name += by_name;
		uint32_t size = 0;
		last_data = zip_read(owner, ze, &size);
		if (!last_data) {
			r->missing++;
			snprintf(msg, msg_len, "read error %s", e->name);
			continue;
		}
		last_size = size;
		place(dst, dsz, e, last_data, size);
	}
	free(last_data);
	zip_close(&zips[0]);
	zip_close(&zips[1]);
	if (r->missing) {
		s12_rom_free(r);
		return -4;
	}
	snprintf(msg, msg_len, "ok%s", r->by_name ? " (some files matched by name only)" : "");
	return 0;
}

// ReSys12: current MAME keeps the JVS I/O boards' program ROMs in the board devices, MAME 0.260 had them in the set
// (technodr th1io-a.4f, aplarail asca1_io-a.ic2). The JVS model reads the board's identification from it, so when
// the main table has no I/O region, the 0.260 entries are loaded too if the zip has them (optional).
static const S12Game *iocpu_0260(const char *name)
{
	for (int i = 0; i < S12_NUM_GAMES_0260; i++) {
		const S12Game *o = &s12_games_0260[i];
		if (strcmp(o->name, name) != 0)
			continue;
		for (int k = 0; k < o->rom_count; k++)
			if (s12_roms_0260[o->rom_start + k].region == S12_RGN_IOCPU)
				return o;
		return NULL;
	}
	return NULL;
}

static void load_iocpu_extra(S12Rom *r, const char *roms_dir)
{
	const S12Game *o = r->rgn[S12_RGN_IOCPU] ? NULL : iocpu_0260(r->game->name);
	if (!o)
		return;
	const uint32_t size = region_size(o, s12_roms_0260, S12_RGN_IOCPU);
	Zip z;
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.zip", roms_dir, r->game->name);
	if (!size || zip_open(&z, path) != 0)
		return;
	uint8_t *dst = (uint8_t *)calloc(1, size);
	for (int k = 0; dst && k < o->rom_count; k++) {
		const S12RomEntry *e = &s12_roms_0260[o->rom_start + k];
		if (e->region != S12_RGN_IOCPU || e->load == S12_LD_FILL || e->load == S12_LD_RELOAD)
			continue;
		Zip *owner = NULL;
		int by_name = 0;
		const ZipEntry *ze = find_in(&z, 1, e, &owner, &by_name);
		uint32_t n = 0;
		uint8_t *data = ze ? zip_read(owner, ze, &n) : NULL;
		if (!data) {
			free(dst);
			dst = NULL;
			break;
		}
		place(dst, size, e, data, n);
		free(data);
	}
	zip_close(&z);
	if (dst) {
		r->rgn[S12_RGN_IOCPU] = dst;
		r->rgn_size[S12_RGN_IOCPU] = size;
	}
}

// ReSys12: the main table comes from current MAME; romsets made with MAME 0.260 have other dumps of some sets
// (e.g. soulclbr, tektagtjc1). If the main table misses files or matches some only by name, try the 0.260 lists.
int s12_rom_load(S12Rom *r, const char *roms_dir, const char *name, char *msg, int msg_len)
{
	const int gi = s12_find_game(name);
	if (gi < 0) {
		memset(r, 0, sizeof(*r));
		snprintf(msg, msg_len, "unknown set %s", name);
		return -1;
	}
	const S12Game *g = &s12_games[gi];
	int rc = load_with(r, g, g, s12_roms, roms_dir, msg, msg_len);
	if (rc == 0 && !r->by_name) {
		load_iocpu_extra(r, roms_dir);
		return 0;
	}
	if (rc != 0 && rc != -4)
		return rc;
	// sets renamed since MAME 0.260 (current name -> 0.260 name)
	static const char *const renamed[][2] = {
		{ "soulclbrac", "soulclbra" }, { "soulclbrja2", "soulclbrja" }, { "tenkomorja1", "tenkomorja" },
		{ "fgtlayerja", "fgtlayerj" }, { "tektagtc1a", "tektagtc1" }, { "tektagtja3", "tektagtja" },
		{ "mrdrillrja2", "mrdrillrj" }, { "kartduelja", "kartduelj" },
	};
	const char *old = name;
	for (size_t k = 0; k < sizeof(renamed) / sizeof(renamed[0]); k++)
		if (!strcmp(renamed[k][0], name))
			old = renamed[k][1];
	for (int i = 0; i < S12_NUM_GAMES_0260; i++) {
		if (strcmp(s12_games_0260[i].name, name) != 0 && strcmp(s12_games_0260[i].name, old) != 0)
			continue;
		S12Rom alt;
		char amsg[160];
		const int arc = load_with(&alt, g, &s12_games_0260[i], s12_roms_0260, roms_dir, amsg, sizeof(amsg));
		if (arc == 0 && !alt.by_name) {
			if (rc == 0)
				s12_rom_free(r);
			*r = alt;
			snprintf(msg, msg_len, "ok (MAME 0.260 set)");
			return 0;
		}
		if (arc == 0)
			s12_rom_free(&alt); // (a failed load frees its regions itself)
		break;
	}
	return rc;
}

#ifdef __vita__
static void cache_bg_finish(void);
#endif
void s12_rom_free(S12Rom *r)
{
#ifdef __vita__
	cache_bg_finish(); // (background readers write into the regions)
#endif
	for (int i = 0; i < S12_RGN_COUNT; i++) {
		free(r->rgn[i]);
		r->rgn[i] = NULL;
	}
}

// ---- assembled-region cache ----
// file: header, then the regions in S12_RGN order, each rgn_size bytes. Written to <name>.tmp, then renamed.
#define CACHE_MAGIC 0x31435352u // "RSC1"
#define CACHE_VERSION 1u

typedef struct {
	uint32_t magic, version;
	uint32_t key_lo, key_hi;          // FNV-1a over the zip directories used
	uint32_t rgn_size[S12_RGN_COUNT];
	char name[32];
} CacheHeader;

static void fnv64(uint64_t *h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	for (size_t i = 0; i < n; i++)
		*h = (*h ^ b[i]) * 1099511628211ull;
}

// key from the zip directories (names, CRCs, sizes) of the set and its parent: 0 if the set zip cannot be opened
static void hash_entry(uint64_t *h, const S12RomEntry *e)
{
	const uint32_t v[6] = { e->region, e->load, e->flags, e->offset, e->size, e->crc };
	fnv64(h, v, sizeof(v));
}

static uint64_t cache_key(const S12Game *g, const char *roms_dir)
{
	uint64_t h = 1469598103934665603ull;
	const uint32_t ver = CACHE_VERSION;
	fnv64(&h, &ver, sizeof(ver));
	if (iocpu_0260(g->name))
		fnv64(&h, "iocpu", 5); // caches from before the optional I/O ROM (load_iocpu_extra)
	// the set's load table (both lists): a corrected offset (tools/gen_s12_games.py OFFSET_FIX) rebuilds the cache
	for (int i = 0; i < g->rom_count; i++)
		hash_entry(&h, &s12_roms[g->rom_start + i]);
	for (size_t i = 0; i < sizeof(s12_games_0260) / sizeof(s12_games_0260[0]); i++)
		if (!strcmp(s12_games_0260[i].name, g->name)) {
			for (int k = 0; k < s12_games_0260[i].rom_count; k++)
				hash_entry(&h, &s12_roms_0260[s12_games_0260[i].rom_start + k]);
			break;
		}
	const char *sets[2] = { g->name, g->parent };
	for (int s = 0; s < 2; s++) {
		if (!sets[s][0])
			continue;
		char path[512];
		snprintf(path, sizeof(path), "%s/%s.zip", roms_dir, sets[s]);
		Zip z;
		if (zip_open(&z, path) != 0) {
			if (s == 0)
				return 0;
			continue;
		}
		for (int i = 0; i < z.count; i++) {
			fnv64(&h, z.e[i].name, strlen(z.e[i].name));
			fnv64(&h, &z.e[i].header.crc32, sizeof(z.e[i].header.crc32));
			fnv64(&h, &z.e[i].header.uncompressedSize, sizeof(z.e[i].header.uncompressedSize));
		}
		zip_close(&z);
	}
	return h ? h : 1;
}

#ifdef __vita__
// newlib fread on the Vita reached ~3.5MB/s here; sceIoRead with large blocks reads at card speed. The regions are
// read in 1MB pieces by two threads at once (each with its own handle, sceIoPread at the piece's offset): one
// reader reached ~13MB/s (80MB tektagt in 6.6 s), two 5.4 s, three or four no better.
// Only the program and the sub CPU are read before the game starts (tektagt 4.5MB of 80MB); the sound samples (16MB)
// and the graphics bank (56MB) follow in the background while the game boots. A ROM DMA, bank mapping or C352
// key-on that needs a piece not read yet asks for it next and waits (s12_rom_wait)
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

int s12_rom_cache_readers = 2; // threads reading the cache
int s12_rom_bg_load = 1;        // 0: everything is read before the game starts (measurements)
const char *s12_rom_cache_why = ""; // why the last cache read failed (log)
volatile uint32_t s12_rom_bg_ms;   // time the last background read took (log)
uint32_t s12_rom_stage_ms[3];       // last cache read: header, allocation, start pieces (log)
volatile int s12_rom_all_ready = 1; // 0 while pieces are still being read in the background

#define CACHE_PIECE 0x100000
static struct {
	const char *path;
	uint8_t *dst[S12_RGN_COUNT];
	uint32_t size[S12_RGN_COUNT];
	uint32_t first[S12_RGN_COUNT]; // first piece of each region
	uint32_t pieces, *order;       // reading order: the regions needed at start, then the bank
	volatile uint32_t next;        // next entry of order
	volatile uint8_t *taken, *ready;
	volatile int32_t want;         // a piece the emulation waits for, -1 = none
	volatile uint32_t done;        // pieces read
	volatile int failed;
	uint64_t t0;
} s_cr;
static SceUID s_cr_th[4];
static int s_cr_nth;

// piece k: its region, the offset in the region and in the file, its size
static int cache_piece(uint32_t k, int *rg, uint32_t *off, SceOff *foff, uint32_t *len)
{
	SceOff f = sizeof(CacheHeader);
	for (int i = 0; i < S12_RGN_COUNT; i++) {
		const uint32_t np = (s_cr.size[i] + CACHE_PIECE - 1) / CACHE_PIECE;
		if (k < np) {
			*rg = i;
			*off = k * CACHE_PIECE;
			*foff = f + *off;
			*len = s_cr.size[i] - *off < CACHE_PIECE ? s_cr.size[i] - *off : CACHE_PIECE;
			return 1;
		}
		k -= np;
		f += s_cr.size[i];
	}
	return 0;
}

// the next piece to read: the one the emulation waits for, else the next in order; -1 when all are taken
static int32_t cache_take(void)
{
	const int32_t w = s_cr.want;
	if (w >= 0 && __sync_bool_compare_and_swap(&s_cr.taken[w], 0, 1))
		return w;
	for (;;) {
		const uint32_t i = __atomic_fetch_add(&s_cr.next, 1, __ATOMIC_ACQ_REL);
		if (i >= s_cr.pieces)
			return -1;
		const uint32_t k = s_cr.order[i];
		if (__sync_bool_compare_and_swap(&s_cr.taken[k], 0, 1))
			return (int32_t)k;
	}
}

static int cache_reader(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	SceUID fd = sceIoOpen(s_cr.path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		s12_rom_cache_why = "reader open failed";
		s_cr.failed = 1;
		return 0;
	}
	for (;;) {
		const int32_t k = cache_take();
		int rg;
		uint32_t off, len;
		SceOff foff;
		if (k < 0 || s_cr.failed || !cache_piece((uint32_t)k, &rg, &off, &foff, &len))
			break;
		if (sceIoPread(fd, s_cr.dst[rg] + off, len, foff) != (int)len) {
			s12_rom_cache_why = "read failed";
			s_cr.failed = 1;
			break;
		}
		__atomic_store_n(&s_cr.ready[k], 1, __ATOMIC_RELEASE);
		if (__atomic_add_fetch(&s_cr.done, 1, __ATOMIC_ACQ_REL) == s_cr.pieces) {
			s12_rom_bg_ms = (uint32_t)((sceKernelGetProcessTimeWide() - s_cr.t0) / 1000);
			s12_rom_all_ready = 1;
		}
	}
	sceIoClose(fd);
	return 0;
}

// wait until bytes [off, off + len) of region rg are read (asks for them first)
void s12_rom_wait(int rg, uint32_t off, uint32_t len)
{
	if (s12_rom_all_ready || !s_cr.ready || !len || off >= s_cr.size[rg])
		return;
	if (off + len > s_cr.size[rg])
		len = s_cr.size[rg] - off;
	SceUID fd = -1;
	for (uint32_t k = s_cr.first[rg] + off / CACHE_PIECE; k <= s_cr.first[rg] + (off + len - 1) / CACHE_PIECE; k++) {
		// a piece no reader has taken yet is read here (the readers run at a low priority on cores that are busy
		// during play); one being read is waited for
		if (!s_cr.ready[k] && __sync_bool_compare_and_swap(&s_cr.taken[k], 0, 1)) {
			int prg;
			uint32_t poff, plen;
			SceOff foff;
			if (fd < 0)
				fd = sceIoOpen(s_cr.path, SCE_O_RDONLY, 0);
			if (fd < 0 || !cache_piece(k, &prg, &poff, &foff, &plen) ||
				sceIoPread(fd, s_cr.dst[prg] + poff, plen, foff) != (int)plen) {
				s12_rom_cache_why = "read failed";
				s_cr.failed = 1;
				break;
			}
			__atomic_store_n(&s_cr.ready[k], 1, __ATOMIC_RELEASE);
			if (__atomic_add_fetch(&s_cr.done, 1, __ATOMIC_ACQ_REL) == s_cr.pieces) {
				s12_rom_bg_ms = (uint32_t)((sceKernelGetProcessTimeWide() - s_cr.t0) / 1000);
				s12_rom_all_ready = 1;
			}
			continue;
		}
		while (!__atomic_load_n(&s_cr.ready[k], __ATOMIC_ACQUIRE) && !s_cr.failed) {
			s_cr.want = (int32_t)k;
			sceKernelDelayThread(200);
		}
	}
	if (fd >= 0)
		sceIoClose(fd);
}

// the background readers have finished (before the ROM is freed)
static void cache_bg_finish(void)
{
	for (int i = 0; i < s_cr_nth; i++) {
		sceKernelWaitThreadEnd(s_cr_th[i], NULL, NULL);
		sceKernelDeleteThread(s_cr_th[i]);
	}
	s_cr_nth = 0;
	free((void *)s_cr.taken);
	free((void *)s_cr.ready);
	free(s_cr.order);
	s_cr.taken = s_cr.ready = NULL;
	s_cr.order = NULL;
	s12_rom_all_ready = 1;
}

static int cache_read_vita(S12Rom *r, const S12Game *g, const char *path, uint64_t key)
{
	const uint64_t t_start = sceKernelGetProcessTimeWide();
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		s12_rom_cache_why = "no cache file";
		return -1;
	}
	CacheHeader hd;
	const int hok = sceIoRead(fd, &hd, sizeof(hd)) == (int)sizeof(hd) && hd.magic == CACHE_MAGIC &&
		hd.version == CACHE_VERSION && hd.key_lo == (uint32_t)key && hd.key_hi == (uint32_t)(key >> 32);
	sceIoClose(fd);
	if (!hok) {
		s12_rom_cache_why = "cache from other ROM files";
		return -1;
	}
	cache_bg_finish();
	memset(r, 0, sizeof(*r));
	r->game = g;
	memset((void *)&s_cr, 0, sizeof(s_cr));
	s_cr.path = path;
	s_cr.want = -1;
	s_cr.t0 = sceKernelGetProcessTimeWide();
	s12_rom_stage_ms[0] = (uint32_t)((s_cr.t0 - t_start) / 1000);
	for (int rg = 0; rg < S12_RGN_COUNT; rg++) {
		r->rgn_size[rg] = hd.rgn_size[rg];
		s_cr.size[rg] = hd.rgn_size[rg];
		s_cr.first[rg] = s_cr.pieces;
		s_cr.pieces += (hd.rgn_size[rg] + CACHE_PIECE - 1) / CACHE_PIECE;
		if (!hd.rgn_size[rg])
			continue;
		r->rgn[rg] = (uint8_t *)malloc(hd.rgn_size[rg]);
		if (!r->rgn[rg]) {
			s12_rom_free(r);
			return -1;
		}
		s_cr.dst[rg] = r->rgn[rg];
	}
	s_cr.order = (uint32_t *)malloc(s_cr.pieces * sizeof(uint32_t) + 1);
	s_cr.taken = (volatile uint8_t *)calloc(s_cr.pieces + 1, 1);
	s_cr.ready = (volatile uint8_t *)calloc(s_cr.pieces + 1, 1);
	if (!s_cr.order || !s_cr.taken || !s_cr.ready) {
		cache_bg_finish();
		s12_rom_free(r);
		return -1;
	}
	s12_rom_stage_ms[1] = (uint32_t)((sceKernelGetProcessTimeWide() - s_cr.t0) / 1000);
	uint32_t n = 0, start_pieces = 0;
	// the regions needed at start first (program, sub CPU), then the sound samples, then the graphics bank
	for (int pass = 0; pass < 3; pass++)
		for (int rg = 0; rg < S12_RGN_COUNT; rg++)
			if ((rg == S12_RGN_C352 ? 1 : rg == S12_RGN_BANK ? 2 : 0) == pass)
				for (uint32_t k = 0; k < (s_cr.size[rg] + CACHE_PIECE - 1) / CACHE_PIECE; k++) {
					s_cr.order[n++] = s_cr.first[rg] + k;
					start_pieces += pass == 0 || !s12_rom_bg_load;
				}
	s12_rom_all_ready = s_cr.pieces == 0;
	// readers on the cores the loading screen leaves free (CPU1 runs this thread, CPU3 is idle until the game)
	for (int i = 0; i < s12_rom_cache_readers && s_cr_nth < 4; i++) {
		const SceUID t = sceKernelCreateThread("rs12_cacheread", cache_reader, 0x60, 0x2000, 0,
			i == 0 ? 0x80000 : SCE_KERNEL_CPU_MASK_USER_1, NULL);
		if (t >= 0 && sceKernelStartThread(t, 0, NULL) >= 0)
			s_cr_th[s_cr_nth++] = t;
		else if (t >= 0)
			sceKernelDeleteThread(t);
	}
	if (!s_cr_nth)
		cache_reader(0, NULL); // (no thread: everything here)
	// wait for the pieces needed at start; the bank goes on in the background at a low priority
	for (;;) {
		uint32_t got = 0;
		for (uint32_t i = 0; i < start_pieces; i++)
			got += s_cr.ready[s_cr.order[i]];
		if (s_progress)
			s_progress((int)((uint64_t)got * 1000 / (start_pieces ? start_pieces : 1)));
		if (got == start_pieces || s_cr.failed)
			break;
		sceKernelDelayThread(2000);
	}
	s12_rom_stage_ms[2] = (uint32_t)((sceKernelGetProcessTimeWide() - s_cr.t0) / 1000) - s12_rom_stage_ms[1];
	for (int i = 0; i < s_cr_nth; i++)
		sceKernelChangeThreadPriority(s_cr_th[i], 0x78);
	if (s_cr.failed) {
		cache_bg_finish();
		s12_rom_free(r);
		return -1;
	}
	return 0;
}
#else
volatile int s12_rom_all_ready = 1;
void s12_rom_wait(int rg, uint32_t off, uint32_t len)
{
	(void)rg;
	(void)off;
	(void)len;
}
#endif

static int cache_read(S12Rom *r, const S12Game *g, const char *path, uint64_t key)
{
#ifdef __vita__
	return cache_read_vita(r, g, path, key);
#endif
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	CacheHeader hd;
	if (fread(&hd, sizeof(hd), 1, f) != 1 || hd.magic != CACHE_MAGIC || hd.version != CACHE_VERSION ||
		hd.key_lo != (uint32_t)key || hd.key_hi != (uint32_t)(key >> 32)) {
		fclose(f);
		return -1;
	}
	memset(r, 0, sizeof(*r));
	r->game = g;
	uint64_t total = 0, done = 0;
	for (int rg = 0; rg < S12_RGN_COUNT; rg++)
		total += hd.rgn_size[rg];
	for (int rg = 0; rg < S12_RGN_COUNT; rg++) {
		r->rgn_size[rg] = hd.rgn_size[rg];
		if (!hd.rgn_size[rg])
			continue;
		r->rgn[rg] = (uint8_t *)malloc(hd.rgn_size[rg]);
		if (!r->rgn[rg])
			goto fail;
		// read in 1MB chunks for the progress bar
		for (uint32_t o = 0; o < hd.rgn_size[rg]; o += 0x100000) {
			const uint32_t n = hd.rgn_size[rg] - o < 0x100000 ? hd.rgn_size[rg] - o : 0x100000;
			if (fread(r->rgn[rg] + o, 1, n, f) != n)
				goto fail;
			done += n;
			if (s_progress)
				s_progress((int)(done * 1000 / (total ? total : 1)));
		}
	}
	fclose(f);
	return 0;
fail:
	fclose(f);
	s12_rom_free(r);
	return -1;
}

static void cache_write(const S12Rom *r, const char *path, uint64_t key)
{
	char tmp[540];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return;
	CacheHeader hd;
	memset(&hd, 0, sizeof(hd));
	hd.magic = CACHE_MAGIC;
	hd.version = CACHE_VERSION;
	hd.key_lo = (uint32_t)key;
	hd.key_hi = (uint32_t)(key >> 32);
	for (int rg = 0; rg < S12_RGN_COUNT; rg++)
		hd.rgn_size[rg] = r->rgn_size[rg];
	strncpy(hd.name, r->game->name, sizeof(hd.name) - 1);
	int ok = fwrite(&hd, sizeof(hd), 1, f) == 1;
	for (int rg = 0; ok && rg < S12_RGN_COUNT; rg++)
		if (r->rgn_size[rg])
			ok = fwrite(r->rgn[rg], 1, r->rgn_size[rg], f) == r->rgn_size[rg];
	fclose(f);
	remove(path);
	if (ok)
		rename(tmp, path);
	else
		remove(tmp);
}

int s12_rom_load_cached(S12Rom *r, const char *roms_dir, const char *cache_dir, const char *name, char *msg, int msg_len,
	void (*progress)(int permille))
{
	const int gi = s12_find_game(name);
	if (gi < 0 || !cache_dir)
		return s12_rom_load(r, roms_dir, name, msg, msg_len);
	const S12Game *g = &s12_games[gi];
	const uint64_t key = cache_key(g, roms_dir);
	if (!key)
		return s12_rom_load(r, roms_dir, name, msg, msg_len); // reports the missing zip
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.bin", cache_dir, name);
	s_progress = progress;
	if (cache_read(r, g, path, key) == 0) {
		s_progress = NULL;
		snprintf(msg, msg_len, "ok (cache)");
		return 0;
	}
	const int ret = s12_rom_load(r, roms_dir, name, msg, msg_len);
	if (ret == 0)
		cache_write(r, path, key);
	if (progress)
		progress(1000);
	s_progress = NULL;
	return ret;
}

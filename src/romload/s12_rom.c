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

void s12_rom_free(S12Rom *r)
{
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
// newlib fread on the Vita reached ~3.5MB/s here; sceIoRead with large blocks reads at card speed
#include <psp2/io/fcntl.h>

static int cache_read_vita(S12Rom *r, const S12Game *g, const char *path, uint64_t key)
{
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	CacheHeader hd;
	if (sceIoRead(fd, &hd, sizeof(hd)) != (int)sizeof(hd) || hd.magic != CACHE_MAGIC || hd.version != CACHE_VERSION ||
		hd.key_lo != (uint32_t)key || hd.key_hi != (uint32_t)(key >> 32)) {
		sceIoClose(fd);
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
		for (uint32_t o = 0; o < hd.rgn_size[rg];) {
			const uint32_t want = hd.rgn_size[rg] - o < 0x800000 ? hd.rgn_size[rg] - o : 0x800000;
			const int n = sceIoRead(fd, r->rgn[rg] + o, want);
			if (n <= 0)
				goto fail;
			o += (uint32_t)n;
			done += (uint32_t)n;
			if (s_progress)
				s_progress((int)(done * 1000 / (total ? total : 1)));
		}
	}
	sceIoClose(fd);
	return 0;
fail:
	sceIoClose(fd);
	s12_rom_free(r);
	return -1;
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

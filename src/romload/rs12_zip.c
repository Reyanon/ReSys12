/*  ReSys12 zip access (junzip). Taken from ReST-V src/core/rstv_zip.c.
    This file is part of ReSys12. GPL-2.0-or-later.
*/
#include "rs12_zip.h"

#include <stdlib.h>
#include <string.h>
#include "zlib.h"

// ---- zip access ------------------------------------------------------------

static int record_cb(JZFile *zip, int idx, JZFileHeader *header, char *filename, void *user)
{
	(void)zip; (void)idx;
	Zip *z = (Zip *)user;
	if (z->count >= MAX_ZIP_ENTRIES)
		return 0;
	ZipEntry *e = &z->e[z->count++];
	const char *base = strrchr(filename, '/');
	base = base ? base + 1 : filename;
	strncpy(e->name, base, sizeof(e->name) - 1);
	e->name[sizeof(e->name) - 1] = 0;
	e->header = *header;
	return 1;
}

int zip_open(Zip *z, const char *path)
{
	memset(z, 0, sizeof(*z));
	z->fp = fopen(path, "rb");
	if (!z->fp)
		return -1;
	z->zip = jzfile_from_stdio_file(z->fp);
	JZEndRecord end;
	if (jzReadEndRecord(z->zip, &end) || jzReadCentralDirectory(z->zip, &end, record_cb, z)) {
		z->zip->close(z->zip);
		z->fp = NULL;
		return -1;
	}
	return 0;
}

void zip_close(Zip *z)
{
	if (z->fp)
		z->zip->close(z->zip);
	z->fp = NULL;
}

const ZipEntry *zip_find(const Zip *z, const char *name, u32 crc)
{
	if (!z->fp)
		return NULL;
	for (int i = 0; i < z->count; i++)
		if (z->e[i].header.crc32 == crc)
			return &z->e[i];
	for (int i = 0; i < z->count; i++)
		if (strcmp(z->e[i].name, name) == 0)
			return &z->e[i];
	return NULL;
}

// Reads an entry; returns malloc'd buffer (caller frees) or NULL
u8 *zip_read(Zip *z, const ZipEntry *e, u32 *size)
{
	JZFileHeader h;
	char name[256];
	if (z->zip->seek(z->zip, e->header.offset, SEEK_SET))
		return NULL;
	if (jzReadLocalFileHeader(z->zip, &h, name, sizeof(name)))
		return NULL;
	u8 *buf = (u8 *)malloc(h.uncompressedSize ? h.uncompressedSize : 1);
	if (!buf)
		return NULL;
	if (jzReadData(z->zip, &h, buf) != Z_OK) {
		free(buf);
		return NULL;
	}
	*size = h.uncompressedSize;
	return buf;
}

// one file from a zip. malloc'd buffer or NULL
u8 *rs12_zip_load(const char *path, const char *name, u32 crc, u32 *size)
{
	Zip z;
	if (zip_open(&z, path) != 0)
		return NULL;
	const ZipEntry *e = zip_find(&z, name, crc);
	u8 *d = e ? zip_read(&z, e, size) : NULL;
	zip_close(&z);
	return d;
}

/*  ReSys12 zip access (junzip). Taken from ReST-V src/core/rstv_zip.h.
    This file is part of ReSys12. GPL-2.0-or-later.
*/
#ifndef RS12_ZIP_H
#define RS12_ZIP_H
#include <stdio.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#include "junzip.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MAX_ZIP_ENTRIES 64

typedef struct {
	char name[128];
	JZFileHeader header;
} ZipEntry;

typedef struct {
	FILE *fp;
	JZFile *zip;
	int count;
	ZipEntry e[MAX_ZIP_ENTRIES];
} Zip;

int zip_open(Zip *z, const char *path);
void zip_close(Zip *z);
const ZipEntry *zip_find(const Zip *z, const char *name, u32 crc); // by CRC, then by name
u8 *zip_read(Zip *z, const ZipEntry *e, u32 *size);                 // malloc'd, caller frees
u8 *rs12_zip_load(const char *path, const char *name, u32 crc, u32 *size);
#ifdef __cplusplus
}
#endif
#endif

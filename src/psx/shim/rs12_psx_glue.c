// ReSys12: glue between the pcsx_rearmed core subset and the System 12 board.
// Replaces the PS1-only parts left out of the build (plugins.c, psxcommon.c, CD-ROM, SPU, SIO/pads, BIOS HLE).
// GPL-2.0-or-later (same as pcsx_rearmed).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "psxcommon.h"
#include "plugins.h"
#include "r3000a.h"
#include "psxbios.h"
#include "cdrom.h"
#include "sio.h"
#include "spu.h"
#include "psxhle.h"
#include "psxhw.h"
#include "psxmem.h"
#include "psxmem_map.h"
#include "gpu.h"
#include "../frontend/plugin_lib.h"

PcsxConfig Config;

static void *pl_mmap(unsigned int size)
{
	return psxMap(0, size, 0, MAP_TAG_VRAM);
}

static void pl_munmap(void *ptr, unsigned int size)
{
	psxUnmap(ptr, size, MAP_TAG_VRAM);
}

extern unsigned int hSyncCount, frame_counter;

// the frontend fills in the pl_vout_* callbacks (same defaults as pcsx_rearmed frontend/libretro.c)
struct rearmed_cbs pl_rearmed_cbs = {
	.mmap             = pl_mmap,
	.munmap           = pl_munmap,
	.gpu_state_change = gpu_state_change,
	.gpu_hcnt         = &hSyncCount,
	.gpu_frame_count  = &frame_counter,
};

// ---- logging ----
void SysPrintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void SysMessage(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

// ---- frame end: called at vblank by psxRcntUpdate; makes psxCpu->Execute() return ----
void EmuUpdate(void)
{
	psxRegs.stop++;
}

// ---- GPU: builtin gpulib (gpu_neon), bound by rs12_psx_bind() ----
GPUinit          GPU_init;
GPUshutdown      GPU_shutdown;
GPUopen          GPU_open;
GPUclose         GPU_close;
GPUreadStatus    GPU_readStatus;
GPUreadData      GPU_readData;
GPUreadDataMem   GPU_readDataMem;
GPUwriteStatus   GPU_writeStatus;
GPUwriteData     GPU_writeData;
GPUwriteDataMem  GPU_writeDataMem;
GPUdmaChain      GPU_dmaChain;
GPUfreeze        GPU_freeze;
GPUvBlank        GPU_vBlank;
GPUgetScreenInfo GPU_getScreenInfo;
void (*rs12_GPUrearmedCallbacks)(const struct rearmed_cbs *);

extern void *const rs12_gpu_entry[];

void rs12_psx_bind(void)
{
	GPU_init = (GPUinit)rs12_gpu_entry[0];
	GPU_shutdown = (GPUshutdown)rs12_gpu_entry[1];
	GPU_open = (GPUopen)rs12_gpu_entry[2];
	GPU_close = (GPUclose)rs12_gpu_entry[3];
	GPU_readStatus = (GPUreadStatus)rs12_gpu_entry[4];
	GPU_readData = (GPUreadData)rs12_gpu_entry[5];
	GPU_readDataMem = (GPUreadDataMem)rs12_gpu_entry[6];
	GPU_writeStatus = (GPUwriteStatus)rs12_gpu_entry[7];
	GPU_writeData = (GPUwriteData)rs12_gpu_entry[8];
	GPU_writeDataMem = (GPUwriteDataMem)rs12_gpu_entry[9];
	GPU_dmaChain = (GPUdmaChain)rs12_gpu_entry[10];
	GPU_freeze = (GPUfreeze)rs12_gpu_entry[11];
	GPU_vBlank = (GPUvBlank)rs12_gpu_entry[12];
	GPU_getScreenInfo = (GPUgetScreenInfo)rs12_gpu_entry[13];
	rs12_GPUrearmedCallbacks = (void (*)(const struct rearmed_cbs *))rs12_gpu_entry[14];
}

// ---- save state I/O (filled by the frontend when states are added) ----
struct PcsxSaveFuncs SaveFuncs;

// ---- PS1-only devices System 12 does not have: CD-ROM, SIO/pads, CD DMA, BIOS HLE ----
void cdrReset(void) {}
void cdrInterrupt(void) {}
void cdrPlayReadInterrupt(void) {}
void cdrLidSeekInterrupt(void) {}
void cdrDmaInterrupt(void) {}
unsigned char cdrRead0(void) { return 0; }
unsigned char cdrRead1(void) { return 0; }
unsigned char cdrRead2(void) { return 0; }
unsigned char cdrRead3(void) { return 0; }
void cdrWrite0(unsigned char v) { (void)v; }
void cdrWrite1(unsigned char v) { (void)v; }
void cdrWrite2(unsigned char v) { (void)v; }
void cdrWrite3(unsigned char v) { (void)v; }
void psxDma3(u32 madr, u32 bcr, u32 chcr)
{
	(void)madr; (void)bcr;
	HW_DMA3_CHCR &= SWAP32(~0x01000000u);
	(void)chcr;
}

// SIO0 with nothing attached, as MAME's psxsio: the status reads TX ready + TX empty, and every byte sent shifts in
// 0xff from the idle (high) RX line, so RX ready comes up. toukon3 polls both and hung with the status left at 0
void sioWrite8(unsigned char value) { (void)value; psxHu16ref(0x1044) |= SWAPu16(0x0002); }
void sioWriteMode16(unsigned short value) { (void)value; }
void sioWriteCtrl16(unsigned short value) { (void)value; }
unsigned char sioRead8(void) { psxHu16ref(0x1044) &= SWAPu16((u16)~0x0002); return 0xff; }
void sioReset(void) { psxHu16ref(0x1044) = SWAPu16(0x0005); }
void sioInterrupt(void) {}
// no PSX BIOS HLE on System 12 (the compiler thread's mixed mode asks only for EXEC_CALLER_HLE)
int psxBiosSoftcallEnded(void) { return 1; }

void spuDelayedIrq(void) {}
void spuUpdate(void) {}

void psxBiosCheckBranch(void) {}
void (*biosA0[256])();
void (**biosB0)();
void (*biosC0[256 + 128])();
static void hle_nop(void) {}
void (* const psxHLEt[hleop_count_])() = { hle_nop };

// ---- SPU: System 12 has none (C352 on the H8 side) ----
static unsigned short CALLBACK spu_read(unsigned long reg, unsigned int cycles) { (void)reg; (void)cycles; return 0; }
static void CALLBACK spu_write(unsigned long reg, unsigned short val, unsigned int cycles) { (void)reg; (void)val; (void)cycles; }
static void CALLBACK spu_dma(unsigned short *p, int n, unsigned int cycles) { (void)p; (void)n; (void)cycles; }
SPUwriteRegister    SPU_writeRegister = spu_write;
SPUreadRegister     SPU_readRegister = spu_read;
SPUwriteDMAMem      SPU_writeDMAMem = spu_dma;
SPUreadDMAMem       SPU_readDMAMem = spu_dma;
SPUasync            SPU_async = NULL;

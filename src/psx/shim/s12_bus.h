// ReSys12: hooks from the pcsx_rearmed core into the System 12 board (src/s12). GPL-2.0-or-later.
#ifndef S12_BUS_H
#define S12_BUS_H

#include <stdint.h>

// 0x1f000000-0x1fffffff in kuseg/kseg0/kseg1 that the core does not handle itself (RAM, scratchpad, 0x1f801000 hw regs)
#define S12_IS_BOARD(a) ((((a) & 0x1f000000u) == 0x1f000000u) && (((a) >> 29) == 0 || ((a) >> 29) == 4 || ((a) >> 29) == 5))

// phys = address & 0x1fffffff
uint8_t s12_read8(uint32_t phys);
uint16_t s12_read16(uint32_t phys);
uint32_t s12_read32(uint32_t phys);
void s12_write8(uint32_t phys, uint32_t v);
void s12_write16(uint32_t phys, uint32_t v);
void s12_write32(uint32_t phys, uint32_t v);

// called from psxMemReset: copy the program flash into psxR, map the ROM bank window
void s12_mem_reset(void);
// called from psxReset after the CPU reset: boot hack etc.
void s12_cpu_reset(void);
// DMA channel 5 (System 12 ROM DMA) and its completion event (PSXINT_S12DMA)
void psxDma5(uint32_t madr, uint32_t bcr, uint32_t chcr);
void s12_dma5_done(void);
// ---- sub CPU side (src/s12/s12_sub.c) ----
// scheduled event (PSXINT_S12): runs the sub CPU up to the main CPU time and reschedules itself
void s12_event(void);
// screen VBLANK edge from the root counters (1 = start, 0 = end): H8 IRQ1 + port B bit 7 (MAME namcos12_sub_irq)
void s12_vblank(int state);
// reset the sub CPU side; called from s12_cpu_reset
void s12_sub_reset(void);

#endif

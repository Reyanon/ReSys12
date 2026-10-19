// Vita clock boost (ARM 444MHz at app start, GPU and bus raised)
#include "platform/clock.h"

#include <psp2/power.h>

#include "platform/log.h"
#include "platform/timer.h"

// Target ARM clock. Acceptance target is 444. For tests, -DRS12_ARM_MHZ=500 etc.
#ifndef RS12_ARM_MHZ
#define RS12_ARM_MHZ 444
#endif

void rs12_clock_boost(void)
{
	int ra = scePowerSetArmClockFrequency(RS12_ARM_MHZ);
	int rb = scePowerSetBusClockFrequency(222);
	int rg = scePowerSetGpuClockFrequency(222);
	int rx = scePowerSetGpuXbarClockFrequency(166);

	RS12_LOG("clock: set(arm %d) ret arm=0x%08x bus=0x%08x gpu=0x%08x xbar=0x%08x", RS12_ARM_MHZ, ra, rb, rg, rx);
	(void)ra; (void)rb; (void)rg; (void)rx;
	rs12_clock_log(1);
}

// Estimate the real ARM clock (MHz). Plugins such as PSVshell hook scePowerGet* and
// may return values that differ from reality, so time a loop with 1 cycle per iteration.
int rs12_clock_measure_arm_mhz(void)
{
	const unsigned iters = 20000000u;
	uint64_t t0 = rs12_time_us();
	unsigned n = iters;
	__asm__ volatile(
		"1:\n\t"
		"subs %0, %0, #1\n\t"
		"bne 1b\n\t"
		: "+r"(n) : : "cc");
	uint64_t dt = rs12_time_us() - t0;
	return dt ? (int)(iters / dt) : 0;
}

// measure != 0 also measures the real clock. Holds the CPU for about 45ms, so only at startup
// (measuring periodically drops a frame each time)
void rs12_clock_log(int measure)
{
	RS12_LOG("clock: arm=%d bus=%d gpu=%d xbar=%d (measured arm~%dMHz)",
		scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
		scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency(),
		measure ? rs12_clock_measure_arm_mhz() : 0);
	(void)measure;
}


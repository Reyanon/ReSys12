#ifndef RS12_PLATFORM_TIMER_H
#define RS12_PLATFORM_TIMER_H

#include <stdint.h>
#include <psp2/kernel/processmgr.h>

// process monotonic time (microseconds)
static inline uint64_t rs12_time_us(void)
{
	return (uint64_t)sceKernelGetProcessTimeWide();
}

#endif

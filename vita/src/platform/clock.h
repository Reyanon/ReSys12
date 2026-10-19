#ifndef RS12_PLATFORM_CLOCK_H
#define RS12_PLATFORM_CLOCK_H

// Raise ARM to 444MHz, bus 222MHz, GPU 222MHz, GPU Xbar 166MHz.
void rs12_clock_boost(void);

// Log current clocks over UDP. If measure is nonzero, also measure the real clock (about 45ms, startup only)
void rs12_clock_log(int measure);

// Estimate the real ARM clock (MHz) with a fixed-cycle loop (about 45-180ms)
int rs12_clock_measure_arm_mhz(void);

#ifdef RS12_CLOCK_SWEEP
// clock step test for diagnostics (-DRS12_CLOCK_SWEEP)
void rs12_clock_sweep(void);
#endif

#endif

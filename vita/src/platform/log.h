#ifndef RS12_PLATFORM_LOG_H
#define RS12_PLATFORM_LOG_H

// Built-in UDP logger
// - Port 18194. Receive on the PC with `nc -lu 18194` or `python tools/udp_log.py`.
// - Target address set at build time with RS12_LOG_HOST. Broadcast if unset.
// - Without RS12_ENABLE_LOG (release build) every call compiles away.

#define RS12_LOG_PORT 18194

#ifdef RS12_ENABLE_LOG

void rs12_log_init(void);
void rs12_log_shutdown(void);
void rs12_log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define RS12_LOG(...) rs12_log_printf(__VA_ARGS__)

#else

#define rs12_log_init()     ((void)0)
#define rs12_log_shutdown() ((void)0)
#define RS12_LOG(...)       ((void)0)

#endif

#endif

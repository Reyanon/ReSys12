// ReSys12: addresses of the builtin gpulib entry points. Kept apart from plugins.h, whose typedefs share the function
// names (pcsx_rearmed frontend/plugin.c does the same lookup by name). GPL-2.0-or-later.
#include <stdint.h>

long GPUinit(void);
long GPUshutdown(void);
long GPUopen(unsigned long *, char *, char *);
long GPUclose(void);
uint32_t GPUreadStatus(void);
uint32_t GPUreadData(void);
void GPUreadDataMem(uint32_t *, int);
void GPUwriteStatus(uint32_t);
void GPUwriteData(uint32_t);
void GPUwriteDataMem(uint32_t *, int);
long GPUdmaChain(uint32_t *, uint32_t, uint32_t *, int32_t *);
long GPUfreeze(uint32_t, void *, uint16_t **);
void GPUvBlank(int, int);
void GPUgetScreenInfo(int *, int *);
struct rearmed_cbs;
void GPUrearmedCallbacks(const struct rearmed_cbs *);

void *const rs12_gpu_entry[] = {
	(void *)GPUinit, (void *)GPUshutdown, (void *)GPUopen, (void *)GPUclose,
	(void *)GPUreadStatus, (void *)GPUreadData, (void *)GPUreadDataMem,
	(void *)GPUwriteStatus, (void *)GPUwriteData, (void *)GPUwriteDataMem,
	(void *)GPUdmaChain, (void *)GPUfreeze, (void *)GPUvBlank, (void *)GPUgetScreenInfo,
	(void *)GPUrearmedCallbacks,
};

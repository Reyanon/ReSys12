// ReSys12: Vita implementation of the small thread API pcsx_rearmed's gpulib uses for threaded rendering
// (frontend/pcsxr-threads.h: sthread/slock/scond, normally libretro-common rthreads). GPL-2.0-or-later.
// The GPU thread is pinned to CPU2 (CPU0 = emulation, CPU1 = sound).
#ifdef __vita__
#include <stdlib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "../frontend/pcsxr-threads.h"

int pcsxr_sthread_core_count = 3;
int rs12_drc_cpu_mask = 0x80000; // CPU3 (shared with the system; CPU1 has the sound and the GPU helper: audio
// underruns there while compiling); the frontend may change it before the core starts

struct sthread {
	SceUID uid;
	void (*func)(void *);
	int drc;
};
struct slock {
	SceKernelLwMutexWork m;
};
struct scond {
	SceKernelLwCondWork c;
	struct slock *bound; // LwCond is created on a mutex; bound on first wait
};

void pcsxr_sthread_init(void)
{
}

// CPU3 is closed to user threads on a stock system (sceKernelCreateThread refuses the mask); a plugin such as CoreUnlocker
// opens it. Asked once: a thread must really run there before anything is planned around it
static int cpu3_probe(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	return 0;
}

int rs12_cpu3_usable(void)
{
	static int s_ok = -1;
	if (s_ok < 0) {
		s_ok = 0;
		const SceUID t = sceKernelCreateThread("rs12_cpu3", cpu3_probe, 0x60, 0x1000, 0, 0x80000, NULL);
		if (t >= 0) {
			SceUInt timeout = 200000;
			if (sceKernelStartThread(t, 0, NULL) >= 0 && sceKernelWaitThreadEnd(t, NULL, &timeout) >= 0) {
				s_ok = 1;
				sceKernelDeleteThread(t);
			}
		}
	}
	return s_ok;
}

// a thread meant for CPU3: there when it can be, else on CPU1
int rs12_cpu3_mask(void)
{
	return rs12_cpu3_usable() ? 0x80000 : SCE_KERNEL_CPU_MASK_USER_1;
}

static int thread_entry(SceSize args, void *argp)
{
	struct sthread *t = *(struct sthread **)argp;
	(void)args;
	t->func(NULL);
	return 0;
}

struct sthread *pcsxr_sthread_create(void (*thread_func)(void *), enum pcsxr_thread_type type)
{
	struct sthread *t = (struct sthread *)calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->func = thread_func;
	t->drc = type == PCSXRT_DRC;
	// GPU rendering: CPU2, just below the audio output thread (64) so sound never starves.
	// Recompiler (compiles blocks while the emulation thread interprets them): CPU1 below the sound thread (0x48) and
	// above the split render helper (0x4A, whose share the GPU thread takes over when the helper is late)
	const int prio = type == PCSXRT_GPU ? 65 : type == PCSXRT_DRC ? 0x49 : 0x60;
	int cpu = type == PCSXRT_GPU ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_CPU_MASK_USER_1;
	if (type == PCSXRT_DRC) { // measurements: the compiler's CPU mask from ux0:data/resys12/drccpu (0x80000 = CPU3)
		extern int rs12_drc_cpu_mask;
		if (rs12_drc_cpu_mask)
			cpu = rs12_drc_cpu_mask;
	}
	t->uid = sceKernelCreateThread("rs12_psxthr", thread_entry, prio, 0x20000, 0, cpu, NULL);
	if (t->uid < 0) {
		free(t);
		return NULL;
	}
	sceKernelStartThread(t->uid, sizeof(t), &t);
	return t;
}

void sthread_join(struct sthread *t)
{
	if (!t)
		return;
	sceKernelWaitThreadEnd(t->uid, NULL, NULL);
	sceKernelDeleteThread(t->uid);
	free(t);
}

struct slock *slock_new(void)
{
	struct slock *l = (struct slock *)calloc(1, sizeof(*l));
	if (l && sceKernelCreateLwMutex(&l->m, "rs12_slock", 0, 0, NULL) < 0) {
		free(l);
		return NULL;
	}
	return l;
}

void slock_free(struct slock *l)
{
	if (!l)
		return;
	sceKernelDeleteLwMutex(&l->m);
	free(l);
}

void slock_lock(struct slock *l)
{
	sceKernelLockLwMutex(&l->m, 1, NULL);
}

void slock_unlock(struct slock *l)
{
	sceKernelUnlockLwMutex(&l->m, 1);
}

struct scond *scond_new(void)
{
	return (struct scond *)calloc(1, sizeof(struct scond));
}

void scond_free(struct scond *c)
{
	if (!c)
		return;
	if (c->bound)
		sceKernelDeleteLwCond(&c->c);
	free(c);
}

void scond_wait(struct scond *c, struct slock *l)
{
	if (!c->bound) {
		sceKernelCreateLwCond(&c->c, "rs12_scond", 0, &l->m, NULL);
		c->bound = l;
	}
	sceKernelWaitLwCond(&c->c, NULL);
}

void scond_signal(struct scond *c)
{
	if (c->bound)
		sceKernelSignalLwCond(&c->c);
}

// ---- ReSys12: split rendering helper (gpu_neon psx_gpu_if.c) ----
// One job at a time per helper: the renderer thread publishes the jobs (kick), renders its own band,
// then finishes each: if a helper has not picked its job up yet, the renderer runs it itself
// ("stolen"), otherwise it waits for it. Both sides spin briefly before sleeping on a semaphore, as
// jobs come every few hundred microseconds while a frame is drawn and a kernel wakeup costs tens of
// them. The first helper runs on CPU1 below the sound thread (0x48), which must keep its priority;
// the second (three bands) on CPU3.
#include <stdint.h>
#include <psp2/kernel/processmgr.h>

#define SPLIT_HELPER_PRIO   0x4A // first helper (CPU1)
#define SPLIT_HELPER2_PRIO  0x4A // second helper (CPU3): below the recompiler (0x49), which holds the code cache
                                 // while it compiles (the emulation interprets meanwhile); a late job is taken
                                 // back by the renderer
#define SPLIT_WORKER_SPIN_US 600 // helper: spin this long after the last job before sleeping
#define SPLIT_MAIN_SPIN_US   300 // renderer: spin this long for the helper before sleeping
#define SPLIT_WORKERS 2

extern volatile uint64_t rs12_gpu_split_busy_us, rs12_gpu_split_busy_c_us; // psx_gpu_if.c

typedef struct {
	void (*job)(int);
	volatile uint32_t req, claim, done;
	volatile int worker_sleeping, main_sleeping;
	volatile uint64_t job_us;
	SceUID go, fin;
} split_worker;
static split_worker s_sw[SPLIT_WORKERS] = { { .go = -1, .fin = -1 }, { .go = -1, .fin = -1 } };
static SceUID s_sw_tid[SPLIT_WORKERS];

uint64_t rs12_split_now_us(void)
{
	return sceKernelGetProcessTimeWide();
}

static inline uint32_t split_load(volatile uint32_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

// run worker w's pending job request req on this thread (claimed by the caller)
static void split_run(int w, uint32_t req, volatile uint64_t *busy)
{
	split_worker *sw = &s_sw[w];
	const uint64_t t0 = sceKernelGetProcessTimeWide();
	sw->job(w);
	const uint64_t dt = sceKernelGetProcessTimeWide() - t0;
	sw->job_us = dt;
	*busy += dt;
	__atomic_store_n(&sw->done, req, __ATOMIC_RELEASE);
	__sync_synchronize();
	if (sw->main_sleeping)
		sceKernelSignalSema(sw->fin, 1);
}

static int split_worker_thread(SceSize args, void *argp)
{
	const int w = *(int *)argp;
	split_worker *sw = &s_sw[w];
	volatile uint64_t *busy = w == 0 ? &rs12_gpu_split_busy_us : &rs12_gpu_split_busy_c_us;
	uint32_t seen = 0;
	(void)args;
	for (;;) {
		uint32_t req = split_load(&sw->req);
		if (req == seen) {
			const uint64_t t0 = sceKernelGetProcessTimeWide();
			unsigned n = 0;
			while ((req = split_load(&sw->req)) == seen) {
				if ((++n & 63) == 0 && sceKernelGetProcessTimeWide() - t0 > SPLIT_WORKER_SPIN_US)
					break;
			}
			if (req == seen) {
				sw->worker_sleeping = 1;
				__sync_synchronize();
				if (split_load(&sw->req) == seen) {
					SceUInt timeout = 100000;
					sceKernelWaitSema(sw->go, 1, &timeout);
				}
				sw->worker_sleeping = 0;
				continue;
			}
		}
		seen = req;
		if (__sync_bool_compare_and_swap(&sw->claim, req - 1, req))
			split_run(w, req, busy);
	}
	return 0;
}

// ReSys12: another thread that is only waiting (the emulation thread waiting for the GPU thread) runs a pending
// helper job itself. Returns 1 if it ran one
volatile uint64_t rs12_gpu_split_help_us;
int rs12_split_worker_help(void)
{
	for (int w = 0; w < SPLIT_WORKERS; w++) {
		split_worker *sw = &s_sw[w];
		const uint32_t req = split_load(&sw->req);
		if (!sw->job || split_load(&sw->done) == req)
			continue;
		if (!__sync_bool_compare_and_swap(&sw->claim, req - 1, req))
			continue;
		split_run(w, req, &rs12_gpu_split_help_us);
		return 1;
	}
	return 0;
}

// worker w on CPU cpu (1 or 3); returns 1 if its thread runs
int rs12_split_worker_start(int w, void (*job)(int), int cpu)
{
	static int idx[SPLIT_WORKERS] = { 0, 1 };
	if (w < 0 || w >= SPLIT_WORKERS)
		return 0;
	split_worker *sw = &s_sw[w];
	sw->job = job;
	sw->go = sceKernelCreateSema("rs12_split_go", 0, 0, 1, NULL);
	sw->fin = sceKernelCreateSema("rs12_split_fin", 0, 0, 1, NULL);
	if (sw->go < 0 || sw->fin < 0)
		return 0;
	SceUID t = sceKernelCreateThread(w ? "rs12_gpusplit2" : "rs12_gpusplit", split_worker_thread,
		w ? SPLIT_HELPER2_PRIO : SPLIT_HELPER_PRIO, 0x20000, 0,
		cpu == 3 ? rs12_cpu3_mask() : SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (t < 0)
		return 0;
	s_sw_tid[w] = t;
	sceKernelStartThread(t, sizeof(int), &idx[w]);
	return 1;
}

// measurements (ux0:data/resys12/splitprio): the priority of the first helper (CPU1, below the sound thread by default)
void rs12_split_helper_prio(int prio)
{
	if (s_sw_tid[0] > 0)
		sceKernelChangeThreadPriority(s_sw_tid[0], prio);
}

void rs12_split_worker_kick(int w)
{
	split_worker *sw = &s_sw[w];
	__atomic_store_n(&sw->req, sw->req + 1, __ATOMIC_RELEASE);
	__sync_synchronize();
	if (sw->worker_sleeping)
		sceKernelSignalSema(sw->go, 1);
}

// returns 0 if the job ran on this thread, 1 if a helper ran it (*job_us = its time)
int rs12_split_worker_finish(int w, uint64_t *job_us)
{
	split_worker *sw = &s_sw[w];
	const uint32_t seq = sw->req;
	if (__sync_bool_compare_and_swap(&sw->claim, seq - 1, seq)) {
		sw->job(w);
		__atomic_store_n(&sw->done, seq, __ATOMIC_RELEASE);
		return 0;
	}
	if (split_load(&sw->done) != seq) {
		const uint64_t t0 = sceKernelGetProcessTimeWide();
		unsigned n = 0;
		while (split_load(&sw->done) != seq) {
			if ((++n & 63) == 0 && sceKernelGetProcessTimeWide() - t0 > SPLIT_MAIN_SPIN_US)
				break;
		}
		if (split_load(&sw->done) != seq) {
			sw->main_sleeping = 1;
			__sync_synchronize();
			while (split_load(&sw->done) != seq) {
				SceUInt timeout = 2000;
				sceKernelWaitSema(sw->fin, 1, &timeout);
			}
			sw->main_sleeping = 0;
		}
	}
	*job_us = sw->job_us;
	return 1;
}
#endif

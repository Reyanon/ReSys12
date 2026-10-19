// ReSys12: Vita implementation of the small thread API pcsx_rearmed's gpulib uses for threaded rendering
// (frontend/pcsxr-threads.h: sthread/slock/scond, normally libretro-common rthreads). GPL-2.0-or-later.
// The GPU thread is pinned to CPU2 (CPU0 = emulation, CPU1 = sound).
#ifdef __vita__
#include <stdlib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "../frontend/pcsxr-threads.h"

int pcsxr_sthread_core_count = 3;

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
	const int cpu = type == PCSXRT_GPU ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_CPU_MASK_USER_1;
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
// One job at a time: the renderer thread publishes a job (kick), renders its own band, then
// finishes: if the helper has not picked the job up yet, the renderer runs it itself ("stolen"),
// otherwise it waits for it. Both sides spin briefly before sleeping on a semaphore, as jobs come
// every few hundred microseconds while a frame is drawn and a kernel wakeup costs tens of them.
// The helper runs on CPU1 below the sound thread (0x48), which must keep its priority.
#include <stdint.h>
#include <psp2/kernel/processmgr.h>

#define SPLIT_HELPER_PRIO   0x4A
#define SPLIT_WORKER_SPIN_US 600 // helper: spin this long after the last job before sleeping
#define SPLIT_MAIN_SPIN_US   300 // renderer: spin this long for the helper before sleeping

extern volatile uint64_t rs12_gpu_split_busy_us; // psx_gpu_if.c

static void (*s_split_job)(void);
static volatile uint32_t s_split_req, s_split_claim, s_split_done;
static volatile int s_split_worker_sleeping, s_split_main_sleeping;
static volatile uint64_t s_split_job_us;
static SceUID s_split_go = -1, s_split_fin = -1;

uint64_t rs12_split_now_us(void)
{
	return sceKernelGetProcessTimeWide();
}

static inline uint32_t split_load(volatile uint32_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static int split_worker_thread(SceSize args, void *argp)
{
	uint32_t seen = 0;
	(void)args;
	(void)argp;
	for (;;) {
		uint32_t req = split_load(&s_split_req);
		if (req == seen) {
			const uint64_t t0 = sceKernelGetProcessTimeWide();
			unsigned n = 0;
			while ((req = split_load(&s_split_req)) == seen) {
				if ((++n & 63) == 0 && sceKernelGetProcessTimeWide() - t0 > SPLIT_WORKER_SPIN_US)
					break;
			}
			if (req == seen) {
				s_split_worker_sleeping = 1;
				__sync_synchronize();
				if (split_load(&s_split_req) == seen) {
					SceUInt timeout = 100000;
					sceKernelWaitSema(s_split_go, 1, &timeout);
				}
				s_split_worker_sleeping = 0;
				continue;
			}
		}
		seen = req;
		if (__sync_bool_compare_and_swap(&s_split_claim, req - 1, req)) {
			const uint64_t t0 = sceKernelGetProcessTimeWide();
			s_split_job();
			const uint64_t dt = sceKernelGetProcessTimeWide() - t0;
			s_split_job_us = dt;
			rs12_gpu_split_busy_us += dt;
			__atomic_store_n(&s_split_done, req, __ATOMIC_RELEASE);
			__sync_synchronize();
			if (s_split_main_sleeping)
				sceKernelSignalSema(s_split_fin, 1);
		}
	}
	return 0;
}

// ReSys12: another thread that is only waiting (the emulation thread waiting for the GPU thread) runs a pending
// helper job itself. Returns 1 if it ran one
volatile uint64_t rs12_gpu_split_help_us;
int rs12_split_worker_help(void)
{
	const uint32_t req = split_load(&s_split_req);
	if (!s_split_job || split_load(&s_split_done) == req)
		return 0;
	if (!__sync_bool_compare_and_swap(&s_split_claim, req - 1, req))
		return 0;
	const uint64_t t0 = sceKernelGetProcessTimeWide();
	s_split_job();
	const uint64_t dt = sceKernelGetProcessTimeWide() - t0;
	s_split_job_us = dt;
	rs12_gpu_split_help_us += dt;
	__atomic_store_n(&s_split_done, req, __ATOMIC_RELEASE);
	__sync_synchronize();
	if (s_split_main_sleeping)
		sceKernelSignalSema(s_split_fin, 1);
	return 1;
}

// returns 1 if the helper thread runs
int rs12_split_worker_start(void (*job)(void))
{
	s_split_job = job;
	s_split_go = sceKernelCreateSema("rs12_split_go", 0, 0, 1, NULL);
	s_split_fin = sceKernelCreateSema("rs12_split_fin", 0, 0, 1, NULL);
	if (s_split_go < 0 || s_split_fin < 0)
		return 0;
	SceUID t = sceKernelCreateThread("rs12_gpusplit", split_worker_thread, SPLIT_HELPER_PRIO, 0x20000, 0,
		SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (t < 0)
		return 0;
	sceKernelStartThread(t, 0, NULL);
	return 1;
}

void rs12_split_worker_kick(void)
{
	__atomic_store_n(&s_split_req, s_split_req + 1, __ATOMIC_RELEASE);
	__sync_synchronize();
	if (s_split_worker_sleeping)
		sceKernelSignalSema(s_split_go, 1);
}

// returns 0 if the job ran on this thread, 1 if the helper ran it (*job_us = its time)
int rs12_split_worker_finish(uint64_t *job_us)
{
	const uint32_t seq = s_split_req;
	if (__sync_bool_compare_and_swap(&s_split_claim, seq - 1, seq)) {
		s_split_job();
		__atomic_store_n(&s_split_done, seq, __ATOMIC_RELEASE);
		return 0;
	}
	if (split_load(&s_split_done) != seq) {
		const uint64_t t0 = sceKernelGetProcessTimeWide();
		unsigned n = 0;
		while (split_load(&s_split_done) != seq) {
			if ((++n & 63) == 0 && sceKernelGetProcessTimeWide() - t0 > SPLIT_MAIN_SPIN_US)
				break;
		}
		if (split_load(&s_split_done) != seq) {
			s_split_main_sleeping = 1;
			__sync_synchronize();
			while (split_load(&s_split_done) != seq) {
				SceUInt timeout = 2000;
				sceKernelWaitSema(s_split_fin, 1, &timeout);
			}
			s_split_main_sleeping = 0;
		}
	}
	*job_us = s_split_job_us;
	return 1;
}
#endif

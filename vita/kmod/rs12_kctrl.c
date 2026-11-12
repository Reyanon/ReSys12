// ReSys12 kernel helper (as in ReST-V): the PS button for the app. The user side cannot see the PS button while the shell
// lock holds it (no sceCtrlPeekBuffer* reported it on the device), and a kernel read made inside the app's syscall
// only saw a long press, so a kernel thread of this module reads the pad every 8 ms, counts presses and double presses
// (home only on a double press), and on request presses PS for the system (button emulation) to go home.
// The module name carries a version (rs12_kctrl_v5): a loaded module stays until reboot, and a new export under an
// old name would never be linked.
#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/io/dirent.h>
#include <psp2kern/kernel/sysmem/uid_puid.h>
#include <taihen.h>

#define POLL_US 8000
#define DOUBLE_POLLS 50 // a second press within 400 ms is a double press
#define QUIET_POLLS 40  // after pressing PS for the system: its own press is not counted

static volatile unsigned int s_presses, s_doubles;
static volatile int s_held, s_run, s_home;
static SceUID s_thread = -1;

// An update into the app. Nothing can write the app folder while the app runs (not even another SELF of the
// same title, a syscall for it or this module's thread: EACCES), so the app unpacks the files, asks for the install
// and exits; this module's thread copies ux0:data/resys12/update/files/<rel> to ux0:app/RSYS00012/<rel> once the folder
// can be written (the user starts the app again). One file: written next to the old one (.new), then put in its place
#define UPD_SRC "ux0:data/resys12/update/files/"
#define UPD_DST "ux0:app/RSYS00012/"
static char s_cbuf[64 * 1024];
static int kcat(char *d, int dsz, const char *a, const char *b, const char *c)
{
	int n = 0;
	for (const char *s = a; *s && n < dsz - 1; s++) d[n++] = *s;
	for (const char *s = b; *s && n < dsz - 1; s++) d[n++] = *s;
	for (const char *s = c; *s && n < dsz - 1; s++) d[n++] = *s;
	d[n] = 0;
	return n < dsz - 1;
}
static int install_rel(const char *rel)
{
	int r = -1;
	static char src[256], dst[256], tmp[260]; // (one install at a time, on the module's thread; its stack is small)
	if (!rel[0] || rel[0] == '/')
		goto out;
	for (int i = 0; rel[i]; i++)
		if (rel[i] == ':' || rel[i] == '\\' || (rel[i] == '.' && rel[i + 1] == '.'))
			goto out;
	if (!kcat(src, sizeof(src), UPD_SRC, rel, "") || !kcat(dst, sizeof(dst), UPD_DST, rel, "") ||
		!kcat(tmp, sizeof(tmp), UPD_DST, rel, ".new"))
		goto out;
	for (int i = (int)sizeof(UPD_DST) - 1; dst[i]; i++) // the folders on the way
		if (dst[i] == '/') {
			dst[i] = 0;
			ksceIoMkdir(dst, 0777);
			dst[i] = '/';
		}
	const SceUID in = ksceIoOpen(src, SCE_O_RDONLY, 0);
	if (in < 0) {
		r = in;
		goto out;
	}
	const SceUID o = ksceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (o < 0) {
		ksceIoClose(in);
		r = o;
		goto out;
	}
	int n, ok = 1;
	while ((n = ksceIoRead(in, s_cbuf, sizeof(s_cbuf))) > 0)
		if (ksceIoWrite(o, s_cbuf, n) != n) {
			ok = 0;
			break;
		}
	ksceIoClose(in);
	ksceIoClose(o);
	if (!ok || n < 0) {
		ksceIoRemove(tmp);
		r = -2;
		goto out;
	}
	ksceIoRemove(dst);
	r = ksceIoRename(tmp, dst);
out:
	return r;
}

static volatile int s_later; // install asked for (the app is exiting)
static void install_dir(const char *rel, int depth, int *bad, int *n) // every file below update/files/<rel>
{
	char dir[256];
	if (!kcat(dir, sizeof(dir), rel[0] ? UPD_SRC : "ux0:data/resys12/update/files", rel, ""))
		return;
	const SceUID d = ksceIoDopen(dir);
	if (d < 0)
		return;
	SceIoDirent e;
	while (ksceIoDread(d, &e) > 0) {
		if (e.d_name[0] == '.')
			continue;
		char r[192];
		if (!kcat(r, sizeof(r), rel, rel[0] ? "/" : "", e.d_name))
			continue;
		if (SCE_S_ISDIR(e.d_stat.st_mode)) {
			if (depth < 6)
				install_dir(r, depth + 1, bad, n);
		} else {
			(*n)++;
			if (install_rel(r) < 0)
				(*bad)++;
		}
	}
	ksceIoDclose(d);
}
static void install_result(int n, int bad, int launched) // ux0:data/resys12/update_result.txt (the app logs it)
{
	char t[64];
	int k = 0;
	const int v[3] = { n, bad, launched };
	for (int i = 0; i < 3; i++) {
		unsigned x = (unsigned)v[i];
		char tmp[12];
		int m = 0;
		do { tmp[m++] = (char)('0' + x % 10); x /= 10; } while (x && m < 11);
		while (m) t[k++] = tmp[--m];
		t[k++] = i < 2 ? ' ' : 10;
	}
	const SceUID f = ksceIoOpen("ux0:data/resys12/update_result.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (f >= 0) {
		ksceIoWrite(f, t, k);
		ksceIoClose(f);
	}
}

static int poll_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	int held = 0, since = 1 << 20, quiet = 0;
	while (s_run) {
		if (s_later && ++s_later % 25 == 0) { // every 200 ms until the app's folder can be written
			if (install_rel("eboot.bin") >= 0) {
				int bad = 0, n = 0;
				install_dir("", 0, &bad, &n);
				install_result(n, bad, 0); // (the user starts the app again: no launch from the kernel)
				s_later = 0;
			} else if (s_later > 25 * 150) { // 30 s: given up
				install_result(0, -1, 0);
				s_later = 0;
			}
		}
		if (s_home) {
			s_home = 0;
			ksceCtrlSetButtonEmulation(0, 0, 0, SCE_CTRL_PSBUTTON, 8);
			quiet = QUIET_POLLS;
		}
		SceCtrlData d;
		d.buttons = 0;
		if (ksceCtrlPeekBufferPositive(0, &d, 1) >= 0) {
			const int ps = (d.buttons & SCE_CTRL_PSBUTTON) != 0;
			if (ps && !held && !quiet) {
				if (since < DOUBLE_POLLS) {
					s_doubles++;
					since = 1 << 20; // (a third press starts over)
				} else {
					s_presses++;
					since = 0;
				}
			}
			held = ps;
			s_held = ps;
		}
		if (since < (1 << 20))
			since++;
		if (quiet)
			quiet--;
		ksceKernelDelayThread(POLL_US);
	}
	return 0;
}

// ---- measurements only: where a thread of the app spends its time. A kernel thread on the same CPU and above it wakes
// every PROF_US; if the sampled thread is running or ready it is debug-suspended for the register read (PC, LR) and
// resumed (suspend 0x1002 / resume 0x0002, as frangarcj/vita-trace); a waiting thread gives the sample (1, 0) ----
#define PROF_US 500
#define PROF_N 4096 // (kernel memory is small: the app reads the samples every few frames)
static uint32_t s_prof_pc[PROF_N]; // (pc, lr) pairs
static volatile int s_prof_n, s_prof_err, s_prof_run;
static volatile SceUID s_prof_thid = -1;
static SceUID s_prof_thread = -1;
static int prof_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	while (s_prof_run) {
		ksceKernelDelayThread(PROF_US);
		const SceUID t = s_prof_thid;
		if (t < 0)
			continue;
		const int n = s_prof_n;
		if (n + 2 > PROF_N)
			continue;
		SceKernelThreadInfo info;
		for (unsigned i = 0; i < sizeof(info); i++)
			((char *)&info)[i] = 0;
		info.size = sizeof(info);
		if (ksceKernelGetThreadInfo(t, &info) < 0)
			continue;
		if (info.status != SCE_THREAD_RUNNING && info.status != SCE_THREAD_READY) {
			s_prof_pc[n] = 1; // waiting
			s_prof_pc[n + 1] = 0;
			s_prof_n = n + 2;
			continue;
		}
		if (ksceKernelDebugSuspendThread(t, 0x1002) < 0)
			continue;
		SceThreadCpuRegisters r;
		const int e = ksceKernelGetThreadCpuRegisters(t, &r);
		if (e >= 0) {
			s_prof_pc[n] = r.entry[0].pc;
			s_prof_pc[n + 1] = r.entry[0].lr;
			s_prof_n = n + 2;
		} else
			s_prof_err = e;
		if (ksceKernelDebugResumeThread(t, 0x0002) < 0) {
			s_prof_err = 0x7E5;
			s_prof_thid = -1; // (could not resume: stop sampling)
		}
	}
	return 0;
}
// start sampling the calling process's thread user_thid (cpu: its CPU mask); -1 stops
int rs12KCtrlProfStart(SceUID user_thid, int cpu_mask)
{
	int state;
	ENTER_SYSCALL(state);
	int r = 0;
	if (user_thid < 0)
		s_prof_thid = -1;
	else {
		const SceUID g = ksceKernelKernelUidForUserUid(ksceKernelGetProcessId(), user_thid);
		s_prof_n = 0;
		s_prof_err = 0;
		if (s_prof_thread < 0) {
			s_prof_run = 1;
			s_prof_thread = ksceKernelCreateThread("rs12_kprof", prof_thread, 0x20, 0x4000, 0, cpu_mask, NULL); // (16KB: 4KB stacks crashed kernel threads)
			if (s_prof_thread >= 0)
				ksceKernelStartThread(s_prof_thread, 0, NULL);
		}
		s_prof_thid = g;
		r = g < 0 ? g : s_prof_thread;
	}
	EXIT_SYSCALL(state);
	return r;
}
// the samples so far (pc, lr pairs) to user memory, at most max words; returns the words copied (the buffer starts over)
// and the last error in *err
int rs12KCtrlProfRead(uint32_t *dst, int max, int *err)
{
	int state;
	ENTER_SYSCALL(state);
	int n = s_prof_n;
	if (n > max)
		n = max;
	ksceKernelMemcpyKernelToUser(dst, s_prof_pc, (SceSize)n * 4);
	const int e = s_prof_err;
	ksceKernelMemcpyKernelToUser(err, &e, 4);
	s_prof_n = 0;
	EXIT_SYSCALL(state);
	return n;
}

// PS button: bits 0-14 single presses, 15-29 double presses (both counted since the module started), bit 30 held now
unsigned int rs12KCtrlPsState(void)
{
	int state;
	ENTER_SYSCALL(state);
	const unsigned int c = (s_presses & 0x7FFF) | ((s_doubles & 0x7FFF) << 15) | (s_held ? 1u << 30 : 0u);
	EXIT_SYSCALL(state);
	return c;
}

// press PS for the system once (the app has let go of its PS lock first): the home screen
int rs12KCtrlGoHome(void)
{
	int state;
	ENTER_SYSCALL(state);
	s_home = 1;
	EXIT_SYSCALL(state);
	return 0;
}

// install the unpacked update once the app has exited (see install_dir)
int rs12KCtrlInstallOnExit(void)
{
	int state;
	ENTER_SYSCALL(state);
	s_later = 1;
	EXIT_SYSCALL(state);
	return 0;
}

// ---- CPU3 for the app's threads. CPU3 is closed to apps by a CPU mask check in SceKernelThreadMgr; the CapUnlocker plugin
// (GrapheneCt, MIT) hooks it at text offset 0x114C to return 0. The same here, but only where that code is the known one:
// the module's NID differs on every firmware, the function need not, and the first 64 bytes hold the function's head and
// both branches making the refusal 0x80028025, with no relocated value. Anything else: no hook, the app uses three cores ----
#define CPU3_OFFSET 0x114c // (Thumb)
#define CPU3_CODE 64
static const uint8_t s_cpu3_ok[][CPU3_CODE] = {
	{ // (read on 3.65)
		0x10, 0xb5, 0xa0, 0xb1, 0x53, 0x6f, 0x5b, 0x69, 0xb3, 0xf1, 0x00, 0x7f, 0x20, 0xd0, 0x0f, 0xd8,
		0xb3, 0xf1, 0x80, 0x7f, 0x3c, 0xd1, 0x30, 0xf0, 0x07, 0x00, 0x08, 0xd0, 0x48, 0xf2, 0x25, 0x00,
		0x11, 0xf4, 0x80, 0x4f, 0xc8, 0xf2, 0x02, 0x00, 0x18, 0xbf, 0x00, 0x20, 0x10, 0xbd, 0x10, 0xbd,
		0xb3, 0xf1, 0x80, 0x6f, 0x02, 0xd0, 0xb3, 0xf1, 0xa0, 0x6f, 0x29, 0xd1, 0x48, 0xf2, 0x25, 0x03,
	},
};
int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset, uintptr_t *addr); // (taiHEN module utils)
static tai_hook_ref_t s_cpu3_ref;
static SceUID s_cpu3_hook = -1;
static int cpu3_check_patched(void) { return 0; } // (the check passes)

int rs12KCtrlCpu3(int on, unsigned int *out)
{
	int state;
	ENTER_SYSCALL(state);
	unsigned int o[4] = { 0, 0, 0, 0 };
	int r = 0;
	tai_module_info_t ti;
	ti.size = sizeof(ti);
	if (taiGetModuleInfoForKernel(KERNEL_PID, "SceKernelThreadMgr", &ti) < 0)
		r = 3;
	else {
		o[0] = ti.module_nid;
		const uint8_t *code = NULL;
		uintptr_t a = 0;
		if (module_get_offset(KERNEL_PID, ti.modid, 0, CPU3_OFFSET, &a) >= 0 && a) {
			code = (const uint8_t *)a;
			if (s_cpu3_hook < 0) // (hooked, the bytes there are taiHEN's)
				for (int i = 0; i < 4; i++) {
					o[1] |= (unsigned int)code[i] << (8 * i);
					o[2] |= (unsigned int)code[4 + i] << (8 * i);
				}
		} else
			r = 4;
		if (r == 0 && on == 1 && s_cpu3_hook < 0) {
			int ok = 0;
			for (unsigned i = 0; i < sizeof(s_cpu3_ok) / sizeof(s_cpu3_ok[0]); i++) {
				int same = 1;
				for (int k = 0; k < CPU3_CODE; k++)
					same &= code[k] == s_cpu3_ok[i][k];
				ok |= same;
			}
			if (!ok)
				r = 2; // (nothing done)
			else {
				const SceUID h = taiHookFunctionOffsetForKernel(KERNEL_PID, &s_cpu3_ref, ti.modid, 0, CPU3_OFFSET, 1,
					cpu3_check_patched);
				if (h >= 0)
					s_cpu3_hook = h;
				else
					r = h;
			}
		} else if (r == 0 && on == 0 && s_cpu3_hook >= 0) {
			taiHookReleaseForKernel(s_cpu3_hook, s_cpu3_ref);
			s_cpu3_hook = -1;
		}
	}
	if (r == 0)
		r = s_cpu3_hook >= 0;
	o[3] = s_cpu3_hook >= 0;
	if (out)
		ksceKernelMemcpyKernelToUser(out, o, sizeof(o));
	EXIT_SYSCALL(state);
	return r;
}

int module_start(SceSize args, void *argp);
int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	s_run = 1;
	s_thread = ksceKernelCreateThread("rs12_kctrl", poll_thread, 0x3C, 0x4000, 0, 0, NULL); // (the install walks folders)
	if (s_thread >= 0)
		ksceKernelStartThread(s_thread, 0, NULL);
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	s_run = 0;
	s_prof_run = 0;
	if (s_cpu3_hook >= 0) {
		taiHookReleaseForKernel(s_cpu3_hook, s_cpu3_ref);
		s_cpu3_hook = -1;
	}
	if (s_prof_thread >= 0) {
		ksceKernelWaitThreadEnd(s_prof_thread, NULL, NULL);
		ksceKernelDeleteThread(s_prof_thread);
	}
	if (s_thread >= 0) {
		ksceKernelWaitThreadEnd(s_thread, NULL, NULL);
		ksceKernelDeleteThread(s_thread);
	}
	return SCE_KERNEL_STOP_SUCCESS;
}

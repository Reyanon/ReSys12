// RePad: Bluetooth controllers for the PS Vita. Kernel plugin (ur0:tai, *KERNEL) or loaded by an app.
// MIT License, see LICENSE.
#include <psp2kern/kernel/aimgr.h>
#include <psp2kern/kernel/modulemgr.h>
#include <taihen.h>

#include "bt.h"

int repad_ctrl_start(void);
void repad_ctrl_stop(void);
int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset, uintptr_t *addr); // (taiHEN module utils)

int repad_tv; // a PS TV (or a Vita set up as one): the system serves DualShock 3 / 4 itself

// RePad hooks functions inside SceCtrl and SceBt by their offsets (firmware 3.60-3.74). Before any hook, each offset is
// checked to be the start of a function (Thumb: PUSH {..., LR}, 16 or 32 bits): on a firmware where it is not, RePad
// does not start at all, and the controls stay as the system has them
int repad_code_ok(SceUID modid, uint32_t off)
{
	uintptr_t a = 0;
	if (module_get_offset(KERNEL_PID, modid, 0, off, &a) < 0 || !a)
		return 0;
	const uint16_t h = *(const volatile uint16_t *)(a & ~(uintptr_t)1);
	const uint16_t h2 = *(const volatile uint16_t *)((a & ~(uintptr_t)1) + 2);
	return (h & 0xFF00) == 0xB500 || (h == 0xE92D && (h2 & 0x4000));
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp)
{
	// another controller plugin already takes the Bluetooth controllers: two would fight over them
	// (REPAD_ALLOW_VITACONTROL: test builds that run beside it anyway)
#ifndef REPAD_ALLOW_VITACONTROL
	tai_module_info_t mi;
	mi.size = sizeof(mi);
	if (taiGetModuleInfoForKernel(KERNEL_PID, "vitacontrol", &mi) >= 0)
		return SCE_KERNEL_START_NO_RESIDENT;
#endif
	repad_tv = ksceSblAimgrIsDolce() > 0;
	if (repad_bt_check() < 0 || repad_ctrl_start() < 0) {
		repad_ctrl_stop();
		return SCE_KERNEL_START_NO_RESIDENT; // (a firmware RePad does not know: nothing hooked)
	}
	if (repad_bt_start() < 0) {
		repad_bt_stop();
		repad_ctrl_stop();
		return SCE_KERNEL_START_FAILED;
	}
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
	repad_bt_stop();
	repad_ctrl_stop();
	return SCE_KERNEL_STOP_SUCCESS;
}

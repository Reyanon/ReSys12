// ReSys12 kernel helper, user side (vita/kmod/rs12_kctrl.c)
#ifndef RS12_KCTRL_H
#define RS12_KCTRL_H
unsigned int rs12KCtrlPsState(void); // bits 0-14 single PS presses, 15-29 double presses, 30 held
int rs12KCtrlGoHome(void);           // press PS for the system once (after the app lets go of its lock)
int rs12KCtrlInstallOnExit(void);    // once the app has exited: ux0:data/resys12/update/files -> the app folder
// CPU3 for the app: on 1 hook, 0 release, -1 read only; out: module NID, the check's first 8 bytes, state (1 hooked).
// Returns 1 hooked, 0 not hooked, 2 not the known code, 3 no module info, 4 no code address, else taiHEN's error
int rs12KCtrlCpu3(int on, unsigned int *out);
#include <stdint.h>
// measurements only: sample the PC / LR of a thread of this app (user_thid -1 stops), read the samples
int rs12KCtrlProfStart(int user_thid, int cpu_mask);
int rs12KCtrlProfRead(uint32_t *dst, int max, int *err);
#endif
